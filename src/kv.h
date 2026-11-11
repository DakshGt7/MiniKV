/*
 * MiniKV - a small persistent key-value store with a crash-safe
 * append-only log and an in-memory hash index.
 *
 * Keys and values are arbitrary byte strings (binary safe).
 * Single writer: the database file is protected by an advisory lock.
 */
#ifndef MINIKV_KV_H
#define MINIKV_KV_H

#include <stddef.h>
#include <stdint.h>

#define KV_MAX_KEY_LEN 1024u
#define KV_MAX_VAL_LEN (1024u * 1024u)

typedef enum {
    KV_OK = 0,
    KV_ERR_IO = -1,
    KV_ERR_NOTFOUND = -2,
    KV_ERR_INVALID = -3,
    KV_ERR_NOMEM = -4,
    KV_ERR_LOCKED = -5,
    KV_ERR_CORRUPT = -6
} kv_status;

typedef struct kv kv_t;

typedef struct {
    size_t   live_keys;           /* keys currently stored                   */
    uint64_t file_bytes;          /* size of the log file                    */
    uint64_t live_bytes;          /* bytes occupied by live records          */
    uint64_t garbage_bytes;       /* file_bytes - live_bytes (reclaimable)   */
    uint64_t recovered_records;   /* records replayed at open                */
    uint64_t discarded_bytes;     /* bytes cut from the tail during recovery */
} kv_stats_t;

typedef void (*kv_key_cb)(const char *key, size_t klen, size_t vlen, void *ctx);

/* Open (creating if needed) a database. sync != 0 => fsync after every write. */
kv_status kv_open(const char *path, int sync, kv_t **out);
void      kv_close(kv_t *kv);

kv_status kv_put(kv_t *kv, const void *key, size_t klen, const void *val, size_t vlen);
/* On success *val is malloc'd (caller frees); a NUL byte is appended for convenience. */
kv_status kv_get(kv_t *kv, const void *key, size_t klen, void **val, size_t *vlen);
kv_status kv_del(kv_t *kv, const void *key, size_t klen);

/* Rewrite the log keeping only live records; atomic via rename(). */
kv_status kv_compact(kv_t *kv);

void      kv_stats(const kv_t *kv, kv_stats_t *out);
void      kv_foreach(const kv_t *kv, kv_key_cb cb, void *ctx);

const char *kv_strerror(kv_status s);

#endif
