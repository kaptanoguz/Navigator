# Phase 7 ops engine: copy throughput

Measured with `navigator_ops_tests "[.perf]"` (hidden tests in `tests/ops/perf_test.cpp`), Release
build, GCC 13.3, Linux 6.8, 8 cores, NVMe, fixtures under `/tmp` (same disk as `/`, ext4-class
filesystem) and `/dev/shm` (tmpfs). Every byte is written inside a fixture that is deleted
afterwards. One run, warm page cache; numbers are indicative, not a benchmark suite.

| Case | Time | Throughput |
|------|------|------------|
| copy 1 GiB file, same fs (default, no fsync) | 0.47 s | 2192 MiB/s |
| baseline `cp --reflink=never` 1 GiB | 0.50 s | 2059 MiB/s |
| copy 1 GiB, same fs, `sync_copies` (fsync) | 0.97 s | 1060 MiB/s |
| copy 1 GiB, disk -> tmpfs | 0.34 s | 3013 MiB/s |
| move 1 GiB, disk -> tmpfs (copy + fsync + verify + unlink) | 0.44 s | 2332 MiB/s |
| copy 50k x 1 KiB files (100 folders), same fs | 2.41 s | 20.8k items/s |
| baseline `cp -a` of the same tree | 1.07 s | 46.7k items/s |
| trash that 50k-file folder (one rename) | < 1 ms | n/a |
| permanent delete 50k files (scan + unlink) | 0.67 s | 74.9k items/s |
| cross-fs move path, 50k files (fsync per file, EXDEV seam) | 53 s | 0.9k items/s |

## Reading the numbers

- Large files match `cp`: the engine uses FICLONE, then `copy_file_range`, in 1 MiB chunks, with a
  cancel/pause check per chunk.
- Many small files are about 2.2x slower than `cp -a`. The cost is the safety design: per file a
  temp name (`O_EXCL`), metadata, `RENAME_NOREPLACE` commit, `fstat` before/after, plus an
  scan pass first so totals and free space are known (S14). Acceptable for Phase 7; a known
  optimisation is skipping the temp name for files under one chunk, which would weaken S6 and is
  deliberately NOT done.
- The cross-filesystem move figure is dominated by one `fsync` per destination file (S7
  verification); it is a worst case for small files on a real disk. Large files are fast (row 5).
- Trashing a folder is one rename regardless of size (T1), so it is instant.
- Progress is rate-limited to `progress_interval` (100 ms default) so reporting costs nothing
  measurable.

Reproduce: `ninja navigator_ops_tests && ./tests/ops/navigator_ops_tests "[.perf]"` (needs about
2 GiB free; run in a Release build).
