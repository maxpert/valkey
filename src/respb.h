/*
 * RESPB (Redis Binary Protocol) - High-performance binary protocol for Valkey
 *
 * This header defines the opcodes, constants, and structures for the RESPB
 * binary protocol, providing backwards compatibility with RESP2/RESP3.
 */

#ifndef __RESPB_H
#define __RESPB_H

#include <stdint.h>

/* RESPB Magic bytes for handshake */
#define RESPB_MAGIC_BYTE1 0xD3
#define RESPB_MAGIC_BYTE2 0xC1

/* RESPB Protocol version */
#define RESPB_VERSION 1

/* RESPB Request Header Structure:
 * - 2 bytes: Opcode (0x0000 to 0xEFFF for core commands)
 * - 2 bytes: Mux ID (for multiplexing support)
 * Total: 4 bytes minimum header
 */
#define RESPB_HEADER_SIZE 4

/* RESPB Response opcodes */
#define RESPB_RESP_OK        0x8000  /* OK/status reply */
#define RESPB_RESP_ERROR     0x8001  /* Error message */
#define RESPB_RESP_INTEGER   0x8002  /* Integer */
#define RESPB_RESP_BULK      0x8003  /* Bulk string */
#define RESPB_RESP_ARRAY     0x8004  /* Array (multibulk) */
#define RESPB_RESP_NULL      0x8005  /* Null value */
#define RESPB_RESP_BOOL      0x8006  /* Boolean */
#define RESPB_RESP_DOUBLE    0x8007  /* Double/float */
#define RESPB_RESP_MAP       0x8008  /* Map (key-value pairs) */

/* Special opcodes */
#define RESPB_OP_MODULE      0xF000  /* Module commands (8-byte header) */
#define RESPB_OP_PASSTHROUGH 0xFFFF  /* RESP passthrough mode */

/* =============================================================================
 * RESPB Command Opcodes - organized by category
 * Range: 0x0000 - 0xEFFF for core commands
 * ============================================================================= */

/* String operations: 0x0000-0x003F */
#define RESPB_OP_GET         0x0000
#define RESPB_OP_SET         0x0001
#define RESPB_OP_MGET        0x0002
#define RESPB_OP_MSET        0x0003
#define RESPB_OP_INCR        0x0004
#define RESPB_OP_DECR        0x0005
#define RESPB_OP_INCRBY      0x0006
#define RESPB_OP_DECRBY      0x0007
#define RESPB_OP_APPEND      0x0008
#define RESPB_OP_STRLEN      0x0009
#define RESPB_OP_GETRANGE    0x000A
#define RESPB_OP_SETRANGE    0x000B
#define RESPB_OP_SETNX       0x000C
#define RESPB_OP_SETEX       0x000D
#define RESPB_OP_PSETEX      0x000E
#define RESPB_OP_GETSET      0x000F
#define RESPB_OP_GETEX       0x0010
#define RESPB_OP_GETDEL      0x0011
#define RESPB_OP_INCRBYFLOAT 0x0012
#define RESPB_OP_MSETNX      0x0013

/* List operations: 0x0040-0x007F */
#define RESPB_OP_LPUSH       0x0040
#define RESPB_OP_RPUSH       0x0041
#define RESPB_OP_LPOP        0x0042
#define RESPB_OP_RPOP        0x0043
#define RESPB_OP_LRANGE      0x0044
#define RESPB_OP_LLEN        0x0045
#define RESPB_OP_LINDEX      0x0046
#define RESPB_OP_LSET        0x0047
#define RESPB_OP_LREM        0x0048
#define RESPB_OP_LTRIM       0x0049
#define RESPB_OP_LPOS        0x004A
#define RESPB_OP_LINSERT     0x004B
#define RESPB_OP_LMOVE       0x004C
#define RESPB_OP_BLPOP       0x004D
#define RESPB_OP_BRPOP       0x004E
#define RESPB_OP_BLMOVE      0x004F

/* Set operations: 0x0080-0x00BF */
#define RESPB_OP_SADD        0x0080
#define RESPB_OP_SREM        0x0081
#define RESPB_OP_SMEMBERS    0x0082
#define RESPB_OP_SISMEMBER   0x0083
#define RESPB_OP_SCARD       0x0084
#define RESPB_OP_SPOP        0x0085
#define RESPB_OP_SRANDMEMBER 0x0086
#define RESPB_OP_SMOVE       0x0087
#define RESPB_OP_SDIFF       0x0088
#define RESPB_OP_SINTER      0x0089
#define RESPB_OP_SUNION      0x008A
#define RESPB_OP_SMISMEMBER  0x008B

