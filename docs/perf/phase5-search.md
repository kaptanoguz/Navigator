# Phase 5: name matching, filter pipeline and name index

Numbers for `docs/architecture.md` "Phase 5" benchmark plan items 1 and 3 (item 2, the walk, is
also in `phase5-walk.md` from core-fs; item 4, UI frame times, belongs to ui). Owner: search.

## Acceptance

| target | result |
|---|---|
| `filter_entries`, 20k ASCII names, under 2 ms (justifies `kUiThreadFilterLimit`) | 0.58 - 0.95 ms (mixed Unicode names: 1.03 - 1.39 ms) |
| Restart (load + verify) faster than a cold walk | 114 ms vs 521 ms full index scan (0.22x) and vs 243 ms for a bare `fs::Walker` walk (0.47x) |
| Index query, 1M entries, 3-character query, under 100 ms | 37 - 50 ms p50, at most 55 ms p95 (worst case, a 1-character query matching 433k: 77 - 81 ms p50) |
| Memory about 60 MB per million entries | 58.6 - 61.2 MB per million (resident) |

## Method

- Tool: `tools/search_bench` (`search_bench [--entries N] [--dir DIR] [--only filter|walk|index|million]`).
  It creates a fresh `navigator-search-bench-XXXXXX` directory inside DIR (default: the temp
  directory), works only there, and removes only that directory.
