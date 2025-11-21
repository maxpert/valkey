/*
 * RESPB (Redis Binary Protocol) Implementation
 *
 * This file implements the RESPB binary protocol parser and response generators.
 */

#include "server.h"
#include "respb.h"
#include <arpa/inet.h>

/* =============================================================================
 * RESPB Opcode to Command Mapping
 * ============================================================================= */

/* Structure for direct opcode lookup table - O(1) access */
typedef struct respbOpcodeInfo {
    const char *cmd_name;      /* NULL if opcode not valid */
    int8_t fixed_argc;         /* -1 for variable arity, 0 if invalid */
    robj *shared_name;         /* Shared robj for command name (server only) */
    struct redisCommand *cmd;  /* Direct command pointer (server only) */
} respbOpcodeInfo;

/* Direct lookup table - indexed by opcode for O(1) access */
#define RESPB_OPCODE_TABLE_SIZE 0x0400  /* 1024 entries covers all opcodes */
static respbOpcodeInfo opcodeTable[RESPB_OPCODE_TABLE_SIZE];
static int opcodeTableInitialized = 0;

/* Initialize the direct lookup table - called once on first use */
static void initOpcodeTable(void) {
    if (opcodeTableInitialized) return;
    memset(opcodeTable, 0, sizeof(opcodeTable));

    /* String operations */
    opcodeTable[RESPB_OP_GET] = (respbOpcodeInfo){"GET", 2};
    opcodeTable[RESPB_OP_SET] = (respbOpcodeInfo){"SET", -1};
    opcodeTable[RESPB_OP_MGET] = (respbOpcodeInfo){"MGET", -1};
    opcodeTable[RESPB_OP_MSET] = (respbOpcodeInfo){"MSET", -1};
    opcodeTable[RESPB_OP_INCR] = (respbOpcodeInfo){"INCR", 2};
    opcodeTable[RESPB_OP_DECR] = (respbOpcodeInfo){"DECR", 2};
    opcodeTable[RESPB_OP_INCRBY] = (respbOpcodeInfo){"INCRBY", 3};
    opcodeTable[RESPB_OP_DECRBY] = (respbOpcodeInfo){"DECRBY", 3};
    opcodeTable[RESPB_OP_APPEND] = (respbOpcodeInfo){"APPEND", 3};
    opcodeTable[RESPB_OP_STRLEN] = (respbOpcodeInfo){"STRLEN", 2};
    opcodeTable[RESPB_OP_GETRANGE] = (respbOpcodeInfo){"GETRANGE", 4};
    opcodeTable[RESPB_OP_SETRANGE] = (respbOpcodeInfo){"SETRANGE", 4};
    opcodeTable[RESPB_OP_SETNX] = (respbOpcodeInfo){"SETNX", 3};
    opcodeTable[RESPB_OP_SETEX] = (respbOpcodeInfo){"SETEX", 4};
    opcodeTable[RESPB_OP_PSETEX] = (respbOpcodeInfo){"PSETEX", 4};
    opcodeTable[RESPB_OP_GETSET] = (respbOpcodeInfo){"GETSET", 3};
    opcodeTable[RESPB_OP_GETEX] = (respbOpcodeInfo){"GETEX", -1};
    opcodeTable[RESPB_OP_GETDEL] = (respbOpcodeInfo){"GETDEL", 2};
    opcodeTable[RESPB_OP_INCRBYFLOAT] = (respbOpcodeInfo){"INCRBYFLOAT", 3};
    opcodeTable[RESPB_OP_MSETNX] = (respbOpcodeInfo){"MSETNX", -1};

    /* List operations */
    opcodeTable[RESPB_OP_LPUSH] = (respbOpcodeInfo){"LPUSH", -1};
    opcodeTable[RESPB_OP_RPUSH] = (respbOpcodeInfo){"RPUSH", -1};
    opcodeTable[RESPB_OP_LPOP] = (respbOpcodeInfo){"LPOP", -1};
    opcodeTable[RESPB_OP_RPOP] = (respbOpcodeInfo){"RPOP", -1};
    opcodeTable[RESPB_OP_LRANGE] = (respbOpcodeInfo){"LRANGE", 4};
    opcodeTable[RESPB_OP_LLEN] = (respbOpcodeInfo){"LLEN", 2};
    opcodeTable[RESPB_OP_LINDEX] = (respbOpcodeInfo){"LINDEX", 3};
    opcodeTable[RESPB_OP_LSET] = (respbOpcodeInfo){"LSET", 4};
    opcodeTable[RESPB_OP_LREM] = (respbOpcodeInfo){"LREM", 4};
    opcodeTable[RESPB_OP_LTRIM] = (respbOpcodeInfo){"LTRIM", 4};

    /* Set operations */
    opcodeTable[RESPB_OP_SADD] = (respbOpcodeInfo){"SADD", -1};
    opcodeTable[RESPB_OP_SREM] = (respbOpcodeInfo){"SREM", -1};
    opcodeTable[RESPB_OP_SMEMBERS] = (respbOpcodeInfo){"SMEMBERS", 2};
    opcodeTable[RESPB_OP_SISMEMBER] = (respbOpcodeInfo){"SISMEMBER", 3};
    opcodeTable[RESPB_OP_SCARD] = (respbOpcodeInfo){"SCARD", 2};
    opcodeTable[RESPB_OP_SPOP] = (respbOpcodeInfo){"SPOP", -1};
    opcodeTable[RESPB_OP_SRANDMEMBER] = (respbOpcodeInfo){"SRANDMEMBER", -1};

    /* Sorted set operations */
    opcodeTable[RESPB_OP_ZADD] = (respbOpcodeInfo){"ZADD", -1};
    opcodeTable[RESPB_OP_ZREM] = (respbOpcodeInfo){"ZREM", -1};
    opcodeTable[RESPB_OP_ZRANGE] = (respbOpcodeInfo){"ZRANGE", -1};
    opcodeTable[RESPB_OP_ZSCORE] = (respbOpcodeInfo){"ZSCORE", 3};
    opcodeTable[RESPB_OP_ZRANK] = (respbOpcodeInfo){"ZRANK", 3};
    opcodeTable[RESPB_OP_ZCARD] = (respbOpcodeInfo){"ZCARD", 2};
    opcodeTable[RESPB_OP_ZCOUNT] = (respbOpcodeInfo){"ZCOUNT", 4};
    opcodeTable[RESPB_OP_ZINCRBY] = (respbOpcodeInfo){"ZINCRBY", 4};

    /* Hash operations */
    opcodeTable[RESPB_OP_HSET] = (respbOpcodeInfo){"HSET", -1};
    opcodeTable[RESPB_OP_HGET] = (respbOpcodeInfo){"HGET", 3};
    opcodeTable[RESPB_OP_HMSET] = (respbOpcodeInfo){"HMSET", -1};
    opcodeTable[RESPB_OP_HMGET] = (respbOpcodeInfo){"HMGET", -1};
    opcodeTable[RESPB_OP_HGETALL] = (respbOpcodeInfo){"HGETALL", 2};
    opcodeTable[RESPB_OP_HDEL] = (respbOpcodeInfo){"HDEL", -1};
    opcodeTable[RESPB_OP_HEXISTS] = (respbOpcodeInfo){"HEXISTS", 3};
    opcodeTable[RESPB_OP_HLEN] = (respbOpcodeInfo){"HLEN", 2};
    opcodeTable[RESPB_OP_HKEYS] = (respbOpcodeInfo){"HKEYS", 2};
    opcodeTable[RESPB_OP_HVALS] = (respbOpcodeInfo){"HVALS", 2};
    opcodeTable[RESPB_OP_HINCRBY] = (respbOpcodeInfo){"HINCRBY", 4};
    opcodeTable[RESPB_OP_HINCRBYFLOAT] = (respbOpcodeInfo){"HINCRBYFLOAT", 4};
    opcodeTable[RESPB_OP_HSETNX] = (respbOpcodeInfo){"HSETNX", 4};

    /* Key operations */
    opcodeTable[RESPB_OP_DEL] = (respbOpcodeInfo){"DEL", -1};
    opcodeTable[RESPB_OP_EXISTS] = (respbOpcodeInfo){"EXISTS", -1};
    opcodeTable[RESPB_OP_EXPIRE] = (respbOpcodeInfo){"EXPIRE", -1};
    opcodeTable[RESPB_OP_EXPIREAT] = (respbOpcodeInfo){"EXPIREAT", -1};
    opcodeTable[RESPB_OP_PEXPIRE] = (respbOpcodeInfo){"PEXPIRE", -1};
    opcodeTable[RESPB_OP_PEXPIREAT] = (respbOpcodeInfo){"PEXPIREAT", -1};
    opcodeTable[RESPB_OP_TTL] = (respbOpcodeInfo){"TTL", 2};
    opcodeTable[RESPB_OP_PTTL] = (respbOpcodeInfo){"PTTL", 2};
    opcodeTable[RESPB_OP_PERSIST] = (respbOpcodeInfo){"PERSIST", 2};
    opcodeTable[RESPB_OP_TYPE] = (respbOpcodeInfo){"TYPE", 2};
    opcodeTable[RESPB_OP_KEYS] = (respbOpcodeInfo){"KEYS", 2};
    opcodeTable[RESPB_OP_SCAN] = (respbOpcodeInfo){"SCAN", -1};
    opcodeTable[RESPB_OP_RENAME] = (respbOpcodeInfo){"RENAME", 3};
    opcodeTable[RESPB_OP_RENAMENX] = (respbOpcodeInfo){"RENAMENX", 3};
    opcodeTable[RESPB_OP_UNLINK] = (respbOpcodeInfo){"UNLINK", -1};
    opcodeTable[RESPB_OP_TOUCH] = (respbOpcodeInfo){"TOUCH", -1};
    opcodeTable[RESPB_OP_EXPIRETIME] = (respbOpcodeInfo){"EXPIRETIME", 2};
    opcodeTable[RESPB_OP_PEXPIRETIME] = (respbOpcodeInfo){"PEXPIRETIME", 2};

    /* Connection */
    opcodeTable[RESPB_OP_AUTH] = (respbOpcodeInfo){"AUTH", -1};
    opcodeTable[RESPB_OP_PING] = (respbOpcodeInfo){"PING", -1};
    opcodeTable[RESPB_OP_ECHO] = (respbOpcodeInfo){"ECHO", 2};
    opcodeTable[RESPB_OP_QUIT] = (respbOpcodeInfo){"QUIT", 1};
    opcodeTable[RESPB_OP_SELECT] = (respbOpcodeInfo){"SELECT", 2};
    opcodeTable[RESPB_OP_CLIENT] = (respbOpcodeInfo){"CLIENT", -1};
    opcodeTable[RESPB_OP_HELLO] = (respbOpcodeInfo){"HELLO", -1};

    /* Cluster */
    opcodeTable[RESPB_OP_CLUSTER] = (respbOpcodeInfo){"CLUSTER", -1};
    opcodeTable[RESPB_OP_ASKING] = (respbOpcodeInfo){"ASKING", 1};
    opcodeTable[RESPB_OP_READONLY] = (respbOpcodeInfo){"READONLY", 1};
    opcodeTable[RESPB_OP_READWRITE] = (respbOpcodeInfo){"READWRITE", 1};

    /* Server */
    opcodeTable[RESPB_OP_INFO] = (respbOpcodeInfo){"INFO", -1};
    opcodeTable[RESPB_OP_CONFIG] = (respbOpcodeInfo){"CONFIG", -1};
    opcodeTable[RESPB_OP_DBSIZE] = (respbOpcodeInfo){"DBSIZE", 1};
    opcodeTable[RESPB_OP_FLUSHDB] = (respbOpcodeInfo){"FLUSHDB", -1};
    opcodeTable[RESPB_OP_FLUSHALL] = (respbOpcodeInfo){"FLUSHALL", -1};
    opcodeTable[RESPB_OP_TIME] = (respbOpcodeInfo){"TIME", 1};
    opcodeTable[RESPB_OP_COMMAND] = (respbOpcodeInfo){"COMMAND", -1};

    /* Pub/Sub */
    opcodeTable[RESPB_OP_PUBLISH] = (respbOpcodeInfo){"PUBLISH", 3};
    opcodeTable[RESPB_OP_SUBSCRIBE] = (respbOpcodeInfo){"SUBSCRIBE", -1};
    opcodeTable[RESPB_OP_UNSUBSCRIBE] = (respbOpcodeInfo){"UNSUBSCRIBE", -1};
    opcodeTable[RESPB_OP_PSUBSCRIBE] = (respbOpcodeInfo){"PSUBSCRIBE", -1};
    opcodeTable[RESPB_OP_PUNSUBSCRIBE] = (respbOpcodeInfo){"PUNSUBSCRIBE", -1};

    /* Transactions */
    opcodeTable[RESPB_OP_MULTI] = (respbOpcodeInfo){"MULTI", 1};
    opcodeTable[RESPB_OP_EXEC] = (respbOpcodeInfo){"EXEC", 1};
    opcodeTable[RESPB_OP_DISCARD] = (respbOpcodeInfo){"DISCARD", 1};
    opcodeTable[RESPB_OP_WATCH] = (respbOpcodeInfo){"WATCH", -1};
    opcodeTable[RESPB_OP_UNWATCH] = (respbOpcodeInfo){"UNWATCH", 1};

    /* Scripting */
    opcodeTable[RESPB_OP_EVAL] = (respbOpcodeInfo){"EVAL", -1};
    opcodeTable[RESPB_OP_EVALSHA] = (respbOpcodeInfo){"EVALSHA", -1};
    opcodeTable[RESPB_OP_FCALL] = (respbOpcodeInfo){"FCALL", -1};
    opcodeTable[RESPB_OP_FCALL_RO] = (respbOpcodeInfo){"FCALL_RO", -1};

    opcodeTableInitialized = 1;
}