/* Sorted set operations: 0x00C0-0x00FF */
#define RESPB_OP_ZADD        0x00C0
#define RESPB_OP_ZREM        0x00C1
#define RESPB_OP_ZRANGE      0x00C2
#define RESPB_OP_ZSCORE      0x00C3
#define RESPB_OP_ZRANK       0x00C4
#define RESPB_OP_ZCARD       0x00C5
#define RESPB_OP_ZCOUNT      0x00C6
#define RESPB_OP_ZINCRBY     0x00C7
#define RESPB_OP_ZRANGEBYSCORE 0x00C8
#define RESPB_OP_ZREVRANGE   0x00C9
#define RESPB_OP_ZREVRANK    0x00CA

/* Hash operations: 0x0100-0x013F */
#define RESPB_OP_HSET        0x0100
#define RESPB_OP_HGET        0x0101
#define RESPB_OP_HMSET       0x0102
#define RESPB_OP_HMGET       0x0103
#define RESPB_OP_HGETALL     0x0104
#define RESPB_OP_HDEL        0x0105
#define RESPB_OP_HEXISTS     0x0106
#define RESPB_OP_HLEN        0x0107
#define RESPB_OP_HKEYS       0x0108
#define RESPB_OP_HVALS       0x0109
#define RESPB_OP_HINCRBY     0x010A
#define RESPB_OP_HINCRBYFLOAT 0x010B
#define RESPB_OP_HSETNX      0x010C
#define RESPB_OP_HSCAN       0x010D
#define RESPB_OP_HRANDFIELD  0x010E

/* Bitmap operations: 0x0140-0x015F */
#define RESPB_OP_SETBIT      0x0140
#define RESPB_OP_GETBIT      0x0141
#define RESPB_OP_BITCOUNT    0x0142
#define RESPB_OP_BITOP       0x0143
#define RESPB_OP_BITPOS      0x0144
#define RESPB_OP_BITFIELD    0x0145

/* HyperLogLog operations: 0x0160-0x017F */
#define RESPB_OP_PFADD       0x0160
#define RESPB_OP_PFCOUNT     0x0161
#define RESPB_OP_PFMERGE     0x0162

/* Geospatial operations: 0x0180-0x01BF */
#define RESPB_OP_GEOADD      0x0180
#define RESPB_OP_GEODIST     0x0181
#define RESPB_OP_GEOHASH     0x0182
#define RESPB_OP_GEOPOS      0x0183
#define RESPB_OP_GEORADIUS   0x0184
#define RESPB_OP_GEOSEARCH   0x0185

/* Stream operations: 0x01C0-0x01FF */
#define RESPB_OP_XADD        0x01C0
#define RESPB_OP_XREAD       0x01C1
#define RESPB_OP_XRANGE      0x01C2
#define RESPB_OP_XLEN        0x01C3
#define RESPB_OP_XINFO       0x01C4
#define RESPB_OP_XTRIM       0x01C5
#define RESPB_OP_XDEL        0x01C6
#define RESPB_OP_XACK        0x01C7
#define RESPB_OP_XGROUP      0x01C8
#define RESPB_OP_XREADGROUP  0x01C9

/* Pub/Sub operations: 0x0200-0x023F */
#define RESPB_OP_PUBLISH     0x0200
#define RESPB_OP_SUBSCRIBE   0x0201
#define RESPB_OP_UNSUBSCRIBE 0x0202
#define RESPB_OP_PSUBSCRIBE  0x0203
#define RESPB_OP_PUNSUBSCRIBE 0x0204
#define RESPB_OP_PUBSUB      0x0205

/* Transaction operations: 0x0240-0x025F */
#define RESPB_OP_MULTI       0x0240
#define RESPB_OP_EXEC        0x0241
#define RESPB_OP_DISCARD     0x0242
#define RESPB_OP_WATCH       0x0243
#define RESPB_OP_UNWATCH     0x0244

/* Scripting operations: 0x0260-0x02BF */
#define RESPB_OP_EVAL        0x0260
#define RESPB_OP_EVALSHA     0x0261
#define RESPB_OP_SCRIPT      0x0262
#define RESPB_OP_FCALL       0x0263
#define RESPB_OP_FCALL_RO    0x0264
#define RESPB_OP_FUNCTION    0x0265

