#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_format.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef DBCE_TESTING
extern int dbce_test_migration_header(void);
#endif

static bool result(int error) {
    if (error) errno = error;
    return error == 0;
}

/* Preflight for useful diagnostics. Publication independently enforces no replacement. */
static int unused(const char* path) {
    struct stat st;
    if (!stat(path, &st)) return EEXIST;
    return errno == ENOENT ? 0 : dbce_io_error();
}

typedef struct { FILE* source; FILE* output; } migration;

static int compare_bytes(FILE* src, FILE* dst, int64_t n) {
    int error;
    unsigned char left[16384], right[sizeof(left)];
    while (n) {
        size_t count = n < (int64_t)sizeof(left) ? (size_t)n : sizeof(left);
        if ((error = dbce_read_exact(src, left, count)) || (error = dbce_read_exact(dst, right, count)))
            return error;
        if (memcmp(left, right, count)) return EIO;
        n -= (int64_t)count;
    }
    return 0;
}

static int convert_record(const log_record* rec, void* arg) {
    migration* m = arg;
    unsigned char header[V2_RECORD_BYTES];
    uint32_t crc = dbce_v2_prefix(header, rec->op, rec->key, rec->length);
    if (file_seek(m->source, rec->offset, SEEK_SET)) return dbce_io_error();
    int error = dbce_stream_bytes(m->source, NULL, rec->length, &crc);
    if (error) return error;
    dbce_v2_finish_header(header, crc);
    if ((error = dbce_write_exact(m->output, header, sizeof(header)))) return error;
#ifdef DBCE_TESTING
    if ((error = dbce_test_migration_header())) return error;
#endif
    if ((error = dbce_write_exact(m->output, rec->key, strlen(rec->key)))) return error;
    if (file_seek(m->source, rec->offset, SEEK_SET)) return dbce_io_error();
    return dbce_stream_bytes(m->source, m->output, rec->length, NULL);
}

/* Walk verified V2 records and consume the V1 stream independently, in order. */
static int compare_record(const log_record* rec, void* arg) {
    migration* m = arg;
    dbce_record_header_t old;
    int error = dbce_read_exact(m->source, &old, sizeof(old));
    if (error) return error;
    if (old.magic != DBCE_MAGIC || old.key_len != strlen(rec->key) ||
        old.val_len != rec->length || rec->op != (old.val_len ? OP_SET : OP_DELETE)) return EILSEQ;
    char key[DBCE_MAX_KEY];
    if ((error = dbce_read_exact(m->source, key, old.key_len))) return error;
    if (memcmp(key, rec->key, old.key_len)) return EILSEQ;
    if (file_seek(m->output, rec->offset, SEEK_SET)) return dbce_io_error();
    return compare_bytes(m->source, m->output, rec->length);
}

bool dbce_migrate(const char* source, const char* dest) {
    if (!source || !*source || !dest || !*dest) return result(EINVAL);
    FILE* fp = dbce_locked_existing(source, false);
    if (!fp) return false;
    stage_t stage = {0};
    log_info info;
    int error = unused(dest);
    if (!error) error = dbce_validate_log(fp, &info);
    if (!error && info.v2) error = ENOTSUP;
    if (!error) error = dbce_stage_create(&stage, dest);
    if (!error) error = dbce_write_v2_file_header(stage.fp);
    migration m = {fp, stage.fp};
    if (!error) error = dbce_walk_log(fp, &info, convert_record, &m);
    if (!error && file_seek(fp, 0, SEEK_SET)) error = dbce_io_error();
    int64_t source_size = error ? 0 : info.end;
    if (!error) error = dbce_walk_log(stage.fp, &info, compare_record, &m);
    if (!error && (!info.v2 || file_tell(fp) != source_size)) error = EILSEQ;
    if (!error) error = dbce_stage_publish(&stage, dest);
    error = dbce_stage_finish(&stage, error);
    if (fclose(fp) && !error) error = dbce_io_error();
    return result(error);
}

/* Verify exact bytes, including an incomplete tail in the backup. */
static int copy_verified(FILE* src, FILE* dst, int64_t n) {
    if (file_seek(src, 0, SEEK_SET)) return dbce_io_error();
    int error = dbce_stream_bytes(src, dst, (uint64_t)n, NULL);
    if (error) return error;
    if (file_seek(dst, 0, SEEK_SET) || file_seek(src, 0, SEEK_SET)) return dbce_io_error();
    return compare_bytes(src, dst, n);
}

static int recover_copy(FILE* src, const char* dest, const log_info* info) {
    size_t len = strlen(dest);
    if (len > SIZE_MAX - 5) return ENAMETOOLONG;
    char* backup = malloc(len + 5);
    if (!backup) return ENOMEM;
    memcpy(backup, dest, len);
    memcpy(backup + len, ".bak", 5);
    stage_t output = {0}, original = {0};
    log_info check;
    int error = unused(dest);
    if (!error) error = unused(backup);
    if (!error) error = dbce_stage_create(&output, dest);
    if (!error) error = copy_verified(src, output.fp, info->verified);
    if (!error) error = dbce_validate_log(output.fp, &check);
    if (!error) error = dbce_stage_create(&original, backup);
    if (!error) error = copy_verified(src, original.fp, info->end);
    if (!error) error = dbce_stage_publish(&original, backup);
    if (!error) error = dbce_stage_publish(&output, dest);
    error = dbce_stage_finish(&original, error);
    error = dbce_stage_finish(&output, error);
    free(backup);
    return error;
}

bool dbce_recover(const char* source, const char* dest, dbce_recovery_t* report) {
    if (!source || !*source || !report || (dest && !*dest)) return result(EINVAL);
    *report = (dbce_recovery_t){0};
    FILE* fp = dbce_locked_existing(source, false);
    if (!fp) return false;
    log_info info = {0};
    int error = dbce_validate_log(fp, &info);
    report->issue = info.issue;
    report->verified_bytes = (uint64_t)info.verified;
    if (!error && !info.v2) error = ENOTSUP;
    if (info.v2 && error == EILSEQ && info.tail != DBCE_TAIL_NONE) {
        report->tail = info.tail;
        report->discarded_bytes = (uint64_t)(info.end - info.verified);
        error = dest ? recover_copy(fp, dest, &info) : 0;
    } else if (!error && dest) error = EINVAL; /* Nothing to recover. */
    if (fclose(fp) && !error) error = dbce_io_error();
    return result(error);
}
