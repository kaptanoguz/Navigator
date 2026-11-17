# Phase 5: recursive walk (fs::Walker)

Numbers for `docs/architecture.md` "Phase 5" measurement item 2 (walk entries/s with
`metadata` on and off, time to the first batch, cancel latency). Owner: core-fs.

## Method

- Test: `walk perf: 300k entries, metadata on and off` in `tests/core/walk_test.cpp`, hidden tag
  `[.walkperf]` (not part of the normal `ctest` set because creating the tree takes ~6 s).
  Print-out: `[walk perf] ...` on stdout.
- Fixture: a fresh `temp_directory_path()/navigator-test-<random>` directory on ext4 (NVMe):
  30 top directories x 10 subdirectories x 1,000 empty files = 300,330 entries (330 directories),
  created with `mkdir` / `open(O_CREAT)`, removed afterwards. Page cache warm.
- Each round walks the whole tree with default `WalkOptions` (`batch_size` 2048, latency 50 ms,
  one volume, hidden off) and `metadata = true` (one `fstatat` per entry plus a target stat per
  symlink, exactly `fs::list_directory` metadata), then `metadata = false` (d_type only; one
  `fstatat` per directory for the descent rules). Batches are drained but not stored.
- Cancel latency: `walk: cancellation latency during a 200k-entry walk` (normal `ctest` set):
  200,220 entries, `metadata = true`, one `next_batch()` call that would cover the whole walk
  (`batch_size` 10M, latency 1 h); another thread calls `request_stop()` 30 ms in. Measured from
  `request_stop()` to `next_batch()` returning `Cancelled`; the test asserts < 100 ms.
- Release build (`-O3`, GCC 13, no LTO), single walking thread, `std::chrono::steady_clock`.

Run it yourself:

```sh
./build/release/tests/core/navigator_core_tests "[.walkperf]"
./build/release/tests/core/navigator_core_tests "walk: cancellation latency during a 200k-entry walk"
```

## Machine

Intel Core i7-11370H (4C/8T), NVMe SSD, ext4, Linux 6.8.0, 2026-11-17.

## Results (300,330 entries, three rounds)

| mode | total | first batch | entries/s |
|------|------:|------------:|----------:|
| `metadata = true`  | 477 / 510 / 478 ms | 4.4 / 3.8 / 3.6 ms | 0.59 - 0.63 M |
| `metadata = false` |  97 / 103 /  91 ms | 1.7 / 2.1 / 1.7 ms | 2.9 - 3.3 M |

Cancel latency (200k tree, `metadata = true`): 0.45 - 0.9 ms in release and debug builds,
3.4 ms under ASan. The stop token is polled every 256 readdir results and before every
directory open, so the bound is one getdents64 chunk plus at most 256 `fstatat` calls.

## Notes

- `metadata = false` is about 5x faster: the cost of the metadata walk is the per-entry
  `fstatat`, not readdir. The name index (src/search) should use `metadata = false`.
- The first batch arrives in a few milliseconds because the root is streamed as soon as
  `batch_size` entries are ready; it does not wait for the root directory to be read completely.
- Cold cache (after `echo 3 > /proc/sys/vm/drop_caches`) is not measured here; it is dominated
  by the disk and is the case the progress bar and the cancellation path exist for.