/* Server-only initialization - creates shared objects and resolves commands */
static int serverInitDone = 0;
void respbInitServer(void) {
    if (serverInitDone) return;
    initOpcodeTable();

    for (int i = 0; i < RESPB_OPCODE_TABLE_SIZE; i++) {
        if (opcodeTable[i].cmd_name) {
            /* Create shared robj for command name */
            opcodeTable[i].shared_name = createStringObject(
                opcodeTable[i].cmd_name, strlen(opcodeTable[i].cmd_name));
            /* Lookup and cache the command pointer */
            opcodeTable[i].cmd = lookupCommandByCString((char*)opcodeTable[i].cmd_name);
        }
    }
    serverInitDone = 1;
}

/* Get cached command for opcode - O(1), no string lookup */
struct redisCommand *respbOpcodeCommand(uint16_t opcode) {
    if (opcode >= RESPB_OPCODE_TABLE_SIZE) return NULL;
    return opcodeTable[opcode].cmd;
}

/* Get shared command name robj for opcode - O(1), no allocation */
robj *respbOpcodeSharedName(uint16_t opcode) {
    if (opcode >= RESPB_OPCODE_TABLE_SIZE) return NULL;
    return opcodeTable[opcode].shared_name;
}

/* Find command name for opcode - O(1) direct lookup */
const char *respbOpcodeToCommand(uint16_t opcode) {
    initOpcodeTable();
    if (opcode >= RESPB_OPCODE_TABLE_SIZE) return NULL;
    return opcodeTable[opcode].cmd_name;
}

