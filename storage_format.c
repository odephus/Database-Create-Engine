#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_format.h"
#include <errno.h>
#include <string.h>

static const unsigned char file_header[V2_FILE_BYTES] = {
    0x89,'D','B','2','\r','\n',0x1a,'\n', 2,0, 16,0, 0,0,0,0
};
static const unsigned char record_magic[4] = {'D','R','2','!'};

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(unsigned char* p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8*i));
}

/* Reflected CRC-32/ISO-HDLC, four bits per step. API accepts finalized CRCs. */
uint32_t dbce_crc32_update(uint32_t crc, const void* data, size_t len) {
    static const uint32_t nibble[16] = {
        0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
        0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c
    };
    const unsigned char* p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ nibble[crc & 15];
        crc = (crc >> 4) ^ nibble[crc & 15];
    }
    return ~crc;
}

int dbce_write_v2_file_header(FILE* fp) { return dbce_write_exact(fp, file_header, sizeof(file_header)); }

uint32_t dbce_v2_prefix(unsigned char h[V2_RECORD_BYTES], unsigned char op, const char* key, uint32_t len) {
    size_t k = strlen(key);
    memset(h, 0, V2_RECORD_BYTES);
    memcpy(h, record_magic, 4);
    h[4] = op;
    put32(h+8, (uint32_t)k);
    put32(h+12, len);
    return dbce_crc32_update(dbce_crc32_update(0, h, 16), key, k);
}

void dbce_v2_finish_header(unsigned char h[V2_RECORD_BYTES], uint32_t crc) {
    put32(h+16, crc);
    put32(h+20, dbce_crc32_update(0, h, 20));
}

int dbce_stream_bytes(FILE* src, FILE* dest, uint64_t count, uint32_t* crc) {
    unsigned char buf[32768];
    while (count) {
        size_t n = count < sizeof(buf) ? (size_t)count : sizeof(buf);
        int error = dbce_read_exact(src, buf, n);
        if (!error && dest) error = dbce_write_exact(dest, buf, n);
        if (error) return error;
        if (crc) *crc = dbce_crc32_update(*crc, buf, n);
        count -= n;
    }
    return 0;
}

static int invalid(log_info* info, dbce_issue_t issue) {
    info->issue = issue;
    if (issue == DBCE_ISSUE_VERSION || issue == DBCE_ISSUE_OPERATION) return ENOTSUP;
    if (issue == DBCE_ISSUE_HEADER_CRC || issue == DBCE_ISSUE_RECORD_CRC) return EBADMSG;
    return EILSEQ;
}

static int identify(FILE* fp, log_info* info) {
    if (file_seek(fp, 0, SEEK_END)) return dbce_io_error();
    info->end = file_tell(fp);
    if (info->end < 0 || file_seek(fp, 0, SEEK_SET)) return dbce_io_error();
    unsigned char bytes[V2_FILE_BYTES];
    size_t n = info->end < V2_FILE_BYTES ? (size_t)info->end : V2_FILE_BYTES;
    int error = dbce_read_exact(fp, bytes, n);
    if (error) return error;
    if (!n || bytes[0] != file_header[0]) return file_seek(fp, 0, SEEK_SET) ? dbce_io_error() : 0;
    if (n < V2_FILE_BYTES) return invalid(info, DBCE_ISSUE_FILE_HEADER);
    if (memcmp(bytes, file_header, 8)) return invalid(info, DBCE_ISSUE_MAGIC);
    if (memcmp(bytes+8, file_header+8, 2)) return invalid(info, DBCE_ISSUE_VERSION);
    if (memcmp(bytes+10, file_header+10, 6)) return invalid(info, DBCE_ISSUE_FIELDS);
    info->v2 = true;
    info->verified = V2_FILE_BYTES;
    return 0;
}

