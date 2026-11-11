#define _POSIX_C_SOURCE 200809L
#include "kv.h"
#include "crc32.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0, failures = 0;
static char dbpath[256];

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define RUN(fn) do { \
    int before = failures; \
    fresh_db(); \
    fn(); \
    printf("[%s] %s\n", failures == before ? "PASS" : "FAIL", #fn); \
} while (0)

/* ---------- helpers ---------- */

static void fresh_db(void)
{
    unlink(dbpath);
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.compact", dbpath);
    unlink(tmp);
}

static kv_status put_s(kv_t *kv, const char *k, const char *v)
{
    return kv_put(kv, k, strlen(k), v, strlen(v));
}

static int expect_val(kv_t *kv, const char *k, const char *want)
{
    void *v; size_t n;
    kv_status s = kv_get(kv, k, strlen(k), &v, &n);
    if (s != KV_OK) return 0;
    int ok = (n == strlen(want) && memcmp(v, want, n) == 0);
    free(v);
    return ok;
}

static int missing(kv_t *kv, const char *k)
{
    void *v; size_t n;
    return kv_get(kv, k, strlen(k), &v, &n) == KV_ERR_NOTFOUND;
}

static uint64_t file_size(void)
{
    struct stat st;
    return stat(dbpath, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static void flip_byte(uint64_t off)
{
    int fd = open(dbpath, O_RDWR);
    unsigned char b;
    if (pread(fd, &b, 1, (off_t)off) != 1) { close(fd); return; }
    b ^= 0xFF;
    if (pwrite(fd, &b, 1, (off_t)off) != 1) { /* ignore */ }
    close(fd);
}

/* ---------- tests ---------- */

static void test_crc32_known_vector(void)
{
    /* Standard check value for CRC-32/ISO-HDLC */
    CHECK(crc32_compute("123456789", 9) == 0xCBF43926u);
    CHECK(crc32_compute("", 0) == 0u);
}

static void test_basic_put_get_overwrite_delete(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(put_s(kv, "name", "alice") == KV_OK);
    CHECK(expect_val(kv, "name", "alice"));
    CHECK(put_s(kv, "name", "bob") == KV_OK);
    CHECK(expect_val(kv, "name", "bob"));
    CHECK(kv_del(kv, "name", 4) == KV_OK);
    CHECK(missing(kv, "name"));
    CHECK(kv_del(kv, "name", 4) == KV_ERR_NOTFOUND);
    CHECK(missing(kv, "never-existed"));
    kv_close(kv);
}

static void test_persistence_across_reopen(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(put_s(kv, "a", "1") == KV_OK);
    CHECK(put_s(kv, "b", "2") == KV_OK);
    CHECK(put_s(kv, "a", "3") == KV_OK);
    CHECK(put_s(kv, "c", "4") == KV_OK);
    CHECK(kv_del(kv, "b", 1) == KV_OK);
    kv_close(kv);

    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(expect_val(kv, "a", "3"));
    CHECK(missing(kv, "b"));            /* tombstone must survive restart */
    CHECK(expect_val(kv, "c", "4"));
    kv_stats_t st; kv_stats(kv, &st);
    CHECK(st.live_keys == 2);
    CHECK(st.recovered_records == 5);
    CHECK(st.discarded_bytes == 0);
    kv_close(kv);
}

static void test_binary_and_boundary_values(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);

    /* embedded NUL bytes in key and value */
    const char bk[] = {'k', 0, 'x'};
    const char bv[] = {0, 1, 2, 0, 3};
    CHECK(kv_put(kv, bk, sizeof bk, bv, sizeof bv) == KV_OK);
    void *v; size_t n;
    CHECK(kv_get(kv, bk, sizeof bk, &v, &n) == KV_OK);
    CHECK(n == sizeof bv && memcmp(v, bv, n) == 0);
    free(v);

    /* empty value is legal */
    CHECK(kv_put(kv, "empty", 5, "", 0) == KV_OK);
    CHECK(kv_get(kv, "empty", 5, &v, &n) == KV_OK && n == 0);
    free(v);

    /* limits */
    char *bigk = malloc(KV_MAX_KEY_LEN + 1);
    char *bigv = malloc(KV_MAX_VAL_LEN + 1);
    memset(bigk, 'k', KV_MAX_KEY_LEN + 1);
    memset(bigv, 'v', KV_MAX_VAL_LEN + 1);
    CHECK(kv_put(kv, bigk, KV_MAX_KEY_LEN, bigv, KV_MAX_VAL_LEN) == KV_OK);
    CHECK(kv_put(kv, bigk, KV_MAX_KEY_LEN + 1, "x", 1) == KV_ERR_INVALID);
    CHECK(kv_put(kv, "k", 1, bigv, KV_MAX_VAL_LEN + 1) == KV_ERR_INVALID);
    CHECK(kv_put(kv, "", 0, "x", 1) == KV_ERR_INVALID);
    CHECK(kv_put(kv, NULL, 1, "x", 1) == KV_ERR_INVALID);
    CHECK(kv_get(kv, bigk, KV_MAX_KEY_LEN, &v, &n) == KV_OK && n == KV_MAX_VAL_LEN);
    free(v);
    kv_close(kv);

    /* the maximum-size record must also replay correctly */
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(kv_get(kv, bigk, KV_MAX_KEY_LEN, &v, &n) == KV_OK && n == KV_MAX_VAL_LEN);
    free(v);
    kv_close(kv);
    free(bigk); free(bigv);
}

static void test_torn_tail_recovery(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(put_s(kv, "one", "first") == KV_OK);
    CHECK(put_s(kv, "two", "second") == KV_OK);
    kv_stats_t st; kv_stats(kv, &st);
    uint64_t good = st.file_bytes;
    CHECK(put_s(kv, "three", "third-record-will-be-torn") == KV_OK);
    kv_close(kv);

    /* simulate power loss mid-write: chop 7 bytes off the last record */
    CHECK(truncate(dbpath, (off_t)(file_size() - 7)) == 0);

    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(expect_val(kv, "one", "first"));
    CHECK(expect_val(kv, "two", "second"));
    CHECK(missing(kv, "three"));
    kv_stats(kv, &st);
    CHECK(st.discarded_bytes > 0);
    CHECK(st.file_bytes == good);
    CHECK(file_size() == good);                 /* garbage physically removed */

    /* the store must be fully usable afterwards */
    CHECK(put_s(kv, "three", "rewritten") == KV_OK);
    kv_close(kv);
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(expect_val(kv, "three", "rewritten"));
    CHECK(expect_val(kv, "one", "first"));
    kv_close(kv);
}

static void test_torn_header_only(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(put_s(kv, "keep", "me") == KV_OK);
    kv_stats_t st; kv_stats(kv, &st);
    uint64_t good = st.file_bytes;
    kv_close(kv);

    /* append 5 stray bytes (less than a header) */
    int fd = open(dbpath, O_WRONLY | O_APPEND);
    CHECK(write(fd, "\x01\x02\x03\x04\x05", 5) == 5);
    close(fd);

    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(expect_val(kv, "keep", "me"));
    kv_stats(kv, &st);
    CHECK(st.discarded_bytes == 5);
    CHECK(file_size() == good);
    kv_close(kv);
}

static void test_corruption_in_middle_stops_replay(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(put_s(kv, "k1", "v1") == KV_OK);
    CHECK(put_s(kv, "k2", "v2") == KV_OK);
    kv_stats_t st; kv_stats(kv, &st);
    uint64_t second_start = st.file_bytes;
    CHECK(put_s(kv, "k3", "v3") == KV_OK);
    CHECK(put_s(kv, "k4", "v4") == KV_OK);
    kv_close(kv);

    flip_byte(second_start + 14);   /* a payload byte of record 3 */

    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(expect_val(kv, "k1", "v1"));
    CHECK(expect_val(kv, "k2", "v2"));
    CHECK(missing(kv, "k3"));
    CHECK(missing(kv, "k4"));       /* nothing after the bad record is trusted */
    kv_stats(kv, &st);
    CHECK(st.discarded_bytes > 0);
    kv_close(kv);
}

static void test_get_detects_bit_rot(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(put_s(kv, "key", "precious-data") == KV_OK);
    CHECK(expect_val(kv, "key", "precious-data"));

    flip_byte(13 + 3 + 2);          /* a byte inside the value, while open */

    void *v; size_t n;
    CHECK(kv_get(kv, "key", 3, &v, &n) == KV_ERR_CORRUPT);
    kv_close(kv);
}

static void test_compaction(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    char k[32], v[64];
    for (int round = 0; round < 20; round++)
        for (int i = 0; i < 50; i++) {
            snprintf(k, sizeof k, "key%d", i);
            snprintf(v, sizeof v, "round-%d-value-%d", round, i);
            CHECK(put_s(kv, k, v) == KV_OK);
        }
    for (int i = 0; i < 50; i += 2) {
        snprintf(k, sizeof k, "key%d", i);
        CHECK(kv_del(kv, k, strlen(k)) == KV_OK);
    }

    kv_stats_t before, after;
    kv_stats(kv, &before);
    CHECK(before.garbage_bytes > before.live_bytes);   /* mostly garbage */
    CHECK(kv_compact(kv) == KV_OK);
    kv_stats(kv, &after);
    CHECK(after.file_bytes < before.file_bytes / 5);
    CHECK(after.garbage_bytes == 0);
    CHECK(after.live_keys == 25);
    CHECK(file_size() == after.file_bytes);

    for (int i = 0; i < 50; i++) {
        snprintf(k, sizeof k, "key%d", i);
        snprintf(v, sizeof v, "round-19-value-%d", i);
        if (i % 2 == 0) CHECK(missing(kv, k));
        else CHECK(expect_val(kv, k, v));
    }

    /* writes after compaction go to the new file and survive a reopen */
    CHECK(put_s(kv, "after", "compaction") == KV_OK);
    kv_close(kv);
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    CHECK(expect_val(kv, "after", "compaction"));
    CHECK(expect_val(kv, "key1", "round-19-value-1"));
    CHECK(missing(kv, "key0"));
    kv_stats(kv, &after);
    CHECK(after.live_keys == 26);
    kv_close(kv);
}

static void test_exclusive_lock(void)
{
    kv_t *a, *b = NULL;
    CHECK(kv_open(dbpath, 0, &a) == KV_OK);
    CHECK(kv_open(dbpath, 0, &b) == KV_ERR_LOCKED);
    CHECK(b == NULL);
    kv_close(a);
    CHECK(kv_open(dbpath, 0, &b) == KV_OK);   /* lock released on close */
    kv_close(b);
}

static void test_sync_mode(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 1, &kv) == KV_OK);
    CHECK(put_s(kv, "durable", "yes") == KV_OK);
    CHECK(kv_del(kv, "durable", 7) == KV_OK);
    CHECK(put_s(kv, "durable", "again") == KV_OK);
    CHECK(expect_val(kv, "durable", "again"));
    kv_close(kv);
}