/* Generic key operations: 0x02C0-0x02FF */
#define RESPB_OP_DEL         0x02C0
#define RESPB_OP_EXISTS      0x02C1
#define RESPB_OP_EXPIRE      0x02C2
#define RESPB_OP_EXPIREAT    0x02C3
#define RESPB_OP_PEXPIRE     0x02C4
#define RESPB_OP_PEXPIREAT   0x02C5
#define RESPB_OP_TTL         0x02C6
#define RESPB_OP_PTTL        0x02C7
#define RESPB_OP_PERSIST     0x02C8
#define RESPB_OP_TYPE        0x02C9
#define RESPB_OP_KEYS        0x02CA
#define RESPB_OP_SCAN        0x02CB
#define RESPB_OP_RENAME      0x02CC
#define RESPB_OP_RENAMENX    0x02CD
#define RESPB_OP_UNLINK      0x02CE
#define RESPB_OP_TOUCH       0x02CF
#define RESPB_OP_DUMP        0x02D0
#define RESPB_OP_RESTORE     0x02D1
#define RESPB_OP_COPY        0x02D2
#define RESPB_OP_OBJECT      0x02D3
#define RESPB_OP_EXPIRETIME  0x02D4
#define RESPB_OP_PEXPIRETIME 0x02D5

/* Connection management: 0x0300-0x033F */
#define RESPB_OP_AUTH        0x0300
#define RESPB_OP_PING        0x0301
#define RESPB_OP_ECHO        0x0302
#define RESPB_OP_QUIT        0x0303
#define RESPB_OP_SELECT      0x0304
#define RESPB_OP_CLIENT      0x0305
#define RESPB_OP_HELLO       0x0306

/* Cluster management: 0x0340-0x03BF */
#define RESPB_OP_CLUSTER     0x0340
#define RESPB_OP_ASKING      0x0341
#define RESPB_OP_READONLY    0x0342
#define RESPB_OP_READWRITE   0x0343

/* Server management: 0x03C0-0x04FF */
#define RESPB_OP_INFO        0x03C0
#define RESPB_OP_CONFIG      0x03C1
#define RESPB_OP_DBSIZE      0x03C2
#define RESPB_OP_FLUSHDB     0x03C3
#define RESPB_OP_FLUSHALL    0x03C4
#define RESPB_OP_SAVE        0x03C5
#define RESPB_OP_BGSAVE      0x03C6
#define RESPB_OP_BGREWRITEAOF 0x03C7
#define RESPB_OP_TIME        0x03C8
#define RESPB_OP_DEBUG       0x03C9
#define RESPB_OP_MEMORY      0x03CA
#define RESPB_OP_SLOWLOG     0x03CB
#define RESPB_OP_ACL         0x03CC
#define RESPB_OP_COMMAND     0x03CD
#define RESPB_OP_LATENCY     0x03CE

/* =============================================================================
 * RESPB Structures
 * ============================================================================= */

/* RESPB request header (4 bytes for core commands) */
typedef struct respbHeader {
    uint16_t opcode;   /* Command opcode (network byte order) */
    uint16_t mux_id;   /* Multiplexing ID (network byte order) */
} respbHeader;

/* RESPB module command header (8 bytes) */
typedef struct respbModuleHeader {
    uint16_t opcode;      /* Always RESPB_OP_MODULE (0xF000) */
    uint16_t mux_id;      /* Multiplexing ID */
    uint16_t module_id;   /* Module identifier */
    uint16_t cmd_id;      /* Command identifier within module */
} respbModuleHeader;

/* RESPB passthrough header (8 bytes) */
typedef struct respbPassthroughHeader {
    uint16_t opcode;      /* Always RESPB_OP_PASSTHROUGH (0xFFFF) */
    uint16_t mux_id;      /* Multiplexing ID */
    uint32_t resp_len;    /* Length of RESP data following */
} respbPassthroughHeader;

/* RESPB response header (4 bytes) */
typedef struct respbResponseHeader {
    uint16_t opcode;   /* Response type opcode */
    uint16_t mux_id;   /* Echo back the mux_id from request */
} respbResponseHeader;

/* =============================================================================
 * RESPB Helper Macros
 * ============================================================================= */

/* Check if first byte indicates RESPB protocol (not RESP) */
#define IS_RESPB_BYTE(b) ((unsigned char)(b) != '*' && \
                          (unsigned char)(b) != '$' && \
                          (unsigned char)(b) != '+' && \
                          (unsigned char)(b) != '-' && \
                          (unsigned char)(b) != ':' && \
                          (unsigned char)(b) != '_' && \
                          (unsigned char)(b) != '#' && \
                          (unsigned char)(b) != ',' && \
                          (unsigned char)(b) != '(' && \
                          (unsigned char)(b) != '!' && \
                          (unsigned char)(b) != '=' && \
                          (unsigned char)(b) != '%' && \
                          (unsigned char)(b) != '~' && \
                          (unsigned char)(b) != '>')

