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

/* =============================================================================
 * Command-Specific Decoders
 * ============================================================================= */

/* Parse context for decoders - allows parsing into client or parsedCommand */
typedef struct respbParseContext {
    int *argc;
    robj ***argv;
    int *argv_len;
    size_t *argv_len_sum;
} respbParseContext;

/* Decoder for KEY_ONLY pattern: GET, INCR
 * Format: [keylen:2B][key]
 * Produces argv: [command, key]
 */
static int respbDecodeKeyOnly(client *c, const char *buf, size_t buflen, size_t *pos,
                               respbParseContext *ctx) {
    uint16_t keylen;
    if (respbReadLen16(buf, buflen, pos, &keylen) == C_ERR) return 0;

    /* Validate keylen to prevent huge allocations */
    if (keylen > 512 * 1024 * 1024) {  /* 512MB max key size */
        return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
    }

    if (buflen - *pos < keylen) return 0;

    /* Allocate argv for 2 args: command + key */
    *ctx->argv = zmalloc(sizeof(robj *) * 2);
    if (!*ctx->argv) return 0;

    *ctx->argv_len = 2;
    *ctx->argc = 0;  /* Increment as we build */
    *ctx->argv_len_sum = 0;

    /* Set command name */
    robj *shared_name = respbOpcodeSharedName(c->respb_opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        (*ctx->argv)[0] = shared_name;
        *ctx->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        const char *cmd_name = respbOpcodeToCommand(c->respb_opcode);
        (*ctx->argv)[0] = createStringObject(cmd_name, strlen(cmd_name));
        *ctx->argv_len_sum += strlen(cmd_name);
    }
    *ctx->argc = 1;

    /* Set key */
    (*ctx->argv)[1] = createStringObject(buf + *pos, keylen);
    *ctx->argv_len_sum += keylen;
    *ctx->argc = 2;
    *pos += keylen;

    return READ_FLAGS_PARSING_COMPLETED;
}

/* Decoder for KEY_VALUE pattern: SET
 * Format: [keylen:2B][key][vallen:4B][value][flags:1B]
 * Basic: ignores flags, produces argv: [SET, key, value]
 */
static int respbDecodeSet(client *c, const char *buf, size_t buflen, size_t *pos,
                          respbParseContext *ctx) {
    uint16_t keylen;
    if (respbReadLen16(buf, buflen, pos, &keylen) == C_ERR) return 0;

    /* Validate keylen */
    if (keylen > 512 * 1024 * 1024) {  /* 512MB max */
        return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
    }

    if (buflen - *pos < keylen) return 0;
    size_t key_pos = *pos;
    *pos += keylen;

    uint32_t vallen;
    if (respbReadLen32(buf, buflen, pos, &vallen) == C_ERR) return 0;

    /* Validate vallen */
    if (vallen > 512 * 1024 * 1024) {  /* 512MB max */
        return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
    }

    if (buflen - *pos < vallen) return 0;
    size_t val_pos = *pos;
    *pos += vallen;

    /* Read flags byte */
    if (buflen - *pos < 1) return 0;
    /* uint8_t flags = buf[*pos]; */  /* Basic: ignore flags for now */
    *pos += 1;

    /* Allocate argv for 3 args: command + key + value */
    *ctx->argv = zmalloc(sizeof(robj *) * 3);
    if (!*ctx->argv) return 0;

    *ctx->argv_len = 3;
    *ctx->argc = 0;  /* Increment as we build */
    *ctx->argv_len_sum = 0;

    /* Set command name */
    robj *shared_name = respbOpcodeSharedName(c->respb_opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        (*ctx->argv)[0] = shared_name;
        *ctx->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        (*ctx->argv)[0] = createStringObject("SET", 3);
        *ctx->argv_len_sum += 3;
    }
    *ctx->argc = 1;

    /* Set key and value */
    (*ctx->argv)[1] = createStringObject(buf + key_pos, keylen);
    *ctx->argv_len_sum += keylen;
    *ctx->argc = 2;

    (*ctx->argv)[2] = createStringObject(buf + val_pos, vallen);
    *ctx->argv_len_sum += vallen;
    *ctx->argc = 3;

    return READ_FLAGS_PARSING_COMPLETED;
}

