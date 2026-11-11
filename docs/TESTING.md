# MiniKV Test Plan and Results

Run with `make test` (and `make sanitize` for ASan + UBSan).
Latest result on Ubuntu 24.04, gcc 13.3: **13 groups, 25,070 checks, 0 failures**,
no sanitizer reports, zero compiler warnings with `-Wall -Wextra -Wpedantic`.

| # | Test | What it proves |
|---|---|---|
| 1 | CRC-32 known vector (`"123456789"` → `0xCBF43926`) | checksum implementation is correct |
| 2 | Put / get / overwrite / delete | core semantics, missing-key errors |
| 3 | Persistence across reopen | replay rebuilds identical state; tombstones survive restart |
| 4 | Binary keys/values, empty value, max sizes, oversize rejection | binary safety, boundary handling, input validation |
| 5 | Torn tail (last record cut by 7 bytes) | partial write dropped, file truncated, store still writable |
| 6 | Torn header (5 stray bytes) | sub-header garbage handled |
| 7 | Corruption in middle record | replay stops at bad CRC; later records not trusted |
| 8 | Bit rot while open | `get` returns `KV_ERR_CORRUPT`, never wrong data |
| 9 | Compaction | file shrinks >5x, data identical, works after reopen |
| 10 | Exclusive lock | second opener gets `KV_ERR_LOCKED`; released on close |
| 11 | Sync mode | `fsync` path works |
| 12 | 10,000 keys | hash index resizing is correct |
| 13 | Randomised model check | 30,000 random put/del/get ops with random reopen and compaction, compared against a reference model |

## Manual CLI checks

- `put`, `get`, `del`, `list`, `stats`, `compact`, `shell` behave as documented.
- Exit code `1` for a missing key, `2` for errors.
- Truncating a database file by hand and reopening prints a recovery warning and keeps all earlier data.

## Benchmark (single run, no `--sync`, 100,000 keys)

| Operation | Throughput |
|---|---|
| put | ~1.7 million ops/s |
| get (with CRC verification) | ~2.4 million ops/s |

Numbers vary by machine; use `./minikv db bench N` to measure on yours.
