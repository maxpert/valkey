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

/* Structure to map RESPB opcodes to command names */
typedef struct respbOpcodeMap {
    uint16_t opcode;
    const char *cmd_name;
    int fixed_argc;  /* -1 for variable arity */
} respbOpcodeMap;

/* Opcode mapping table - sorted by opcode for binary search */
static const respbOpcodeMap opcodeTable[] = {
    /* String operations */
    {RESPB_OP_GET, "GET", 2},
    {RESPB_OP_SET, "SET", -1},  /* Variable: SET key value [EX|PX|EXAT|PXAT] */
    {RESPB_OP_MGET, "MGET", -1},
    {RESPB_OP_MSET, "MSET", -1},
    {RESPB_OP_INCR, "INCR", 2},
    {RESPB_OP_DECR, "DECR", 2},
    {RESPB_OP_INCRBY, "INCRBY", 3},
    {RESPB_OP_DECRBY, "DECRBY", 3},
    {RESPB_OP_APPEND, "APPEND", 3},
    {RESPB_OP_STRLEN, "STRLEN", 2},
    {RESPB_OP_GETRANGE, "GETRANGE", 4},
    {RESPB_OP_SETRANGE, "SETRANGE", 4},
    {RESPB_OP_SETNX, "SETNX", 3},
    {RESPB_OP_SETEX, "SETEX", 4},
    {RESPB_OP_PSETEX, "PSETEX", 4},
    {RESPB_OP_GETSET, "GETSET", 3},
    {RESPB_OP_GETEX, "GETEX", -1},
    {RESPB_OP_GETDEL, "GETDEL", 2},
    {RESPB_OP_INCRBYFLOAT, "INCRBYFLOAT", 3},
    {RESPB_OP_MSETNX, "MSETNX", -1},

    /* List operations */
    {RESPB_OP_LPUSH, "LPUSH", -1},
    {RESPB_OP_RPUSH, "RPUSH", -1},
    {RESPB_OP_LPOP, "LPOP", -1},
    {RESPB_OP_RPOP, "RPOP", -1},
    {RESPB_OP_LRANGE, "LRANGE", 4},
    {RESPB_OP_LLEN, "LLEN", 2},
    {RESPB_OP_LINDEX, "LINDEX", 3},
    {RESPB_OP_LSET, "LSET", 4},
    {RESPB_OP_LREM, "LREM", 4},
    {RESPB_OP_LTRIM, "LTRIM", 4},

    /* Set operations */
    {RESPB_OP_SADD, "SADD", -1},
    {RESPB_OP_SREM, "SREM", -1},
    {RESPB_OP_SMEMBERS, "SMEMBERS", 2},
    {RESPB_OP_SISMEMBER, "SISMEMBER", 3},
    {RESPB_OP_SCARD, "SCARD", 2},
    {RESPB_OP_SPOP, "SPOP", -1},
    {RESPB_OP_SRANDMEMBER, "SRANDMEMBER", -1},

    /* Sorted set operations */
    {RESPB_OP_ZADD, "ZADD", -1},
    {RESPB_OP_ZREM, "ZREM", -1},
    {RESPB_OP_ZRANGE, "ZRANGE", -1},
    {RESPB_OP_ZSCORE, "ZSCORE", 3},
    {RESPB_OP_ZRANK, "ZRANK", 3},
    {RESPB_OP_ZCARD, "ZCARD", 2},
    {RESPB_OP_ZCOUNT, "ZCOUNT", 4},
    {RESPB_OP_ZINCRBY, "ZINCRBY", 4},

    /* Hash operations */
    {RESPB_OP_HSET, "HSET", -1},
    {RESPB_OP_HGET, "HGET", 3},
    {RESPB_OP_HMSET, "HMSET", -1},
    {RESPB_OP_HMGET, "HMGET", -1},
    {RESPB_OP_HGETALL, "HGETALL", 2},
    {RESPB_OP_HDEL, "HDEL", -1},
    {RESPB_OP_HEXISTS, "HEXISTS", 3},
    {RESPB_OP_HLEN, "HLEN", 2},
    {RESPB_OP_HKEYS, "HKEYS", 2},
    {RESPB_OP_HVALS, "HVALS", 2},
    {RESPB_OP_HINCRBY, "HINCRBY", 4},
    {RESPB_OP_HINCRBYFLOAT, "HINCRBYFLOAT", 4},
    {RESPB_OP_HSETNX, "HSETNX", 4},

    /* Generic key operations */
    {RESPB_OP_DEL, "DEL", -1},
    {RESPB_OP_EXISTS, "EXISTS", -1},
    {RESPB_OP_EXPIRE, "EXPIRE", -1},
    {RESPB_OP_EXPIREAT, "EXPIREAT", -1},
    {RESPB_OP_PEXPIRE, "PEXPIRE", -1},
    {RESPB_OP_PEXPIREAT, "PEXPIREAT", -1},
    {RESPB_OP_TTL, "TTL", 2},
    {RESPB_OP_PTTL, "PTTL", 2},
    {RESPB_OP_PERSIST, "PERSIST", 2},
    {RESPB_OP_TYPE, "TYPE", 2},
    {RESPB_OP_KEYS, "KEYS", 2},
    {RESPB_OP_SCAN, "SCAN", -1},
    {RESPB_OP_RENAME, "RENAME", 3},
    {RESPB_OP_RENAMENX, "RENAMENX", 3},
    {RESPB_OP_UNLINK, "UNLINK", -1},
    {RESPB_OP_TOUCH, "TOUCH", -1},
    {RESPB_OP_EXPIRETIME, "EXPIRETIME", 2},
    {RESPB_OP_PEXPIRETIME, "PEXPIRETIME", 2},

    /* Connection management */
    {RESPB_OP_AUTH, "AUTH", -1},
    {RESPB_OP_PING, "PING", -1},
    {RESPB_OP_ECHO, "ECHO", 2},
    {RESPB_OP_QUIT, "QUIT", 1},
    {RESPB_OP_SELECT, "SELECT", 2},
    {RESPB_OP_CLIENT, "CLIENT", -1},
    {RESPB_OP_HELLO, "HELLO", -1},

    /* Cluster */
    {RESPB_OP_CLUSTER, "CLUSTER", -1},
    {RESPB_OP_ASKING, "ASKING", 1},
    {RESPB_OP_READONLY, "READONLY", 1},
    {RESPB_OP_READWRITE, "READWRITE", 1},

    /* Server */
    {RESPB_OP_INFO, "INFO", -1},
    {RESPB_OP_CONFIG, "CONFIG", -1},
    {RESPB_OP_DBSIZE, "DBSIZE", 1},
    {RESPB_OP_FLUSHDB, "FLUSHDB", -1},
    {RESPB_OP_FLUSHALL, "FLUSHALL", -1},
    {RESPB_OP_TIME, "TIME", 1},
    {RESPB_OP_COMMAND, "COMMAND", -1},

    /* Pub/Sub */
    {RESPB_OP_PUBLISH, "PUBLISH", 3},
    {RESPB_OP_SUBSCRIBE, "SUBSCRIBE", -1},
    {RESPB_OP_UNSUBSCRIBE, "UNSUBSCRIBE", -1},
    {RESPB_OP_PSUBSCRIBE, "PSUBSCRIBE", -1},
    {RESPB_OP_PUNSUBSCRIBE, "PUNSUBSCRIBE", -1},

    /* Transactions */
    {RESPB_OP_MULTI, "MULTI", 1},
    {RESPB_OP_EXEC, "EXEC", 1},
    {RESPB_OP_DISCARD, "DISCARD", 1},
    {RESPB_OP_WATCH, "WATCH", -1},
    {RESPB_OP_UNWATCH, "UNWATCH", 1},

    /* Scripting */
    {RESPB_OP_EVAL, "EVAL", -1},
    {RESPB_OP_EVALSHA, "EVALSHA", -1},
    {RESPB_OP_FCALL, "FCALL", -1},
    {RESPB_OP_FCALL_RO, "FCALL_RO", -1},
};

#define OPCODE_TABLE_SIZE (sizeof(opcodeTable) / sizeof(opcodeTable[0]))

/* Find command name for opcode - returns NULL if not found */
const char *respbOpcodeToCommand(uint16_t opcode) {
    for (size_t i = 0; i < OPCODE_TABLE_SIZE; i++) {
        if (opcodeTable[i].opcode == opcode) {
            return opcodeTable[i].cmd_name;
        }
    }
    return NULL;
}

/* Get fixed argc for opcode, returns -1 for variable arity */
int respbOpcodeFixedArgc(uint16_t opcode) {
    for (size_t i = 0; i < OPCODE_TABLE_SIZE; i++) {
        if (opcodeTable[i].opcode == opcode) {
            return opcodeTable[i].fixed_argc;
        }
    }
    return -1;
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

    /* First argument is always the command name */
    c->argv[0] = createStringObject(cmd_name, strlen(cmd_name));
    c->argv_len_sum += strlen(cmd_name);
    c->argc = 1;

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