/* Decoder for KEY_ELEMENTS pattern: LPUSH, RPUSH
 * Format: [keylen:2B][key][count:2B]([elemlen:2B][elem])...
 * Produces argv: [command, key, elem1, elem2, ...]
 */
static int respbDecodeKeyElements(client *c, const char *buf, size_t buflen, size_t *pos,
                                   respbParseContext *ctx) {
    uint16_t keylen;
    if (respbReadLen16(buf, buflen, pos, &keylen) == C_ERR) return 0;
    if (buflen - *pos < keylen) return 0;
    size_t key_pos = *pos;
    *pos += keylen;

    uint16_t count;
    if (respbReadLen16(buf, buflen, pos, &count) == C_ERR) return 0;

    /* Validate count to prevent huge allocations from corrupted data */
    if (count > 10000) {  /* Reasonable limit for LPUSH/RPUSH */
        return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
    }

    /* Allocate argv: command + key + elements */
    int argc = 2 + count;
    *ctx->argv = zmalloc(sizeof(robj *) * argc);
    if (!*ctx->argv) return 0;

    *ctx->argv_len = argc;
    *ctx->argv_len_sum = 0;
    *ctx->argc = 0;  /* Increment as we build */

    /* Set command name */
    robj *shared_name = respbOpcodeSharedName(c->respb_opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        (*ctx->argv)[0] = shared_name;
        *ctx->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        const char *cmd_name = respbOpcodeToCommand(c->respb_opcode);
        (*ctx->argv)[0] = createStringObject(cmd_name, strlen(cmd_name));
        *ctx->argv_len_sum += strlen(cmd_name);
    }
    *ctx->argc = 1;

    /* Set key */
    (*ctx->argv)[1] = createStringObject(buf + key_pos, keylen);
    *ctx->argv_len_sum += keylen;
    *ctx->argc = 2;

    /* Read elements */
    for (int i = 0; i < count; i++) {
        uint16_t elemlen;
        if (respbReadLen16(buf, buflen, pos, &elemlen) == C_ERR) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }
        if (buflen - *pos < elemlen) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }
        (*ctx->argv)[2 + i] = createStringObject(buf + *pos, elemlen);
        *ctx->argv_len_sum += elemlen;
        (*ctx->argc)++;
        *pos += elemlen;
    }

    return READ_FLAGS_PARSING_COMPLETED;
}

/* Decoder for KEY_OPTCOUNT pattern: LPOP, RPOP
 * Format: [keylen:2B][key][count?:2B]
 * Produces argv: [command, key] or [command, key, count]
 *
 * NOTE: Since we cannot reliably detect if the optional count is present
 * (we'd need to check against the next message boundary), we always try to
 * read it if exactly 2 bytes remain. If there are 0 or 1 bytes, no count.
 * If 2+ bytes and they form a valid small count, include it. Otherwise, assume no count.
 */