/* For a short header reject every already-present field inconsistent with a legal header. */
static int check_header(log_info* info, const unsigned char* h, size_t n) {
    if (memcmp(h, record_magic, n < 4 ? n : 4)) return invalid(info, DBCE_ISSUE_MAGIC);
    if (n > 4 && h[4] != OP_SET && h[4] != OP_DELETE) return invalid(info, DBCE_ISSUE_OPERATION);
    for (size_t i = 5; i < n && i < 8; ++i) if (h[i]) return invalid(info, DBCE_ISSUE_FIELDS);
    if (n > 8 && (!h[8] || h[8] > DBCE_MAX_KEY)) return invalid(info, DBCE_ISSUE_LENGTH);
    for (size_t i = 9; i < n && i < 12; ++i) if (h[i]) return invalid(info, DBCE_ISSUE_LENGTH);
    if (n > 12 && h[4] == OP_DELETE) {
        for (size_t i = 12; i < n && i < 16; ++i) if (h[i]) return invalid(info, DBCE_ISSUE_LENGTH);
    }
    if (n > 20) {
        unsigned char expected[4];
        put32(expected, dbce_crc32_update(0, h, 20));
        if (memcmp(h+20, expected, n-20)) return invalid(info, DBCE_ISSUE_HEADER_CRC);
    }
    return 0;
}

static int partial(log_info* info, dbce_tail_t why) {
    info->tail = why;
    return EILSEQ;
}

static int read_record(FILE* fp, log_info* info, log_record* rec) {
    int64_t remaining = info->end - info->verified;
    unsigned char h[V2_RECORD_BYTES];
    uint32_t keylen;
    int64_t header_size;
    int error;
    if (info->v2) {
        header_size = V2_RECORD_BYTES;
        size_t n = remaining < header_size ? (size_t)remaining : V2_RECORD_BYTES;
        if ((error = dbce_read_exact(fp, h, n)) || (error = check_header(info, h, n))) return error;
        if (n < V2_RECORD_BYTES) return partial(info, DBCE_TAIL_HEADER);
        keylen = get32(h+8);
        rec->length = get32(h+12);
        rec->op = h[4];
    } else {
        dbce_record_header_t old;
        header_size = (int64_t)sizeof(old);
        if (remaining < header_size) return partial(info, DBCE_TAIL_HEADER);
        if ((error = dbce_read_exact(fp, &old, sizeof(old)))) return error;
        if (old.magic != DBCE_MAGIC) return invalid(info, DBCE_ISSUE_MAGIC);
        if (!old.key_len || old.key_len > DBCE_MAX_KEY) return invalid(info, DBCE_ISSUE_LENGTH);
        keylen = old.key_len;
        rec->length = old.val_len;
        rec->op = old.val_len ? OP_SET : OP_DELETE;
    }
#if SIZE_MAX <= UINT32_MAX
    if (rec->length >= SIZE_MAX) return EOVERFLOW;
#endif
    remaining -= header_size;
    size_t n = remaining < keylen ? (size_t)remaining : keylen;
    if ((error = dbce_read_exact(fp, rec->key, n))) return error;
    if (memchr(rec->key, 0, n)) return invalid(info, DBCE_ISSUE_KEY);
    if (n < keylen) return partial(info, DBCE_TAIL_KEY);
    rec->key[n] = 0;
    remaining -= keylen;
    rec->offset = info->end - remaining;
    if (remaining < rec->length) return partial(info, DBCE_TAIL_VALUE);
    if (!info->v2) return file_seek(fp, rec->offset + rec->length, SEEK_SET) ? dbce_io_error() : 0;
    uint32_t crc = dbce_crc32_update(dbce_crc32_update(0, h, 16), rec->key, keylen);
    if ((error = dbce_stream_bytes(fp, NULL, rec->length, &crc))) return error;
    return crc == get32(h+16) ? 0 : invalid(info, DBCE_ISSUE_RECORD_CRC);
}

int dbce_walk_log(FILE* fp, log_info* info, record_visitor visitor, void* context) {
    *info = (log_info){0};
    int error = identify(fp, info);
    while (!error && info->verified < info->end) {
        log_record rec;
        error = read_record(fp, info, &rec);
        if (!error && visitor) error = visitor(&rec, context);
        if (!error) info->verified = rec.offset + rec.length;
    }
    return error;
}