/* Get fixed argc for opcode - O(1) direct lookup */
int respbOpcodeFixedArgc(uint16_t opcode) {
    initOpcodeTable();
    if (opcode >= RESPB_OPCODE_TABLE_SIZE) return 0;
    return opcodeTable[opcode].fixed_argc;
}

/* =============================================================================
 * RESPB Request Parsing
 * ============================================================================= */

/* Read a 2-byte length prefix from buffer */
static inline int respbReadLen16(const char *buf, size_t buflen, size_t *pos, uint16_t *len) {
    if (*pos + 2 > buflen) return C_ERR;
    *len = ntohs(*(uint16_t *)(buf + *pos));
    *pos += 2;
    return C_OK;
}

/* Read a 4-byte length prefix from buffer */
static inline int respbReadLen32(const char *buf, size_t buflen, size_t *pos, uint32_t *len) {
    if (*pos + 4 > buflen) return C_ERR;
    *len = ntohl(*(uint32_t *)(buf + *pos));
    *pos += 4;
    return C_OK;
}

/* Read a bulk string with 2-byte length prefix (for potential future use) */
__attribute__((unused))
static int respbReadBulk16(client *c, const char *buf, size_t buflen, size_t *pos, robj **obj) {
    (void)c;  /* Unused */
    uint16_t len;
    if (respbReadLen16(buf, buflen, pos, &len) == C_ERR) return C_ERR;
    if (*pos + len > buflen) return C_ERR;

    *obj = createStringObject(buf + *pos, len);
    *pos += len;
    return C_OK;
}