static int respbDecodeKeyOptCount(client *c, const char *buf, size_t buflen, size_t *pos,
                                   respbParseContext *ctx) {
    uint16_t keylen;
    if (respbReadLen16(buf, buflen, pos, &keylen) == C_ERR) return 0;
    if (buflen - *pos < keylen) return 0;
    size_t key_pos = *pos;
    *pos += keylen;

    /* Save position after key */
    size_t pos_after_key = *pos;

    /* Determine if we have count field:
     * Only read count if we have exactly 2 bytes left (not more, not less).
     * If we have more than 2 bytes, they likely belong to the next message.
     * This is a heuristic since RESPB doesn't encode total message length.
     */
    int has_count = 0;
    uint16_t count_val = 0;
    size_t bytes_remaining = buflen - *pos;

    if (bytes_remaining == 2) {
        /* Exactly 2 bytes - likely a count field */
        if (respbReadLen16(buf, buflen, pos, &count_val) == C_OK) {
            /* Sanity check: count should be reasonable (< 10000 for LPOP/RPOP) */
            if (count_val > 0 && count_val < 10000) {
                has_count = 1;
            } else {
                /* Unreasonable value - probably next message header, revert */
                *pos = pos_after_key;
                has_count = 0;
            }
        }
    } else if (bytes_remaining > 2) {
        /* More than 2 bytes - next message likely present, no count */
        has_count = 0;
    } else {
        /* 0 or 1 bytes - incomplete, need more data or no count */
        if (bytes_remaining > 0) return 0;  /* Need more data */
        has_count = 0;
    }

    int argc = has_count ? 3 : 2;
    *ctx->argv = zmalloc(sizeof(robj *) * argc);
    if (!*ctx->argv) return 0;

    *ctx->argv_len = argc;
    *ctx->argc = 0;  /* Increment as we build */
    *ctx->argv_len_sum = 0;

    /* Set command name */
    robj *shared_name = respbOpcodeSharedName(c->respb_opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        (*ctx->argv)[0] = shared_name;
        *ctx->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        const char *cmd_name = respbOpcodeToCommand(c->respb_opcode);
        (*ctx->argv)[0] = createStringObject(cmd_name, strlen(cmd_name));
        *ctx->argv_len_sum += strlen(cmd_name);
    }
    *ctx->argc = 1;

    /* Set key */
    (*ctx->argv)[1] = createStringObject(buf + key_pos, keylen);
    *ctx->argv_len_sum += keylen;
    *ctx->argc = 2;

    /* Add count if present */
    if (has_count) {
        /* Convert count to string */
        char count_str[16];
        int count_len = snprintf(count_str, sizeof(count_str), "%u", count_val);
        (*ctx->argv)[2] = createStringObject(count_str, count_len);
        *ctx->argv_len_sum += count_len;
        *ctx->argc = 3;
    }

    return READ_FLAGS_PARSING_COMPLETED;
}

/* Decoder for KEY_PAIRS pattern: HSET
 * Format: [keylen:2B][key][count:2B]([fieldlen:2B][field][vallen:4B][value])...
 * Produces argv: [HSET, key, field1, value1, field2, value2, ...]
 */
static int respbDecodeKeyPairs(client *c, const char *buf, size_t buflen, size_t *pos,
                                respbParseContext *ctx) {
    uint16_t keylen;
    if (respbReadLen16(buf, buflen, pos, &keylen) == C_ERR) return 0;
    if (buflen - *pos < keylen) return 0;
    size_t key_pos = *pos;
    *pos += keylen;

    uint16_t pair_count;
    if (respbReadLen16(buf, buflen, pos, &pair_count) == C_ERR) return 0;

    /* Validate pair_count to prevent huge allocations */
    if (pair_count > 10000) {  /* Reasonable limit for HSET */
        return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
    }

    /* Allocate argv: command + key + (field-value pairs) */
    int argc = 2 + (pair_count * 2);
    *ctx->argv = zmalloc(sizeof(robj *) * argc);
    if (!*ctx->argv) return 0;

    *ctx->argv_len = argc;
    *ctx->argv_len_sum = 0;
    *ctx->argc = 0;  /* Increment as we build */

    /* Set command name */
    robj *shared_name = respbOpcodeSharedName(c->respb_opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        (*ctx->argv)[0] = shared_name;
        *ctx->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        (*ctx->argv)[0] = createStringObject("HSET", 4);
        *ctx->argv_len_sum += 4;
    }
    *ctx->argc = 1;

    /* Set key */
    (*ctx->argv)[1] = createStringObject(buf + key_pos, keylen);
    *ctx->argv_len_sum += keylen;
    *ctx->argc = 2;

    /* Read field-value pairs */
    for (int i = 0; i < pair_count; i++) {
        /* Read field (2B length) */
        uint16_t fieldlen;
        if (respbReadLen16(buf, buflen, pos, &fieldlen) == C_ERR) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }
        if (buflen - *pos < fieldlen) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }
        (*ctx->argv)[2 + i * 2] = createStringObject(buf + *pos, fieldlen);
        *ctx->argv_len_sum += fieldlen;
        (*ctx->argc)++;
        *pos += fieldlen;

        /* Read value (4B length) */
        uint32_t vallen;
        if (respbReadLen32(buf, buflen, pos, &vallen) == C_ERR) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }
        if (buflen - *pos < vallen) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }
        (*ctx->argv)[2 + i * 2 + 1] = createStringObject(buf + *pos, vallen);
        *ctx->argv_len_sum += vallen;
        (*ctx->argc)++;
        *pos += vallen;
    }

    return READ_FLAGS_PARSING_COMPLETED;
}

