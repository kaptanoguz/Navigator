# Phase 1: 100k-entry listing and sort

Acceptance item from `docs/yol_haritasi.md` (Faz 1): "100k synthetic entries produce a sorted
list in a test; the time is measured and written to docs".

## Method

- Test: `core.100k entries: list and sort` in `tests/core/perf_test.cpp` (tag `[perf]`, runs in
  the normal `ctest` set). Print-out: `[perf] 100k: ...` on stdout.
- Fixture: a fresh `temp_directory_path()/navigator-test-<random>` directory (here on ext4,
  NVMe), filled with 100,000 empty files `file-0.txt` .. `file-99999.txt` via
  `open(O_CREAT|O_EXCL)`; removed afterwards.
- `list`: `fs::list_directory(dir)` with default options (one `open` + readdir sweep + one
  `fstatat(AT_SYMLINK_NOFOLLOW)` per entry). Page cache warm (files were just created).
- `sort name`: `fs::sort_entries` with default `SortOptions` (Name, natural, case-insensitive,
  folders first) on a copy of the listing in readdir order. The test asserts that the result is
  exactly `file-0.txt, file-1.txt, ..., file-99999.txt`.
- `sort name desc`: same, descending; `sort mtime`: `SortKey::Modified` (mtimes mostly tie, so
  the name chain is exercised too). `list+sort`: a second listing plus the default sort, i.e. what
  a worker does before handing a directory to the UI.
- Wall-clock `std::chrono::steady_clock`, single thread, three consecutive runs of the test
  binary; the table shows the median run.

Run it yourself:

```sh
./build/dev/tests/core/navigator_core_tests "100k entries: list and sort"
```

## Machine

Intel Core i7-11370H (4C/8T, 3.3 GHz), 16 GB RAM, Linux 6.8.0 (Ubuntu 24.04), ext4 on NVMe,
GCC 13.3. Measured 2026-11-16.

## Results (milliseconds, 100,000 entries)

| Build                       | create | list | sort name | sort name desc | sort mtime | list+sort |
|-----------------------------|-------:|-----:|----------:|---------------:|-----------:|----------:|
| Release (`-O3`)             |   1335 |  150 |        68 |             67 |         43 |       212 |
| Debug (`-O0`, dev preset)   |   1376 |  177 |       595 |            594 |        348 |       770 |
| Debug + ASan/UBSan          |  20207 |  654 |      3102 |           2140 |       1334 |      2528 |

`create` is the fixture setup (100k `open`+`close`), not Navigator code.

## Notes

- Listing cost is dominated by the per-entry `fstatat`; names stay in the small-string buffer
  (`file-12345.txt` is 14 bytes), so there is no per-entry heap allocation besides vector growth.
- The comparator never allocates: it decodes UTF-8 and folds one code point at a time (ASCII fast
  path) and compares digit runs in place. `std::stable_sort` allocates one temporary buffer.
- A Release listing+sort of 100k entries takes about 0.2 s, so it must run on a worker thread
  (as `fs.hpp` requires); the cancellable overload checks the stop token every 256 entries.
- Possible later optimizations if profiling in the UI shows a need: precomputed fold keys per
  listing (outside the comparator), `statx` with a reduced mask, parallel sort.