/* Read a bulk string with 4-byte length prefix (for potential future use) */
__attribute__((unused))
static int respbReadBulk32(client *c, const char *buf, size_t buflen, size_t *pos, robj **obj) {
    (void)c;  /* Unused */
    uint32_t len;
    if (respbReadLen32(buf, buflen, pos, &len) == C_ERR) return C_ERR;
    if (*pos + len > buflen) return C_ERR;

    *obj = createStringObject(buf + *pos, len);
    *pos += len;
    return C_OK;
}

/* Parse a RESPB command from the query buffer.
 * Returns:
 *  - READ_FLAGS_PARSING_COMPLETED on success
 *  - 0 if more data needed
 *  - READ_FLAGS_ERROR_* on error
 */
int parseRespbBuffer(client *c) {
    size_t qblen = sdslen(c->querybuf);
    size_t pos = c->qb_pos;
    const char *buf = c->querybuf;

    /* Need at least header size */
    if (qblen - pos < RESPB_HEADER_SIZE) return 0;

    /* Read header */
    uint16_t opcode = ntohs(*(uint16_t *)(buf + pos));
    uint16_t mux_id = ntohs(*(uint16_t *)(buf + pos + 2));
    pos += RESPB_HEADER_SIZE;

    /* Store mux_id and opcode for response */
    c->respb_mux_id = mux_id;
    c->respb_opcode = opcode;

    /* Handle RESP passthrough */
    if (IS_RESPB_PASSTHROUGH_OPCODE(opcode)) {
        if (qblen - pos < 4) return 0;  /* Need RESP length */
        uint32_t resp_len = ntohl(*(uint32_t *)(buf + pos));
        pos += 4;

        if (qblen - pos < resp_len) return 0;  /* Need full RESP data */

        /* Parse the embedded RESP data using standard parser */
        /* For now, we'll handle passthrough by setting reqtype back to RESP */
        c->qb_pos = pos;
        c->reqtype = PROTO_REQ_MULTIBULK;
        return 0;  /* Let standard parser handle it */
    }

    /* Lookup command */
    const char *cmd_name = respbOpcodeToCommand(opcode);
    if (!cmd_name) {
        c->read_flags |= READ_FLAGS_ERROR_BIG_INLINE_REQUEST;  /* Reuse error flag */
        return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
    }

    /* Get expected argc */
    int fixed_argc = respbOpcodeFixedArgc(opcode);
    int argc;
    size_t start_pos = pos;

    if (fixed_argc > 0) {
        argc = fixed_argc;
    } else {
        /* Variable arity - read 2-byte count */
        if (qblen - pos < 2) return 0;
        uint16_t count;
        if (respbReadLen16(buf, qblen, &pos, &count) == C_ERR) return 0;
        argc = count + 1;  /* +1 for command name */
    }

    /* Allocate argv */
    if (c->argv) zfree(c->argv);
    c->argv_len = argc;
    c->argv = zmalloc(sizeof(robj *) * argc);
    c->argv_len_sum = 0;

    /* First argument is the shared command name (no allocation) */
    robj *shared_name = respbOpcodeSharedName(opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        c->argv[0] = shared_name;
        c->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        /* Fallback for uninitialized server */
        c->argv[0] = createStringObject(cmd_name, strlen(cmd_name));
        c->argv_len_sum += strlen(cmd_name);
    }
    c->argc = 1;

    /* Set command directly - bypass string lookup in prepareCommand */
    c->parsed_cmd = respbOpcodeCommand(opcode);

    /* Read remaining arguments */
    for (int i = 1; i < argc; i++) {
        if (qblen - pos < 2) {
            /* Not enough data - need to wait */
            /* Free partially allocated argv */
            for (int j = 0; j < c->argc; j++) {
                decrRefCount(c->argv[j]);
            }
            c->argc = 0;
            return 0;
        }

        /* Check if this is a large string (first byte indicates) */
        uint16_t len16;
        if (respbReadLen16(buf, qblen, &pos, &len16) == C_ERR) {
            for (int j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
            c->argc = 0;
            return 0;
        }

        /* Handle large strings with 0xFFFF marker */
        size_t len;
        if (len16 == 0xFFFF) {
            if (qblen - pos < 4) {
                for (int j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
                c->argc = 0;
                return 0;
            }
            uint32_t len32;
            if (respbReadLen32(buf, qblen, &pos, &len32) == C_ERR) {
                for (int j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
                c->argc = 0;
                return 0;
            }
            len = len32;
        } else {
            len = len16;
        }

        if (qblen - pos < len) {
            for (int j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
            c->argc = 0;
            return 0;
        }

        c->argv[i] = createStringObject(buf + pos, len);
        c->argv_len_sum += len;
        c->argc++;
        pos += len;
    }

    /* Update query buffer position */
    c->qb_pos = pos;
    c->net_input_bytes_curr_cmd = pos - start_pos + RESPB_HEADER_SIZE;
    c->read_flags |= READ_FLAGS_PARSING_COMPLETED;
    c->reqtype = 0;

    return READ_FLAGS_PARSING_COMPLETED;
}

/* =============================================================================
 * RESPB Response Functions
 * ============================================================================= */

/* Add RESPB response header to output buffer */
static void addRespbResponseHeader(client *c, uint16_t resp_opcode) {
    char header[4];
    *(uint16_t *)header = htons(resp_opcode);
    *(uint16_t *)(header + 2) = htons(c->respb_mux_id);
    addReplyProto(c, header, 4);
}

/* Reply with OK status in RESPB format */
void addReplyRespbOK(client *c) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_OK);
    } else {
        addReply(c, shared.ok);
    }
}

/* Reply with error in RESPB format */
void addReplyRespbError(client *c, const char *err) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_ERROR);
        uint16_t len = strlen(err);
        char lenbuf[2];
        *(uint16_t *)lenbuf = htons(len);
        addReplyProto(c, lenbuf, 2);
        addReplyProto(c, err, len);
    } else {
        addReplyError(c, err);
    }
}