/* Generic decoder - fallback for unsupported commands
 * Format: [count:2B]([len:2B][arg])...
 */
static int respbDecodeGeneric(client *c, const char *buf, size_t buflen, size_t *pos,
                               int fixed_argc, uint16_t opcode, respbParseContext *ctx) {
    int argc;
    if (fixed_argc > 0) {
        argc = fixed_argc;
        /* Validate fixed argc */
        if (argc > 1000) {  /* Reasonable limit */
            return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
        }
    } else {
        /* Variable arity - read 2-byte count */
        if (buflen - *pos < 2) return 0;
        uint16_t count;
        if (respbReadLen16(buf, buflen, pos, &count) == C_ERR) return 0;

        /* Validate count */
        if (count > 10000) {  /* Reasonable limit */
            return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
        }

        argc = count + 1;  /* +1 for command name */
    }

    /* Allocate argv */
    *ctx->argv = zmalloc(sizeof(robj *) * argc);
    if (!*ctx->argv) return 0;

    *ctx->argv_len = argc;
    *ctx->argv_len_sum = 0;
    *ctx->argc = 0;  /* Increment as we build */

    /* First argument is the shared command name */
    robj *shared_name = respbOpcodeSharedName(opcode);
    if (shared_name) {
        incrRefCount(shared_name);
        (*ctx->argv)[0] = shared_name;
        *ctx->argv_len_sum += sdslen(shared_name->ptr);
    } else {
        const char *cmd_name = respbOpcodeToCommand(opcode);
        (*ctx->argv)[0] = createStringObject(cmd_name, strlen(cmd_name));
        *ctx->argv_len_sum += strlen(cmd_name);
    }
    *ctx->argc = 1;

    /* Read remaining arguments */
    for (int i = 1; i < argc; i++) {
        if (buflen - *pos < 2) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }

        uint16_t len16;
        if (respbReadLen16(buf, buflen, pos, &len16) == C_ERR) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }

        /* Handle large strings with 0xFFFF marker */
        size_t len;
        if (len16 == 0xFFFF) {
            if (buflen - *pos < 4) {
                for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
                zfree(*ctx->argv);
                *ctx->argv = NULL;
                *ctx->argc = 0;
                return 0;
            }
            uint32_t len32;
            if (respbReadLen32(buf, buflen, pos, &len32) == C_ERR) {
                for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
                zfree(*ctx->argv);
                *ctx->argv = NULL;
                *ctx->argc = 0;
                return 0;
            }
            len = len32;

            /* Validate large string length */
            if (len > 512 * 1024 * 1024) {  /* 512MB max */
                for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
                zfree(*ctx->argv);
                *ctx->argv = NULL;
                *ctx->argc = 0;
                return READ_FLAGS_ERROR_BIG_INLINE_REQUEST;
            }
        } else {
            len = len16;
        }

        if (buflen - *pos < len) {
            for (int j = 0; j < *ctx->argc; j++) decrRefCount((*ctx->argv)[j]);
            zfree(*ctx->argv);
            *ctx->argv = NULL;
            *ctx->argc = 0;
            return 0;
        }

        (*ctx->argv)[i] = createStringObject(buf + *pos, len);
        *ctx->argv_len_sum += len;
        (*ctx->argc)++;
        *pos += len;
    }

    return READ_FLAGS_PARSING_COMPLETED;
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

    /* Dispatch to decoder based on encoding type */
    size_t start_pos = pos;
    int result = 0;
    uint8_t enc_type = RESPB_ENC_GENERIC;

    /* Get encoding type from opcode table */
    if (opcode < RESPB_OPCODE_TABLE_SIZE) {
        const char *check_cmd = respbOpcodeToCommand(opcode);
        if (check_cmd) {
            /* Access the client-side opcode table to get enc_type */
            /* We need to call a function that exposes this from respb.c */
            /* For now, dispatch based on opcode directly */
            switch (opcode) {
            case RESPB_OP_GET:
            case RESPB_OP_INCR:
                enc_type = RESPB_ENC_KEY_ONLY;
                break;
            case RESPB_OP_SET:
                enc_type = RESPB_ENC_KEY_VALUE;
                break;
            case RESPB_OP_LPUSH:
            case RESPB_OP_RPUSH:
                enc_type = RESPB_ENC_KEY_ELEMENTS;
                break;
            case RESPB_OP_LPOP:
            case RESPB_OP_RPOP:
                enc_type = RESPB_ENC_KEY_OPTCOUNT;
                break;
            case RESPB_OP_HSET:
                enc_type = RESPB_ENC_KEY_PAIRS;
                break;
            default:
                enc_type = RESPB_ENC_GENERIC;
                break;
            }
        }
    }

    /* Create parse context for first command (parse into client struct) */
    respbParseContext ctx = {
        .argc = &c->argc,
        .argv = &c->argv,
        .argv_len = &c->argv_len,
        .argv_len_sum = &c->argv_len_sum
    };

    /* Dispatch to appropriate decoder */
    switch (enc_type) {
    case RESPB_ENC_KEY_ONLY:
        result = respbDecodeKeyOnly(c, buf, qblen, &pos, &ctx);
        break;
    case RESPB_ENC_KEY_VALUE:
        result = respbDecodeSet(c, buf, qblen, &pos, &ctx);
        break;
    case RESPB_ENC_KEY_ELEMENTS:
        result = respbDecodeKeyElements(c, buf, qblen, &pos, &ctx);
        break;
    case RESPB_ENC_KEY_OPTCOUNT:
        result = respbDecodeKeyOptCount(c, buf, qblen, &pos, &ctx);
        break;
    case RESPB_ENC_KEY_PAIRS:
        result = respbDecodeKeyPairs(c, buf, qblen, &pos, &ctx);
        break;
    case RESPB_ENC_GENERIC:
    default: {
        int fixed_argc = respbOpcodeFixedArgc(opcode);
        result = respbDecodeGeneric(c, buf, qblen, &pos, fixed_argc, opcode, &ctx);
        break;
    }
    }

    /* Check if parsing completed or need more data */
    if (result == 0) return 0;  /* Need more data */
    if (result != READ_FLAGS_PARSING_COMPLETED) return result;  /* Error */

    /* Set command directly - bypass string lookup in prepareCommand */
    c->parsed_cmd = respbOpcodeCommand(opcode);

    /* Update query buffer position */
    c->qb_pos = pos;
    c->net_input_bytes_curr_cmd = pos - start_pos + RESPB_HEADER_SIZE;
    c->read_flags |= READ_FLAGS_PARSING_COMPLETED;
    c->reqtype = 0;

    /* Try parsing pipelined commands (similar to RESP pipelining) */
    cmdQueue *queue = &c->cmd_queue;
    serverAssert(queue->len == 0);
    int flag = READ_FLAGS_PARSING_COMPLETED;

    while ((flag & READ_FLAGS_PARSING_COMPLETED) &&
           sdslen(c->querybuf) > c->qb_pos &&
           (sdslen(c->querybuf) - c->qb_pos) >= RESPB_HEADER_SIZE) {

        c->reqtype = PROTO_REQ_RESPB;

        /* Grow command queue if needed */
        if (queue->len == queue->cap) {
            if (queue->cap == 0) {
                queue->cap = 16;  /* Min capacity */
            } else if (queue->cap <= 512) {
                queue->cap *= 2;
            } else {
                break;  /* Limit to 512 commands */
            }
            queue->cmds = zrealloc(queue->cmds, queue->cap * sizeof(parsedCommand));
        }

        /* Parse next command into queue */
        parsedCommand *p = &queue->cmds[queue->len++];
        memset(p, 0, sizeof(*p));

        /* Read header for next command */
        size_t cmd_pos = c->qb_pos;
        uint16_t cmd_opcode_raw, cmd_mux_id_raw;
        memcpy(&cmd_opcode_raw, c->querybuf + cmd_pos, sizeof(cmd_opcode_raw));
        memcpy(&cmd_mux_id_raw, c->querybuf + cmd_pos + 2, sizeof(cmd_mux_id_raw));
        uint16_t cmd_opcode = ntohs(cmd_opcode_raw);
        uint16_t cmd_mux_id = ntohs(cmd_mux_id_raw);
        cmd_pos += RESPB_HEADER_SIZE;

        /* Store mux_id and opcode temporarily (needed by decoders) */
        uint16_t saved_opcode = c->respb_opcode;
        uint16_t saved_mux_id = c->respb_mux_id;
        c->respb_opcode = cmd_opcode;
        c->respb_mux_id = cmd_mux_id;

        /* Get encoding type */
        uint8_t cmd_enc_type = RESPB_ENC_GENERIC;
        switch (cmd_opcode) {
        case RESPB_OP_GET:
        case RESPB_OP_INCR:
            cmd_enc_type = RESPB_ENC_KEY_ONLY;
            break;
        case RESPB_OP_SET:
            cmd_enc_type = RESPB_ENC_KEY_VALUE;
            break;
        case RESPB_OP_LPUSH:
        case RESPB_OP_RPUSH:
            cmd_enc_type = RESPB_ENC_KEY_ELEMENTS;
            break;
        case RESPB_OP_LPOP:
        case RESPB_OP_RPOP:
            cmd_enc_type = RESPB_ENC_KEY_OPTCOUNT;
            break;
        case RESPB_OP_HSET:
            cmd_enc_type = RESPB_ENC_KEY_PAIRS;
            break;
        default:
            cmd_enc_type = RESPB_ENC_GENERIC;
            break;
        }

        /* Create parse context for queued command */
        respbParseContext queue_ctx = {
            .argc = &p->argc,
            .argv = &p->argv,
            .argv_len = &p->argv_len,
            .argv_len_sum = &p->argv_len_sum
        };

        /* Dispatch to decoder */
        size_t cmd_buflen = sdslen(c->querybuf);
        switch (cmd_enc_type) {
        case RESPB_ENC_KEY_ONLY:
            flag = respbDecodeKeyOnly(c, c->querybuf, cmd_buflen, &cmd_pos, &queue_ctx);
            break;
        case RESPB_ENC_KEY_VALUE:
            flag = respbDecodeSet(c, c->querybuf, cmd_buflen, &cmd_pos, &queue_ctx);
            break;
        case RESPB_ENC_KEY_ELEMENTS:
            flag = respbDecodeKeyElements(c, c->querybuf, cmd_buflen, &cmd_pos, &queue_ctx);
            break;
        case RESPB_ENC_KEY_OPTCOUNT:
            flag = respbDecodeKeyOptCount(c, c->querybuf, cmd_buflen, &cmd_pos, &queue_ctx);
            break;
        case RESPB_ENC_KEY_PAIRS:
            flag = respbDecodeKeyPairs(c, c->querybuf, cmd_buflen, &cmd_pos, &queue_ctx);
            break;
        case RESPB_ENC_GENERIC:
        default: {
            int cmd_fixed_argc = respbOpcodeFixedArgc(cmd_opcode);
            flag = respbDecodeGeneric(c, c->querybuf, cmd_buflen, &cmd_pos,
                                       cmd_fixed_argc, cmd_opcode, &queue_ctx);
            break;
        }
        }

        /* Restore client opcode/mux_id */
        c->respb_opcode = saved_opcode;
        c->respb_mux_id = saved_mux_id;

        /* Update parsed command metadata */
        p->read_flags = flag;
        p->input_bytes = (cmd_pos - c->qb_pos);
        p->cmd = respbOpcodeCommand(cmd_opcode);
        p->slot = -1;

        /* If parse failed or incomplete, remove from queue and stop */
        if (!(flag & READ_FLAGS_PARSING_COMPLETED)) {
            /* Free partially parsed command */
            if (p->argv) {
                for (int i = 0; i < p->argc; i++) {
                    if (p->argv[i]) decrRefCount(p->argv[i]);
                }
                zfree(p->argv);
            }
            queue->len--;
            break;
        }

        /* Update position for next iteration */
        c->qb_pos = cmd_pos;
    }

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