/* Check if opcode is in valid core command range */
#define IS_RESPB_CORE_OPCODE(op) ((op) <= 0xEFFF)

/* Check if opcode is module command */
#define IS_RESPB_MODULE_OPCODE(op) ((op) == RESPB_OP_MODULE)

/* Check if opcode is passthrough */
#define IS_RESPB_PASSTHROUGH_OPCODE(op) ((op) == RESPB_OP_PASSTHROUGH)

/* Check if opcode is a response */
#define IS_RESPB_RESPONSE_OPCODE(op) ((op) >= 0x8000 && (op) < 0xF000)

/* Convert between host and network byte order for 16-bit */
#ifdef __BIG_ENDIAN__
#define RESPB_HTONS(x) (x)
#define RESPB_NTOHS(x) (x)
#else
#define RESPB_HTONS(x) ((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF))
#define RESPB_NTOHS(x) ((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF))
#endif

/* Convert between host and network byte order for 32-bit */
#ifdef __BIG_ENDIAN__
#define RESPB_HTONL(x) (x)
#define RESPB_NTOHL(x) (x)
#else
#define RESPB_HTONL(x) (((x) << 24) | (((x) & 0xFF00) << 8) | \
                        (((x) >> 8) & 0xFF00) | ((x) >> 24))
#define RESPB_NTOHL(x) (((x) << 24) | (((x) & 0xFF00) << 8) | \
                        (((x) >> 8) & 0xFF00) | ((x) >> 24))
#endif

/* =============================================================================
 * RESPB Protocol Constants
 * ============================================================================= */

/* Maximum number of arguments for variable-arity commands (2-byte count) */
#define RESPB_MAX_ARGC 65535

/* Maximum bulk string length (4-byte length prefix) */
#define RESPB_MAX_BULK_LEN 0xFFFFFFFF

/* Small bulk threshold - use 2-byte length if under 64KB */
#define RESPB_SMALL_BULK_THRESHOLD 65535

/* Protocol version for RESPB */
#define PROTO_RESPB 4

/* Request type for RESPB */
#define PROTO_REQ_RESPB 3

/* =============================================================================
 * RESPB Function Declarations
 * ============================================================================= */

/* Forward declarations */
struct client;
struct serverObject;

/* Server initialization - call once after command table is ready */
void respbInitServer(void);

/* Parsing functions */
int parseRespbBuffer(struct client *c);
const char *respbOpcodeToCommand(uint16_t opcode);
int respbOpcodeFixedArgc(uint16_t opcode);
struct redisCommand *respbOpcodeCommand(uint16_t opcode);
struct serverObject *respbOpcodeSharedName(uint16_t opcode);

/* Response functions - RESPB-aware wrappers */
void addReplyRespbOK(struct client *c);
void addReplyRespbError(struct client *c, const char *err);
void addReplyRespbLongLong(struct client *c, long long ll);
void addReplyRespbNull(struct client *c);
void addReplyRespbBulkCBuffer(struct client *c, const void *p, size_t len);
void addReplyRespbBulk(struct client *c, struct serverObject *obj);
void addReplyRespbArrayLen(struct client *c, long length);
void addReplyRespbMapLen(struct client *c, long length);
void addReplyRespbBool(struct client *c, int b);
void addReplyRespbDouble(struct client *c, double d);

/* Array element functions (no response header) */
void addReplyRespbBulkElement(struct client *c, const void *p, size_t len);
void addReplyRespbNullElement(struct client *c);
void addReplyRespbLongLongElement(struct client *c, long long ll);

/* =============================================================================
 * Client-side RESPB Functions (for benchmark/cli tools)
 * ============================================================================= */

/* Format a command in RESPB format - returns zmalloc'd buffer, caller must zfree */
char *respbFormatCommand(size_t *len, int argc, const char **argv, const size_t *argvlen);

/* Parse RESPB response from buffer
 * Returns: >0 bytes consumed, 0 need more data, -1 error */
int respbParseResponse(const char *buf, size_t buflen,
                       uint16_t *type, uint16_t *mux_id,
                       const char **data, size_t *datalen);

/* Get integer value from RESPB integer response data */
long long respbGetInteger(const char *data);

#endif /* __RESPB_H */