/* Reply with integer in RESPB format */
void addReplyRespbLongLong(client *c, long long ll) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_INTEGER);
        char buf[8];
        int64_t val = ll;
        /* Network byte order for 64-bit */
        buf[0] = (val >> 56) & 0xFF;
        buf[1] = (val >> 48) & 0xFF;
        buf[2] = (val >> 40) & 0xFF;
        buf[3] = (val >> 32) & 0xFF;
        buf[4] = (val >> 24) & 0xFF;
        buf[5] = (val >> 16) & 0xFF;
        buf[6] = (val >> 8) & 0xFF;
        buf[7] = val & 0xFF;
        addReplyProto(c, buf, 8);
    } else {
        addReplyLongLong(c, ll);
    }
}

/* Reply with null in RESPB format */
void addReplyRespbNull(client *c) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_NULL);
    } else {
        addReplyNull(c);
    }
}

/* Reply with bulk string in RESPB format */
void addReplyRespbBulkCBuffer(client *c, const void *p, size_t len) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_BULK);
        if (len < 0xFFFF) {
            char lenbuf[2];
            *(uint16_t *)lenbuf = htons((uint16_t)len);
            addReplyProto(c, lenbuf, 2);
        } else {
            /* Large string marker + 4-byte length */
            char lenbuf[6];
            *(uint16_t *)lenbuf = htons(0xFFFF);
            *(uint32_t *)(lenbuf + 2) = htonl((uint32_t)len);
            addReplyProto(c, lenbuf, 6);
        }
        addReplyProto(c, p, len);
    } else {
        addReplyBulkCBuffer(c, p, len);
    }
}