- Build: Release, `-O3` with LTO, GCC 13.3, C++23. Single thread, `steady_clock`.
- Machine: Intel Core i7-11370H (4C/8T, `powersave` governor), 15 GB RAM, NVMe SSD, ext4,
  Linux 6.8.0, 2026-11-23. Measured with the 1-minute load average at 2.1 - 2.3. An earlier run
  at load 15 (another agent's clang-tidy) was 2-4x slower everywhere and was discarded.
- **Page cache is warm everywhere.** The tool does not drop caches (that is a system-wide
  setting). Cold numbers are not measured; they are disk-bound. To measure them, run
  `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches` yourself before `--only index`.
- **filter**: synthetic `DirEntry` listings of 1k / 20k / 100k / 200k names, about 18 bytes each,
  built from Navigator test stems (`report`, `Rapor`, `İstanbul`, NFD `e`+U+0301, ...), a number
  and an extension. "ASCII" replaces every non-ASCII byte with `x`; "mixed" keeps them (about a
  third of the names are non-ASCII and take the full `fs::fold_name_into` path). Median of 51
  runs (20k and below) or 11 runs.
- **tree** (walk and index): about 300k entries, 30k directories, depth up to 12, 2% hidden
  directories with children, 5% hidden files, 5 unreadable (mode 000) directories; all directory
  mtimes set two hours back, so the restart verify does not hit the 2 s coarse-timestamp guard.
- **index**: the full scan is timed without an index file. Save = `encode_index` +
  `write_file_atomic` (temp file, fsync, rename, directory fsync) of the same tree. Restart = new
  `IIndex` with the file until `Ready`, 7 rounds alternated with a bare `fs::Walker` walk
  (`metadata = false`, hidden on), medians reported.
- **million**: 10k real directories (100 x 100, mtimes aged), plus an index file listing
  1M entries (99 synthetic file names per directory) that exist only in the file. The verify pass
  finds every directory unchanged, so this measures load, memory and queries without creating
  a million inodes. Resident memory = RSS after start minus RSS before (`malloc_trim` both times).
- Queries: limit 1000 (the ui default), `show_hidden = false`, no type filter, 21 runs each,
  p50 / p95.

## 1. `filter_entries` (folder filter)

Median ms (hits in parentheses, ASCII listing):

| entries | query | Substring ASCII | Fuzzy ASCII | Substring mixed | Fuzzy mixed |
|---:|---|---:|---:|---:|---:|
| 1,000 | `r` (281) | 0.036 | 0.038 | 0.059 | 0.060 |
| 1,000 | `rep` (46 / 66) | 0.029 | 0.032 | 0.047 | 0.052 |
| 1,000 | `rapor12x` (0) | 0.026 | 0.028 | 0.044 | 0.042 |
| 20,000 | `r` (5,799) | 0.87 | 0.95 | 1.36 | 1.39 |
| 20,000 | `rep` (889 / 1,304) | 0.65 | 0.73 | 1.10 | 1.17 |
| 20,000 | `rapor12x` (0 / 11) | 0.58 | 0.62 | 1.03 | 1.07 |
| 100,000 | `r` (28,645) | 4.5 | 4.9 | 7.0 | 7.2 |
| 100,000 | `rep` (4,130 / 6,277) | 3.5 | 3.8 | 5.6 | 6.1 |
| 100,000 | `rapor12x` (0 / 30) | 3.1 | 3.2 | 5.3 | 5.5 |
| 200,000 | `r` (57,692) | 9.3 | 9.7 | 14.2 | 14.6 |
| 200,000 | `rep` (8,407 / 12,574) | 6.9 | 7.9 | 11.6 | 12.4 |
| 200,000 | `rapor12x` (0 / 187) | 6.2 | 6.6 | 11.0 | 11.6 |

Cost is linear, about 30 ns per non-matching ASCII name and 50 ns per mixed name, plus about
50 ns per hit (scoring). Fuzzy costs within 15% of Substring: the DP runs only on names whose
folded bytes contain the query as a subsequence. 20k entries stay under 2 ms in every case, so
`kUiThreadFilterLimit = 20'000` holds with margin. Above it the ui uses a worker, as designed.

## 2. `fs::Walker` on the tree (cross-check of phase5-walk.md)

| metadata | total | entries/s | first batch |
|---|---:|---:|---:|
| false | 239 ms | 1.25 M | 1.7 ms |
| true | 536 ms | 0.56 M | 3.2 ms |

Cancel latency (stop requested from another thread at a random point, 40 walks): p50 0.024 ms,
max 0.068 ms. This tree has 100x more directories than the core-fs tree (30k vs 330), hence the
lower `metadata = false` rate: one `fstatat` per directory dominates.

## 3. Name index, 300k tree

| measurement (median) | value |
|---|---:|
| entries / directories / unreadable | 299,617 / 29,968 / 5 |
| full scan to `Ready` (no file) | 521 ms |
| save (encode + fsync + rename) | 37.5 ms |
| index file | 10.68 MB, 35.6 bytes per entry |
| **restart to `Ready` (load + verify)** | **114 ms** |
| of which load (read, checksum, parse, fold) | 54 ms |
| of which verify (one `statx` per directory, nothing re-listed) | 58 ms |
| bare `fs::Walker` walk, `metadata = false` | 243 ms |
| restart / full scan | 0.22 |
| restart / bare walk | 0.47 |

Why a full scan costs twice a bare walk: the index lists every directory with its own one-level
`Walker` (resumable per directory, re-listable alone by verify and inotify), stats every
directory once more for its mtime, watches it, and applies each directory in its own exclusive
section with names folded once. The restart touches each directory with one `statx` and reads
no directory contents.

Queries after a restart (limit 1000):

| query | Substring p50 / p95 | Fuzzy p50 / p95 | matched (Sub / Fuzzy) |
|---|---:|---:|---:|
| `rep` | 9.2 / 9.9 ms | 10.3 / 11.7 ms | 8,447 / 12,271 |
| `ist` | 10.9 / 11.4 ms | 11.9 / 12.3 ms | 8,544 / 10,115 |
| `a` | 18.3 / 18.9 ms | 18.6 / 19.0 ms | 88,355 |
| `rapor 12` | 8.1 / 8.4 ms | 10.1 / 10.6 ms | 605 / 1,666 |
| `main.cpp` | 5.7 / 5.9 ms | 8.5 / 8.8 ms | 0 / 761 |
| `zzzq` | 5.3 / 5.6 ms | 5.6 / 5.8 ms | 0 |

## 4. Name index, 1M synthetic entries

Load + verify of 10k directories: 183 - 197 ms (4 runs). Resident memory: 58.6 - 61.2 MB,
i.e. 59 - 61 bytes per entry.

p50 / p95 ms over 4 runs (range of p50, worst p95):

| query | Substring p50 | Substring p95 max | Fuzzy p50 | Fuzzy p95 max | matched (Sub / Fuzzy) |
|---|---:|---:|---:|---:|---:|
| `rep` | 36.8 - 38.6 | 40.8 | 41.4 - 42.6 | 46.5 | 41,008 / 61,713 |
| `ist` | 44.5 - 46.5 | 54.5 | 48.1 - 50.0 | 53.6 | 41,524 / 49,708 |
| `a` | 76.6 - 80.8 | 84.2 | 78.0 - 81.6 | 88.7 | 433,443 |
| `rapor 12` | 31.0 - 32.4 | 33.4 | 33.7 - 36.9 | 54.5 | 2,065 / 4,697 |
| `main.cpp` | 26.0 - 28.0 | 31.2 | 31.6 - 34.1 | 36.5 | 0 / 4,171 |
| `zzzq` | 24.6 - 26.2 | 26.9 | 25.6 - 27.4 | 30.7 | 0 |

A query costs about 25 ns per indexed entry to reject (pre-folded names, one `find` per term)
plus about 120 ns per match (refold with offsets, score, rank). Only the best `limit` get
highlights and absolute paths.

## Notes and limits

- Queries hold the index's shared lock for their whole run (a consistent snapshot). The lock
  prefers writers (`src/search/rw_lock.hpp`): with glibc's reader-preferring `shared_mutex`, a
  steady stream of queries starved the service thread forever (found by
  `search.queries run concurrently with live updates`). So a query waits for at most one writer
  section plus the queries already running. A writer section is one directory's worth of work,
  except the removal of a whole large subtree and a Rebuild's reset, which take longer.
- Save frequency: the index ignores changes in its own cache directory when deciding whether it
  is dirty. Otherwise indexing the volume that holds `~/.cache` would save every
  `save_interval` forever, each save reporting itself through inotify.
- Symlinks (BUG-P5-1 fix, 2026-11-23): when a folder is listed (scan, verify, live
  re-listing), each symlink entry gets one follow `statx` (DONT_SYNC, NO_AUTOMOUNT, never
  entered), so a link to a folder is a Folder for the type filter, as in a walk. Cost measured
  on one folder, median of 7 scans: 20k regular files 6.2 ms, 20k links to a folder 29.3 ms,
  i.e. about 1.2 us per link. The generated 300k tree above has no symlinks, so its numbers are
  unchanged. Limit: a link whose target changes between file and folder without its parent
  folder changing is picked up only by a Rebuild.
- Not measured here: cold page cache, network or slow disks, inotify throughput under bulk
  changes, UI frame times while results stream in (ui).
