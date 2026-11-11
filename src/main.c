#define _POSIX_C_SOURCE 200809L
#include "kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXIT_NOTFOUND 1
#define EXIT_FAIL 2

static void usage(const char *prog)
{
    fprintf(stderr,
        "MiniKV - persistent key-value store\n\n"
        "usage: %s [--sync] <dbfile> <command> [args]\n\n"
        "commands:\n"
        "  put <key> <value>   store a value\n"
        "  get <key>           print a value (exit 1 if missing)\n"
        "  del <key>           delete a key (exit 1 if missing)\n"
        "  list                list keys (sorted) with value sizes\n"
        "  stats               show storage statistics\n"
        "  compact             reclaim space used by overwritten/deleted keys\n"
        "  bench <n>           time n puts and n gets\n"
        "  shell               interactive prompt (put/get/del/list/stats/compact/quit)\n\n"
        "--sync fsyncs after every write (durable, slower).\n", prog);
}

typedef struct {
    char  **keys;
    size_t *vlens;
    size_t  n, cap;
} keylist_t;

static void collect(const char *key, size_t klen, size_t vlen, void *ctx)
{
    keylist_t *l = ctx;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        char **nk = realloc(l->keys, nc * sizeof *nk);
        size_t *nv = realloc(l->vlens, nc * sizeof *nv);
        if (!nk || !nv) { fprintf(stderr, "out of memory\n"); exit(EXIT_FAIL); }
        l->keys = nk; l->vlens = nv; l->cap = nc;
    }
    char *copy = malloc(klen + 1);
    if (!copy) { fprintf(stderr, "out of memory\n"); exit(EXIT_FAIL); }
    memcpy(copy, key, klen);
    copy[klen] = '\0';
    l->keys[l->n] = copy;
    l->vlens[l->n] = vlen;
    l->n++;
}

typedef struct { const char *key; size_t vlen; } pair_t;

static int cmp_pair(const void *a, const void *b)
{
    return strcmp(((const pair_t *)a)->key, ((const pair_t *)b)->key);
}

static int cmd_list(kv_t *kv)
{
    keylist_t l = {0};
    kv_foreach(kv, collect, &l);
    pair_t *p = malloc((l.n ? l.n : 1) * sizeof *p);
    if (!p) return EXIT_FAIL;
    for (size_t i = 0; i < l.n; i++) { p[i].key = l.keys[i]; p[i].vlen = l.vlens[i]; }
    qsort(p, l.n, sizeof *p, cmp_pair);
    for (size_t i = 0; i < l.n; i++)
        printf("%s\t%zu bytes\n", p[i].key, p[i].vlen);
    for (size_t i = 0; i < l.n; i++) free(l.keys[i]);
    free(l.keys); free(l.vlens); free(p);
    return 0;
}