/* Reply with bulk string from robj in RESPB format */
void addReplyRespbBulk(client *c, robj *obj) {
    if (c->resp == PROTO_RESPB) {
        sds s = obj->ptr;
        addReplyRespbBulkCBuffer(c, s, sdslen(s));
    } else {
        addReplyBulk(c, obj);
    }
}

/* Start array reply in RESPB format */
void addReplyRespbArrayLen(client *c, long length) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_ARRAY);
        if (length < 0xFFFF) {
            char lenbuf[2];
            *(uint16_t *)lenbuf = htons((uint16_t)length);
            addReplyProto(c, lenbuf, 2);
        } else {
            char lenbuf[6];
            *(uint16_t *)lenbuf = htons(0xFFFF);
            *(uint32_t *)(lenbuf + 2) = htonl((uint32_t)length);
            addReplyProto(c, lenbuf, 6);
        }
    } else {
        addReplyArrayLen(c, length);
    }
}

/* Start map reply in RESPB format */
void addReplyRespbMapLen(client *c, long length) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_MAP);
        if (length < 0xFFFF) {
            char lenbuf[2];
            *(uint16_t *)lenbuf = htons((uint16_t)length);
            addReplyProto(c, lenbuf, 2);
        } else {
            char lenbuf[6];
            *(uint16_t *)lenbuf = htons(0xFFFF);
            *(uint32_t *)(lenbuf + 2) = htonl((uint32_t)length);
            addReplyProto(c, lenbuf, 6);
        }
    } else {
        addReplyMapLen(c, length);
    }
}

/* Reply with boolean in RESPB format */
void addReplyRespbBool(client *c, int b) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_BOOL);
        char val = b ? 1 : 0;
        addReplyProto(c, &val, 1);
    } else {
        addReplyBool(c, b);
    }
}

/* Reply with double in RESPB format */
void addReplyRespbDouble(client *c, double d) {
    if (c->resp == PROTO_RESPB) {
        addRespbResponseHeader(c, RESPB_RESP_DOUBLE);
        char buf[8];
        uint64_t *ptr = (uint64_t *)&d;
        uint64_t val = *ptr;
        /* Network byte order */
        buf[0] = (val >> 56) & 0xFF;
        buf[1] = (val >> 48) & 0xFF;
        buf[2] = (val >> 40) & 0xFF;
        buf[3] = (val >> 32) & 0xFF;
        buf[4] = (val >> 24) & 0xFF;
        buf[5] = (val >> 16) & 0xFF;
        buf[6] = (val >> 8) & 0xFF;
        buf[7] = val & 0xFF;
        addReplyProto(c, buf, 8);
    } else {
        addReplyDouble(c, d);
    }
}

/* Add bulk element to array (no header, just length + data) */
void addReplyRespbBulkElement(client *c, const void *p, size_t len) {
    if (c->resp == PROTO_RESPB) {
        if (len < 0xFFFF) {
            char lenbuf[2];
            *(uint16_t *)lenbuf = htons((uint16_t)len);
            addReplyProto(c, lenbuf, 2);
        } else {
            char lenbuf[6];
            *(uint16_t *)lenbuf = htons(0xFFFF);
            *(uint32_t *)(lenbuf + 2) = htonl((uint32_t)len);
            addReplyProto(c, lenbuf, 6);
        }
        addReplyProto(c, p, len);
    } else {
        addReplyBulkCBuffer(c, p, len);
    }
}

/* Add null element to array */
void addReplyRespbNullElement(client *c) {
    if (c->resp == PROTO_RESPB) {
        /* Use special length marker 0xFFFE for null */
        char marker[2];
        *(uint16_t *)marker = htons(0xFFFE);
        addReplyProto(c, marker, 2);
    } else {
        addReplyNull(c);
    }
}

/* Add integer element to array */
void addReplyRespbLongLongElement(client *c, long long ll) {
    if (c->resp == PROTO_RESPB) {
        /* Use special marker 0xFFFD followed by 8-byte int */
        char buf[10];
        *(uint16_t *)buf = htons(0xFFFD);
        int64_t val = ll;
        buf[2] = (val >> 56) & 0xFF;
        buf[3] = (val >> 48) & 0xFF;
        buf[4] = (val >> 40) & 0xFF;
        buf[5] = (val >> 32) & 0xFF;
        buf[6] = (val >> 24) & 0xFF;
        buf[7] = (val >> 16) & 0xFF;
        buf[8] = (val >> 8) & 0xFF;
        buf[9] = val & 0xFF;
        addReplyProto(c, buf, 10);
    } else {
        addReplyLongLong(c, ll);
    }
}

/* =============================================================================
 * Client-side RESPB Command Formatting (for benchmark/cli)
 * ============================================================================= */

