/*
 * RESPB (Redis Binary Protocol) Implementation - Client/Shared Functions
 *
 * This file implements the RESPB binary protocol for client-side use
 * (CLI, benchmark). Server-side functions are in respb_server.c
 */

#include "respb.h"
#include "zmalloc.h"
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>

/* =============================================================================
 * RESPB Opcode to Command Mapping
 * ============================================================================= */

/* Structure for direct opcode lookup table - O(1) access */
typedef struct respbOpcodeInfo {
    const char *cmd_name;      /* NULL if opcode not valid */
    int8_t fixed_argc;         /* -1 for variable arity, 0 if invalid */
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

    /* Write header (using memcpy for safe unaligned access) */
    uint16_t opcode_net = htons(opcode);
    uint16_t mux_id_net = htons(0);  /* mux_id = 0 for now */
    memcpy(p, &opcode_net, sizeof(opcode_net));
    p += 2;
    memcpy(p, &mux_id_net, sizeof(mux_id_net));
    p += 2;

    /* Write argc for variable arity commands */
    if (fixed_argc < 0) {
        uint16_t argc_net = htons((uint16_t)(argc - 1));  /* exclude command name */
        memcpy(p, &argc_net, sizeof(argc_net));
        p += 2;
    }

    /* Write arguments (skip command name) */
    for (int i = 1; i < argc; i++) {
        size_t arglen = argvlen ? argvlen[i] : strlen(argv[i]);
        if (arglen < 0xFFFF) {
            uint16_t len_net = htons((uint16_t)arglen);
            memcpy(p, &len_net, sizeof(len_net));
            p += 2;
        } else {
            uint16_t marker = htons(0xFFFF);
            uint32_t len_net = htonl((uint32_t)arglen);
            memcpy(p, &marker, sizeof(marker));
            p += 2;
            memcpy(p, &len_net, sizeof(len_net));
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

    /* Read header using memcpy for safe unaligned access */
    uint16_t opcode_raw, mux_id_raw;
    memcpy(&opcode_raw, buf, sizeof(opcode_raw));
    memcpy(&mux_id_raw, buf + 2, sizeof(mux_id_raw));
    uint16_t resp_opcode = ntohs(opcode_raw);
    *mux_id = ntohs(mux_id_raw);
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
        uint16_t len16_raw;
        memcpy(&len16_raw, buf + pos, sizeof(len16_raw));
        uint16_t len16 = ntohs(len16_raw);
        pos += 2;

        size_t len;
        if (len16 == 0xFFFF) {
            /* Large string */
            if (buflen < pos + 4) return 0;
            uint32_t len32_raw;
            memcpy(&len32_raw, buf + pos, sizeof(len32_raw));
            len = ntohl(len32_raw);
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
        uint16_t count16_raw;
        memcpy(&count16_raw, buf + pos, sizeof(count16_raw));
        uint16_t count16 = ntohs(count16_raw);
        pos += 2;

        size_t count;
        if (count16 == 0xFFFF) {
            if (buflen < pos + 4) return 0;
            uint32_t count32_raw;
            memcpy(&count32_raw, buf + pos, sizeof(count32_raw));
            count = ntohl(count32_raw);
            pos += 4;
        } else {
            count = count16;
        }

        /* For MAP, count is number of pairs, so double it for elements */
        if (resp_opcode == RESPB_RESP_MAP) count *= 2;

        /* Consume elements - they may be nested RESPB responses */
        for (size_t i = 0; i < count; i++) {
            if (buflen < pos + 2) return 0;
            uint16_t elem_header_raw;
            memcpy(&elem_header_raw, buf + pos, sizeof(elem_header_raw));
            uint16_t elem_header = ntohs(elem_header_raw);

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
                uint32_t elen_raw;
                memcpy(&elen_raw, buf + pos, sizeof(elen_raw));
                size_t elen = ntohl(elen_raw);
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
