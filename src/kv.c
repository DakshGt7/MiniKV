#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "kv.h"
#include "crc32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * On-disk record (little endian):
 *   u32 crc32   over every byte that follows the crc field
 *   u32 klen
 *   u32 vlen
 *   u8  flag    0 = PUT, 1 = DELETE (tombstone, vlen must be 0)
 *   key[klen]
 *   val[vlen]
 */
#define HDR_SIZE 13u
#define FLAG_PUT 0u
#define FLAG_DEL 1u
#define INITIAL_BUCKETS 64u

typedef struct node {
    struct node *next;
    uint64_t rec_off;   /* offset of the record header in the log */
    uint64_t new_off;   /* scratch used while compacting          */
    uint32_t klen;
    uint32_t vlen;
    uint32_t hash;
    char     key[];
} node_t;

struct kv {
    int       fd;
    char     *path;
    int       sync;
    node_t  **buckets;
    size_t    nbuckets;
    size_t    count;
    uint64_t  end;          /* current end of log (next append offset) */
    uint64_t  live_bytes;
    uint64_t  recovered_records;
    uint64_t  discarded_bytes;
};

/* ---------- small helpers ---------- */

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fnv1a(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint64_t rec_size(uint32_t klen, uint32_t vlen)
{
    return (uint64_t)HDR_SIZE + klen + vlen;
}

static int pread_full(int fd, void *buf, size_t n, uint64_t off)
{
    unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1; /* unexpected EOF */
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t n, uint64_t off)
{
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, (off_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w; off += (uint64_t)w;
    }
    return 0;
}

static int valid_args(const void *key, size_t klen, size_t vlen)
{
    return key != NULL && klen > 0 && klen <= KV_MAX_KEY_LEN && vlen <= KV_MAX_VAL_LEN;
}

/* ---------- in-memory hash index (separate chaining) ---------- */

static kv_status idx_resize(kv_t *kv, size_t nb)
{
    node_t **nbk = calloc(nb, sizeof *nbk);
    if (!nbk) return KV_ERR_NOMEM;
    for (size_t i = 0; i < kv->nbuckets; i++) {
        node_t *n = kv->buckets[i];
        while (n) {
            node_t *next = n->next;
            size_t b = n->hash % nb;
            n->next = nbk[b];
            nbk[b] = n;
            n = next;
        }
    }
    free(kv->buckets);
    kv->buckets = nbk;
    kv->nbuckets = nb;
    return KV_OK;
}

static node_t *idx_find(const kv_t *kv, const void *key, size_t klen, uint32_t h)
{
    for (node_t *n = kv->buckets[h % kv->nbuckets]; n; n = n->next)
        if (n->hash == h && n->klen == klen && memcmp(n->key, key, klen) == 0)
            return n;
    return NULL;
}

static kv_status idx_set(kv_t *kv, const void *key, size_t klen, uint32_t h,
                         uint64_t rec_off, uint32_t vlen)
{
    node_t *n = idx_find(kv, key, klen, h);
    if (n) {
        kv->live_bytes -= rec_size(n->klen, n->vlen);
        n->rec_off = rec_off;
        n->vlen = vlen;
        kv->live_bytes += rec_size(n->klen, n->vlen);
        return KV_OK;
    }
    if (kv->count + 1 > kv->nbuckets * 3 / 4) {
        kv_status s = idx_resize(kv, kv->nbuckets * 2);
        if (s != KV_OK) return s;
    }
    n = malloc(sizeof *n + klen);
    if (!n) return KV_ERR_NOMEM;
    n->klen = (uint32_t)klen;
    n->vlen = vlen;
    n->hash = h;
    n->rec_off = rec_off;
    n->new_off = 0;
    memcpy(n->key, key, klen);
    size_t b = h % kv->nbuckets;
    n->next = kv->buckets[b];
    kv->buckets[b] = n;
    kv->count++;
    kv->live_bytes += rec_size(n->klen, n->vlen);
    return KV_OK;
}

static void idx_remove(kv_t *kv, const void *key, size_t klen, uint32_t h)
{
    node_t **pp = &kv->buckets[h % kv->nbuckets];
    while (*pp) {
        node_t *n = *pp;
        if (n->hash == h && n->klen == klen && memcmp(n->key, key, klen) == 0) {
            *pp = n->next;
            kv->live_bytes -= rec_size(n->klen, n->vlen);
            kv->count--;
            free(n);
            return;
        }
        pp = &n->next;
    }
}

static void idx_free(kv_t *kv)
{
    for (size_t i = 0; i < kv->nbuckets; i++) {
        node_t *n = kv->buckets[i];
        while (n) {
            node_t *next = n->next;
            free(n);
            n = next;
        }
    }
    free(kv->buckets);
    kv->buckets = NULL;
    kv->nbuckets = 0;
    kv->count = 0;
}

/* ---------- log record I/O ---------- */

/* Build a full record in a malloc'd buffer. */
static unsigned char *build_record(unsigned flag, const void *key, uint32_t klen,
                                   const void *val, uint32_t vlen, size_t *total)
{
    size_t n = (size_t)rec_size(klen, vlen);
    unsigned char *buf = malloc(n);
    if (!buf) return NULL;
    put_u32(buf + 4, klen);
    put_u32(buf + 8, vlen);
    buf[12] = (unsigned char)flag;
    memcpy(buf + HDR_SIZE, key, klen);
    if (vlen) memcpy(buf + HDR_SIZE + klen, val, vlen);
    put_u32(buf, crc32_compute(buf + 4, n - 4));
    *total = n;
    return buf;
}

static kv_status append_record(kv_t *kv, unsigned flag, const void *key, uint32_t klen,
                               const void *val, uint32_t vlen, uint64_t *off_out)
{
    size_t n;
    unsigned char *buf = build_record(flag, key, klen, val, vlen, &n);
    if (!buf) return KV_ERR_NOMEM;

    if (pwrite_full(kv->fd, buf, n, kv->end) != 0 ||
        (kv->sync && fsync(kv->fd) != 0)) {
        /* Do not leave a half-written record behind. */
        if (ftruncate(kv->fd, (off_t)kv->end) != 0) { /* best effort */ }
        free(buf);
        return KV_ERR_IO;
    }
    free(buf);
    *off_out = kv->end;
    kv->end += n;
    return KV_OK;
}

/*
 * Read and verify the record at 'off'. Returns a malloc'd buffer holding the
 * whole record (header + key + value) or NULL; *status says why.
 */
static unsigned char *read_record(int fd, uint64_t off, uint32_t klen, uint32_t vlen,
                                  kv_status *status)
{
    size_t n = (size_t)rec_size(klen, vlen);
    unsigned char *buf = malloc(n);
    if (!buf) { *status = KV_ERR_NOMEM; return NULL; }
    if (pread_full(fd, buf, n, off) != 0) {
        free(buf); *status = KV_ERR_IO; return NULL;
    }
    if (get_u32(buf) != crc32_compute(buf + 4, n - 4)) {
        free(buf); *status = KV_ERR_CORRUPT; return NULL;
    }
    *status = KV_OK;
    return buf;
}

/* Replay the log to rebuild the index; truncate anything after the first bad record. */
static kv_status recover(kv_t *kv)
{
    struct stat st;
    if (fstat(kv->fd, &st) != 0) return KV_ERR_IO;
    uint64_t size = (uint64_t)st.st_size;
    uint64_t off = 0;

    while (off + HDR_SIZE <= size) {
        unsigned char hdr[HDR_SIZE];
        if (pread_full(kv->fd, hdr, HDR_SIZE, off) != 0) break;
        uint32_t klen = get_u32(hdr + 4);
        uint32_t vlen = get_u32(hdr + 8);
        unsigned flag = hdr[12];

        if (klen == 0 || klen > KV_MAX_KEY_LEN || vlen > KV_MAX_VAL_LEN) break;
        if (flag != FLAG_PUT && flag != FLAG_DEL) break;
        if (flag == FLAG_DEL && vlen != 0) break;
        if (off + rec_size(klen, vlen) > size) break;          /* torn tail */

        kv_status s;
        unsigned char *rec = read_record(kv->fd, off, klen, vlen, &s);
        if (!rec) {
            if (s == KV_ERR_CORRUPT) break;
            return s;
        }
        const unsigned char *key = rec + HDR_SIZE;
        uint32_t h = fnv1a(key, klen);
        if (flag == FLAG_PUT)
            s = idx_set(kv, key, klen, h, off, vlen);
        else {
            idx_remove(kv, key, klen, h);
            s = KV_OK;
        }
        free(rec);
        if (s != KV_OK) return s;

        kv->recovered_records++;
        off += rec_size(klen, vlen);
    }

    if (off < size) {
        kv->discarded_bytes = size - off;
        if (ftruncate(kv->fd, (off_t)off) != 0) return KV_ERR_IO;
        if (fsync(kv->fd) != 0) return KV_ERR_IO;
    }
    kv->end = off;
    return KV_OK;
}

/* ---------- public API ---------- */

kv_status kv_open(const char *path, int sync, kv_t **out)
{
    if (!path || !out) return KV_ERR_INVALID;
    *out = NULL;

    kv_t *kv = calloc(1, sizeof *kv);
    if (!kv) return KV_ERR_NOMEM;
    kv->sync = sync;
    kv->path = strdup(path);
    kv->fd = -1;
    if (!kv->path) { free(kv); return KV_ERR_NOMEM; }

    kv->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (kv->fd < 0) { free(kv->path); free(kv); return KV_ERR_IO; }

    if (flock(kv->fd, LOCK_EX | LOCK_NB) != 0) {
        kv_status s = (errno == EWOULDBLOCK) ? KV_ERR_LOCKED : KV_ERR_IO;
        close(kv->fd); free(kv->path); free(kv);
        return s;
    }

    kv->buckets = calloc(INITIAL_BUCKETS, sizeof *kv->buckets);
    kv->nbuckets = INITIAL_BUCKETS;
    if (!kv->buckets) { kv_close(kv); return KV_ERR_NOMEM; }

    kv_status s = recover(kv);
    if (s != KV_OK) { kv_close(kv); return s; }

    *out = kv;
    return KV_OK;
}

void kv_close(kv_t *kv)
{
    if (!kv) return;
    if (kv->fd >= 0) {
        fsync(kv->fd);
        close(kv->fd);      /* releases the flock */
    }
    idx_free(kv);
    free(kv->path);
    free(kv);
}

kv_status kv_put(kv_t *kv, const void *key, size_t klen, const void *val, size_t vlen)
{
    if (!kv || !valid_args(key, klen, vlen) || (vlen > 0 && !val)) return KV_ERR_INVALID;
    uint64_t off;
    kv_status s = append_record(kv, FLAG_PUT, key, (uint32_t)klen, val, (uint32_t)vlen, &off);
    if (s != KV_OK) return s;
    s = idx_set(kv, key, klen, fnv1a(key, klen), off, (uint32_t)vlen);
    if (s != KV_OK) {
        /* The record is durable but the index could not take it (OOM). The
         * caller should reopen the database; report the failure honestly. */
        return s;
    }
    return KV_OK;
}

kv_status kv_get(kv_t *kv, const void *key, size_t klen, void **val, size_t *vlen)
{
    if (!kv || !valid_args(key, klen, 0) || !val || !vlen) return KV_ERR_INVALID;
    node_t *n = idx_find(kv, key, klen, fnv1a(key, klen));
    if (!n) return KV_ERR_NOTFOUND;

    kv_status s;
    unsigned char *rec = read_record(kv->fd, n->rec_off, n->klen, n->vlen, &s);
    if (!rec) return s;

    unsigned char *out = malloc((size_t)n->vlen + 1);
    if (!out) { free(rec); return KV_ERR_NOMEM; }
    memcpy(out, rec + HDR_SIZE + n->klen, n->vlen);
    out[n->vlen] = '\0';
    free(rec);
    *val = out;
    *vlen = n->vlen;
    return KV_OK;
}

kv_status kv_del(kv_t *kv, const void *key, size_t klen)
{
    if (!kv || !valid_args(key, klen, 0)) return KV_ERR_INVALID;
    uint32_t h = fnv1a(key, klen);
    if (!idx_find(kv, key, klen, h)) return KV_ERR_NOTFOUND;

    uint64_t off;
    kv_status s = append_record(kv, FLAG_DEL, key, (uint32_t)klen, NULL, 0, &off);
    if (s != KV_OK) return s;
    idx_remove(kv, key, klen, h);
    return KV_OK;
}

kv_status kv_compact(kv_t *kv)
{
    if (!kv) return KV_ERR_INVALID;

    size_t plen = strlen(kv->path);
    char *tmp = malloc(plen + 9);
    if (!tmp) return KV_ERR_NOMEM;
    memcpy(tmp, kv->path, plen);
    memcpy(tmp + plen, ".compact", 9);

    int nfd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (nfd < 0) { free(tmp); return KV_ERR_IO; }
    if (flock(nfd, LOCK_EX | LOCK_NB) != 0) {
        close(nfd); unlink(tmp); free(tmp);
        return KV_ERR_IO;
    }

    kv_status st = KV_OK;
    uint64_t off = 0;

    for (size_t i = 0; i < kv->nbuckets && st == KV_OK; i++) {
        for (node_t *n = kv->buckets[i]; n; n = n->next) {
            unsigned char *rec = read_record(kv->fd, n->rec_off, n->klen, n->vlen, &st);
            if (!rec) break;   /* st holds the reason; abort, original untouched */
            size_t sz = (size_t)rec_size(n->klen, n->vlen);
            if (pwrite_full(nfd, rec, sz, off) != 0) st = KV_ERR_IO;
            free(rec);
            if (st != KV_OK) break;
            n->new_off = off;
            off += sz;
        }
    }

    if (st == KV_OK && fsync(nfd) != 0) st = KV_ERR_IO;
    if (st == KV_OK && rename(tmp, kv->path) != 0) st = KV_ERR_IO;

    if (st != KV_OK) {
        close(nfd);
        unlink(tmp);
        free(tmp);
        return st;
    }

    /* Switch over: the new file now lives at kv->path. */
    for (size_t i = 0; i < kv->nbuckets; i++)
        for (node_t *n = kv->buckets[i]; n; n = n->next)
            n->rec_off = n->new_off;

    close(kv->fd);
    kv->fd = nfd;
    kv->end = off;
    free(tmp);
    return KV_OK;
}

void kv_stats(const kv_t *kv, kv_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!kv) return;
    out->live_keys = kv->count;
    out->file_bytes = kv->end;
    out->live_bytes = kv->live_bytes;
    out->garbage_bytes = kv->end - kv->live_bytes;
    out->recovered_records = kv->recovered_records;
    out->discarded_bytes = kv->discarded_bytes;
}

void kv_foreach(const kv_t *kv, kv_key_cb cb, void *ctx)
{
    if (!kv || !cb) return;
    for (size_t i = 0; i < kv->nbuckets; i++)
        for (node_t *n = kv->buckets[i]; n; n = n->next)
            cb(n->key, n->klen, n->vlen, ctx);
}

const char *kv_strerror(kv_status s)
{
    switch (s) {
    case KV_OK:           return "ok";
    case KV_ERR_IO:       return "I/O error";
    case KV_ERR_NOTFOUND: return "key not found";
    case KV_ERR_INVALID:  return "invalid argument (empty/oversized key or value)";
    case KV_ERR_NOMEM:    return "out of memory";
    case KV_ERR_LOCKED:   return "database is locked by another process";
    case KV_ERR_CORRUPT:  return "data corruption detected (checksum mismatch)";
    }
    return "unknown error";
}