/* Command name to opcode mapping for client-side formatting */
static uint16_t respbCommandToOpcode(const char *cmd) {
    /* Fast path for common commands */
    if (strcasecmp(cmd, "GET") == 0) return RESPB_OP_GET;
    if (strcasecmp(cmd, "SET") == 0) return RESPB_OP_SET;
    if (strcasecmp(cmd, "MGET") == 0) return RESPB_OP_MGET;
    if (strcasecmp(cmd, "MSET") == 0) return RESPB_OP_MSET;
    if (strcasecmp(cmd, "INCR") == 0) return RESPB_OP_INCR;
    if (strcasecmp(cmd, "DECR") == 0) return RESPB_OP_DECR;
    if (strcasecmp(cmd, "LPUSH") == 0) return RESPB_OP_LPUSH;
    if (strcasecmp(cmd, "RPUSH") == 0) return RESPB_OP_RPUSH;
    if (strcasecmp(cmd, "LPOP") == 0) return RESPB_OP_LPOP;
    if (strcasecmp(cmd, "RPOP") == 0) return RESPB_OP_RPOP;
    if (strcasecmp(cmd, "SADD") == 0) return RESPB_OP_SADD;
    if (strcasecmp(cmd, "HSET") == 0) return RESPB_OP_HSET;
    if (strcasecmp(cmd, "HGET") == 0) return RESPB_OP_HGET;
    if (strcasecmp(cmd, "DEL") == 0) return RESPB_OP_DEL;
    if (strcasecmp(cmd, "PING") == 0) return RESPB_OP_PING;
    if (strcasecmp(cmd, "EXPIRE") == 0) return RESPB_OP_EXPIRE;
    if (strcasecmp(cmd, "TTL") == 0) return RESPB_OP_TTL;
    if (strcasecmp(cmd, "EXISTS") == 0) return RESPB_OP_EXISTS;
    if (strcasecmp(cmd, "ZADD") == 0) return RESPB_OP_ZADD;
    if (strcasecmp(cmd, "ZRANGE") == 0) return RESPB_OP_ZRANGE;
    if (strcasecmp(cmd, "LRANGE") == 0) return RESPB_OP_LRANGE;
    /* Add more as needed */
    return 0xFFFF;  /* Unknown - use passthrough */
}

/* Format a command in RESPB format.
 * Returns newly allocated buffer with RESPB command, sets *len to buffer length.
 * Caller must zfree the returned buffer.
 */
char *respbFormatCommand(size_t *len, int argc, const char **argv, const size_t *argvlen) {
    if (argc < 1) return NULL;

    uint16_t opcode = respbCommandToOpcode(argv[0]);
    int fixed_argc = respbOpcodeFixedArgc(opcode);

    /* Calculate buffer size needed */
    size_t bufsize = RESPB_HEADER_SIZE;  /* 4-byte header */

    /* Variable arity needs 2-byte count */
    if (fixed_argc < 0) {
        bufsize += 2;  /* argc count (excluding command name) */
    }

    /* Add size for each argument (skip command name at argv[0]) */
    for (int i = 1; i < argc; i++) {
        size_t arglen = argvlen ? argvlen[i] : strlen(argv[i]);
        if (arglen < 0xFFFF) {
            bufsize += 2 + arglen;  /* 2-byte length + data */
        } else {
            bufsize += 6 + arglen;  /* 0xFFFF marker + 4-byte length + data */
        }
    }

    /* Allocate buffer */
    char *buf = zmalloc(bufsize);
    if (!buf) return NULL;

    char *p = buf;

    /* Write header */
    *(uint16_t *)p = htons(opcode);
    p += 2;
    *(uint16_t *)p = htons(0);  /* mux_id = 0 for now */
    p += 2;

    /* Write argc for variable arity commands */
    if (fixed_argc < 0) {
        *(uint16_t *)p = htons((uint16_t)(argc - 1));  /* exclude command name */
        p += 2;
    }

    /* Write arguments (skip command name) */
    for (int i = 1; i < argc; i++) {
        size_t arglen = argvlen ? argvlen[i] : strlen(argv[i]);
        if (arglen < 0xFFFF) {
            *(uint16_t *)p = htons((uint16_t)arglen);
            p += 2;
        } else {
            *(uint16_t *)p = htons(0xFFFF);
            p += 2;
            *(uint32_t *)p = htonl((uint32_t)arglen);
            p += 4;
        }
        memcpy(p, argv[i], arglen);
        p += arglen;
    }

    *len = p - buf;
    return buf;
}

/* Parse RESPB response from buffer.
 * Returns:
 *   > 0: Number of bytes consumed (complete response)
 *   0: Need more data
 *   -1: Parse error
 *
 * On success, sets *type to response type, *data to response data pointer,
 * *datalen to data length.
 */