static void test_many_keys_force_index_resize(void)
{
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    char k[32], v[32];
    for (int i = 0; i < 10000; i++) {
        snprintf(k, sizeof k, "k%d", i);
        snprintf(v, sizeof v, "v%d", i);
        CHECK(put_s(kv, k, v) == KV_OK);
    }
    kv_stats_t st; kv_stats(kv, &st);
    CHECK(st.live_keys == 10000);
    kv_close(kv);
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    for (int i = 0; i < 10000; i += 997) {
        snprintf(k, sizeof k, "k%d", i);
        snprintf(v, sizeof v, "v%d", i);
        CHECK(expect_val(kv, k, v));
    }
    kv_close(kv);
}

/* Randomised comparison against a trivial in-memory model, with periodic
 * restarts and compactions mixed in. */
#define NKEYS 300
static unsigned long long rng = 88172645463325252ULL;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng % n);
}

static void test_randomised_against_model(void)
{
    char *model[NKEYS] = {0};
    kv_t *kv;
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    char k[32], v[64];

    for (int step = 0; step < 30000; step++) {
        int id = (int)rnd(NKEYS);
        snprintf(k, sizeof k, "key-%d", id);
        unsigned op = rnd(100);

        if (op < 55) {
            snprintf(v, sizeof v, "val-%d-%u", step, rnd(100000));
            kv_status s = put_s(kv, k, v);
            if (s != KV_OK) { CHECK(s == KV_OK); break; }
            free(model[id]); model[id] = strdup(v);
        } else if (op < 75) {
            kv_status s = kv_del(kv, k, strlen(k));
            CHECK(s == (model[id] ? KV_OK : KV_ERR_NOTFOUND));
            free(model[id]); model[id] = NULL;
        } else if (op < 98) {
            if (model[id]) CHECK(expect_val(kv, k, model[id]));
            else CHECK(missing(kv, k));
        } else if (op == 98) {
            kv_close(kv);
            CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
        } else {
            CHECK(kv_compact(kv) == KV_OK);
        }
    }

    kv_close(kv);
    CHECK(kv_open(dbpath, 0, &kv) == KV_OK);
    size_t live = 0;
    for (int id = 0; id < NKEYS; id++) {
        snprintf(k, sizeof k, "key-%d", id);
        if (model[id]) { live++; CHECK(expect_val(kv, k, model[id])); }
        else CHECK(missing(kv, k));
        free(model[id]);
    }
    kv_stats_t st; kv_stats(kv, &st);
    CHECK(st.live_keys == live);
    kv_close(kv);
}

int main(void)
{
    snprintf(dbpath, sizeof dbpath, "/tmp/minikv_test_%d.db", (int)getpid());

    RUN(test_crc32_known_vector);
    RUN(test_basic_put_get_overwrite_delete);
    RUN(test_persistence_across_reopen);
    RUN(test_binary_and_boundary_values);
    RUN(test_torn_tail_recovery);
    RUN(test_torn_header_only);
    RUN(test_corruption_in_middle_stops_replay);
    RUN(test_get_detects_bit_rot);
    RUN(test_compaction);
    RUN(test_exclusive_lock);
    RUN(test_sync_mode);
    RUN(test_many_keys_force_index_resize);
    RUN(test_randomised_against_model);

    fresh_db();
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
