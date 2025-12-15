/*
 * RESPB Server-side Implementation
 *
 * This file contains server-only RESPB functions that use server.h APIs.
 * Client-side functions (for CLI/benchmark) are in respb.c
 */

#include "server.h"
#include "respb.h"
#include <arpa/inet.h>

/* =============================================================================
 * Server-side Opcode Table Extensions
 * ============================================================================= */

/* Extended opcode info for server - stores cached command pointers */
typedef struct respbServerOpcodeInfo {
    robj *shared_name;            /* Shared robj for command name */
    struct serverCommand *cmd;    /* Direct command pointer */
} respbServerOpcodeInfo;

#define RESPB_OPCODE_TABLE_SIZE 0x0400
static respbServerOpcodeInfo serverOpcodeTable[RESPB_OPCODE_TABLE_SIZE];
static int serverOpcodeTableInitialized = 0;

/* Server-only initialization - creates shared objects and resolves commands */
void respbInitServer(void) {
    if (serverOpcodeTableInitialized) return;

    /* Ensure base opcode table is initialized first */
    respbOpcodeToCommand(0);  /* This triggers initOpcodeTable() */

    memset(serverOpcodeTable, 0, sizeof(serverOpcodeTable));

    for (int i = 0; i < RESPB_OPCODE_TABLE_SIZE; i++) {
        const char *cmd_name = respbOpcodeToCommand(i);
        if (cmd_name) {
            /* Create shared robj for command name */
            serverOpcodeTable[i].shared_name = createStringObject(cmd_name, strlen(cmd_name));
            /* Lookup and cache the command pointer */
            serverOpcodeTable[i].cmd = lookupCommandByCString((char*)cmd_name);
        }
    }
    serverOpcodeTableInitialized = 1;
}

/* Get cached command for opcode - O(1), no string lookup */
struct serverCommand *respbOpcodeCommand(uint16_t opcode) {
    if (opcode >= RESPB_OPCODE_TABLE_SIZE) return NULL;
    return serverOpcodeTable[opcode].cmd;
}

/* Get shared command name robj for opcode - O(1), no allocation */
robj *respbOpcodeSharedName(uint16_t opcode) {
    if (opcode >= RESPB_OPCODE_TABLE_SIZE) return NULL;
    return serverOpcodeTable[opcode].shared_name;
}

/* =============================================================================
 * RESPB Request Parsing (Server-side)
 * ============================================================================= */

/* Read a 2-byte length prefix from buffer (handles unaligned access safely) */
static inline int respbReadLen16(const char *buf, size_t buflen, size_t *pos, uint16_t *len) {
    if (*pos + 2 > buflen) return C_ERR;
    uint16_t tmp;
    memcpy(&tmp, buf + *pos, sizeof(tmp));
    *len = ntohs(tmp);
    *pos += 2;
    return C_OK;
}

/* Read a 4-byte length prefix from buffer (handles unaligned access safely) */
static inline int respbReadLen32(const char *buf, size_t buflen, size_t *pos, uint32_t *len) {
    if (*pos + 4 > buflen) return C_ERR;
    uint32_t tmp;
    memcpy(&tmp, buf + *pos, sizeof(tmp));
    *len = ntohl(tmp);
    *pos += 4;
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

    /* Read header (using memcpy for safe unaligned access) */
    uint16_t opcode_raw, mux_id_raw;
    memcpy(&opcode_raw, buf + pos, sizeof(opcode_raw));
    memcpy(&mux_id_raw, buf + pos + 2, sizeof(mux_id_raw));
    uint16_t opcode = ntohs(opcode_raw);
    uint16_t mux_id = ntohs(mux_id_raw);
    pos += RESPB_HEADER_SIZE;

    /* Store mux_id and opcode for response */
    c->respb_mux_id = mux_id;
    c->respb_opcode = opcode;

    /* Handle RESP passthrough */
    if (IS_RESPB_PASSTHROUGH_OPCODE(opcode)) {
        if (qblen - pos < 4) return 0;  /* Need RESP length */
        uint32_t resp_len_raw;
        memcpy(&resp_len_raw, buf + pos, sizeof(resp_len_raw));
        uint32_t resp_len = ntohl(resp_len_raw);
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
 * RESPB Response Functions (Server-side)
 * ============================================================================= */

/* Add RESPB response header to output buffer */
static void addRespbResponseHeader(client *c, uint16_t resp_opcode) {
    char header[4];
    uint16_t opcode_net = htons(resp_opcode);
    uint16_t mux_id_net = htons(c->respb_mux_id);
    memcpy(header, &opcode_net, sizeof(opcode_net));
    memcpy(header + 2, &mux_id_net, sizeof(mux_id_net));
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
        uint16_t len_net = htons(len);
        memcpy(lenbuf, &len_net, sizeof(len_net));
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
            uint16_t len_net = htons((uint16_t)len);
            memcpy(lenbuf, &len_net, sizeof(len_net));
            addReplyProto(c, lenbuf, 2);
        } else {
            /* Large string marker + 4-byte length */
            char lenbuf[6];
            uint16_t marker = htons(0xFFFF);
            uint32_t len_net = htonl((uint32_t)len);
            memcpy(lenbuf, &marker, sizeof(marker));
            memcpy(lenbuf + 2, &len_net, sizeof(len_net));
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
            uint16_t len_net = htons((uint16_t)length);
            memcpy(lenbuf, &len_net, sizeof(len_net));
            addReplyProto(c, lenbuf, 2);
        } else {
            char lenbuf[6];
            uint16_t marker = htons(0xFFFF);
            uint32_t len_net = htonl((uint32_t)length);
            memcpy(lenbuf, &marker, sizeof(marker));
            memcpy(lenbuf + 2, &len_net, sizeof(len_net));
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
            uint16_t len_net = htons((uint16_t)length);
            memcpy(lenbuf, &len_net, sizeof(len_net));
            addReplyProto(c, lenbuf, 2);
        } else {
            char lenbuf[6];
            uint16_t marker = htons(0xFFFF);
            uint32_t len_net = htonl((uint32_t)length);
            memcpy(lenbuf, &marker, sizeof(marker));
            memcpy(lenbuf + 2, &len_net, sizeof(len_net));
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
        uint64_t val;
        /* Use memcpy for type punning to avoid strict aliasing violation */
        memcpy(&val, &d, sizeof(val));
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
            uint16_t len_net = htons((uint16_t)len);
            memcpy(lenbuf, &len_net, sizeof(len_net));
            addReplyProto(c, lenbuf, 2);
        } else {
            char lenbuf[6];
            uint16_t marker = htons(0xFFFF);
            uint32_t len_net = htonl((uint32_t)len);
            memcpy(lenbuf, &marker, sizeof(marker));
            memcpy(lenbuf + 2, &len_net, sizeof(len_net));
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
        char marker_buf[2];
        uint16_t marker = htons(0xFFFE);
        memcpy(marker_buf, &marker, sizeof(marker));
        addReplyProto(c, marker_buf, 2);
    } else {
        addReplyNull(c);
    }
}

/* Add integer element to array */
void addReplyRespbLongLongElement(client *c, long long ll) {
    if (c->resp == PROTO_RESPB) {
        /* Use special marker 0xFFFD followed by 8-byte int */
        char buf[10];
        uint16_t marker = htons(0xFFFD);
        memcpy(buf, &marker, sizeof(marker));
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