static int cmd_stats(kv_t *kv)
{
    kv_stats_t s;
    kv_stats(kv, &s);
    printf("live keys          : %zu\n", s.live_keys);
    printf("log file size      : %llu bytes\n", (unsigned long long)s.file_bytes);
    printf("live data          : %llu bytes\n", (unsigned long long)s.live_bytes);
    printf("reclaimable garbage: %llu bytes", (unsigned long long)s.garbage_bytes);
    if (s.file_bytes)
        printf(" (%.1f%%)", 100.0 * (double)s.garbage_bytes / (double)s.file_bytes);
    printf("\nrecords replayed   : %llu\n", (unsigned long long)s.recovered_records);
    printf("tail bytes dropped : %llu\n", (unsigned long long)s.discarded_bytes);
    return 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cmd_bench(kv_t *kv, long n)
{
    char key[32], val[64];
    double t0 = now_sec();
    for (long i = 0; i < n; i++) {
        snprintf(key, sizeof key, "bench-key-%ld", i);
        int len = snprintf(val, sizeof val, "value-%ld-xxxxxxxxxxxxxxxx", i);
        kv_status s = kv_put(kv, key, strlen(key), val, (size_t)len);
        if (s != KV_OK) { fprintf(stderr, "put failed: %s\n", kv_strerror(s)); return EXIT_FAIL; }
    }
    double t1 = now_sec();
    for (long i = 0; i < n; i++) {
        snprintf(key, sizeof key, "bench-key-%ld", i);
        void *v; size_t vl;
        kv_status s = kv_get(kv, key, strlen(key), &v, &vl);
        if (s != KV_OK) { fprintf(stderr, "get failed: %s\n", kv_strerror(s)); return EXIT_FAIL; }
        free(v);
    }
    double t2 = now_sec();
    printf("%ld puts: %.3f s (%.0f ops/s)\n", n, t1 - t0, (double)n / (t1 - t0));
    printf("%ld gets: %.3f s (%.0f ops/s)\n", n, t2 - t1, (double)n / (t2 - t1));
    return 0;
}

/* Execute one command. Returns an exit code. */
static int run(kv_t *kv, int argc, char **argv)
{
    kv_status s;
    const char *cmd = argv[0];

    if (!strcmp(cmd, "put") && argc == 3) {
        s = kv_put(kv, argv[1], strlen(argv[1]), argv[2], strlen(argv[2]));
        if (s != KV_OK) { fprintf(stderr, "error: %s\n", kv_strerror(s)); return EXIT_FAIL; }
        return 0;
    }
    if (!strcmp(cmd, "get") && argc == 2) {
        void *v; size_t vl;
        s = kv_get(kv, argv[1], strlen(argv[1]), &v, &vl);
        if (s == KV_ERR_NOTFOUND) { fprintf(stderr, "(not found)\n"); return EXIT_NOTFOUND; }
        if (s != KV_OK) { fprintf(stderr, "error: %s\n", kv_strerror(s)); return EXIT_FAIL; }
        fwrite(v, 1, vl, stdout);
        fputc('\n', stdout);
        free(v);
        return 0;
    }
    if (!strcmp(cmd, "del") && argc == 2) {
        s = kv_del(kv, argv[1], strlen(argv[1]));
        if (s == KV_ERR_NOTFOUND) { fprintf(stderr, "(not found)\n"); return EXIT_NOTFOUND; }
        if (s != KV_OK) { fprintf(stderr, "error: %s\n", kv_strerror(s)); return EXIT_FAIL; }
        return 0;
    }
    if (!strcmp(cmd, "list") && argc == 1) return cmd_list(kv);
    if (!strcmp(cmd, "stats") && argc == 1) return cmd_stats(kv);
    if (!strcmp(cmd, "compact") && argc == 1) {
        kv_stats_t before, after;
        kv_stats(kv, &before);
        s = kv_compact(kv);
        if (s != KV_OK) { fprintf(stderr, "error: %s\n", kv_strerror(s)); return EXIT_FAIL; }
        kv_stats(kv, &after);
        printf("compacted: %llu -> %llu bytes\n",
               (unsigned long long)before.file_bytes, (unsigned long long)after.file_bytes);
        return 0;
    }
    if (!strcmp(cmd, "bench") && argc == 2) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (*end || n <= 0) { fprintf(stderr, "bench needs a positive count\n"); return EXIT_FAIL; }
        return cmd_bench(kv, n);
    }
    fprintf(stderr, "unknown command or wrong arguments: %s\n", cmd);
    return EXIT_FAIL;
}

static int shell(kv_t *kv)
{
    char line[KV_MAX_KEY_LEN + 4096];
    printf("minikv> ");
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *argv[3];
        int argc = 0;
        char *p = line;
        while (*p == ' ') p++;
        if (!*p) goto prompt;
        if (!strcmp(p, "quit") || !strcmp(p, "exit")) break;

        argv[argc++] = p;
        p = strchr(p, ' ');
        if (p) {
            *p++ = '\0';
            while (*p == ' ') p++;
            if (*p) {
                argv[argc++] = p;
                /* for "put", the value is the remainder of the line */
                if (strcmp(argv[0], "put") == 0) {
                    char *sp = strchr(p, ' ');
                    if (sp) {
                        *sp++ = '\0';
                        while (*sp == ' ') sp++;
                        if (*sp) argv[argc++] = sp;
                    }
                } else {
                    char *sp = strchr(p, ' ');
                    if (sp) *sp = '\0';
                }
            }
        }
        run(kv, argc, argv);
    prompt:
        printf("minikv> ");
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int sync = 0, a = 1;
    if (a < argc && !strcmp(argv[a], "--sync")) { sync = 1; a++; }
    if (argc - a < 2) { usage(argv[0]); return EXIT_FAIL; }

    const char *path = argv[a++];
    kv_t *kv;
    kv_status s = kv_open(path, sync, &kv);
    if (s != KV_OK) {
        fprintf(stderr, "cannot open '%s': %s\n", path, kv_strerror(s));
        return EXIT_FAIL;
    }

    kv_stats_t st;
    kv_stats(kv, &st);
    if (st.discarded_bytes)
        fprintf(stderr, "warning: recovered from incomplete/corrupt tail, dropped %llu bytes\n",
                (unsigned long long)st.discarded_bytes);

    int rc;
    if (!strcmp(argv[a], "shell")) rc = shell(kv);
    else rc = run(kv, argc - a, argv + a);

    kv_close(kv);
    return rc;
}