int respbParseResponse(const char *buf, size_t buflen,
                       uint16_t *type, uint16_t *mux_id,
                       const char **data, size_t *datalen) {
    if (buflen < 4) return 0;  /* Need at least header */

    uint16_t resp_opcode = ntohs(*(uint16_t *)buf);
    *mux_id = ntohs(*(uint16_t *)(buf + 2));
    *type = resp_opcode;

    size_t pos = 4;

    switch (resp_opcode) {
    case RESPB_RESP_OK:
        /* No payload */
        *data = NULL;
        *datalen = 0;
        return pos;

    case RESPB_RESP_NULL:
        /* No payload */
        *data = NULL;
        *datalen = 0;
        return pos;

    case RESPB_RESP_INTEGER:
        /* 8-byte integer */
        if (buflen < pos + 8) return 0;
        *data = buf + pos;
        *datalen = 8;
        return pos + 8;

    case RESPB_RESP_BOOL:
        /* 1-byte boolean */
        if (buflen < pos + 1) return 0;
        *data = buf + pos;
        *datalen = 1;
        return pos + 1;

    case RESPB_RESP_DOUBLE:
        /* 8-byte double */
        if (buflen < pos + 8) return 0;
        *data = buf + pos;
        *datalen = 8;
        return pos + 8;

    case RESPB_RESP_ERROR:
    case RESPB_RESP_BULK: {
        /* Length-prefixed string */
        if (buflen < pos + 2) return 0;
        uint16_t len16 = ntohs(*(uint16_t *)(buf + pos));
        pos += 2;

        size_t len;
        if (len16 == 0xFFFF) {
            /* Large string */
            if (buflen < pos + 4) return 0;
            len = ntohl(*(uint32_t *)(buf + pos));
            pos += 4;
        } else if (len16 == 0xFFFE) {
            /* Null marker */
            *data = NULL;
            *datalen = 0;
            return pos;
        } else {
            len = len16;
        }

        if (buflen < pos + len) return 0;
        *data = buf + pos;
        *datalen = len;
        return pos + len;
    }

    case RESPB_RESP_ARRAY:
    case RESPB_RESP_MAP: {
        /* For benchmark purposes, we just need to consume the response.
         * Elements may have full headers (opcode+mux_id) or just length+data. */
        if (buflen < pos + 2) return 0;
        uint16_t count16 = ntohs(*(uint16_t *)(buf + pos));
        pos += 2;

        size_t count;
        if (count16 == 0xFFFF) {
            if (buflen < pos + 4) return 0;
            count = ntohl(*(uint32_t *)(buf + pos));
            pos += 4;
        } else {
            count = count16;
        }

        /* For MAP, count is number of pairs, so double it for elements */
        if (resp_opcode == RESPB_RESP_MAP) count *= 2;

        /* Consume elements - they may be nested RESPB responses */
        for (size_t i = 0; i < count; i++) {
            if (buflen < pos + 2) return 0;
            uint16_t elem_header = ntohs(*(uint16_t *)(buf + pos));

            /* Check if element starts with a response opcode (0x80xx) */
            if ((elem_header & 0x8000) != 0) {
                /* Nested RESPB response - recursively parse it */
                uint16_t nested_type, nested_mux;
                const char *nested_data;
                size_t nested_datalen;
                int nested_consumed = respbParseResponse(buf + pos, buflen - pos,
                                                         &nested_type, &nested_mux,
                                                         &nested_data, &nested_datalen);
                if (nested_consumed <= 0) return 0;
                pos += nested_consumed;
            } else if (elem_header == 0xFFFF) {
                /* Large element */
                pos += 2;
                if (buflen < pos + 4) return 0;
                size_t elen = ntohl(*(uint32_t *)(buf + pos));
                pos += 4;
                if (buflen < pos + elen) return 0;
                pos += elen;
            } else if (elem_header == 0xFFFE) {
                /* Null element - no data */
                pos += 2;
            } else if (elem_header == 0xFFFD) {
                /* Integer element - 8 bytes */
                pos += 2;
                if (buflen < pos + 8) return 0;
                pos += 8;
            } else {
                /* Regular length-prefixed element */
                pos += 2;
                if (buflen < pos + elem_header) return 0;
                pos += elem_header;
            }
        }

        *data = buf + 6;  /* Point to count */
        *datalen = pos - 6;
        return pos;
    }

    default:
        return -1;  /* Unknown response type */
    }
}

/* Get integer value from RESPB integer response data */
long long respbGetInteger(const char *data) {
    int64_t val = 0;
    val = ((int64_t)(unsigned char)data[0] << 56) |
          ((int64_t)(unsigned char)data[1] << 48) |
          ((int64_t)(unsigned char)data[2] << 40) |
          ((int64_t)(unsigned char)data[3] << 32) |
          ((int64_t)(unsigned char)data[4] << 24) |
          ((int64_t)(unsigned char)data[5] << 16) |
          ((int64_t)(unsigned char)data[6] << 8) |
          ((int64_t)(unsigned char)data[7]);
    return val;
}
