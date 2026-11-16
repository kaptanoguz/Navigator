# QA — Phase 1 race and flake findings

Owner: qa. Suite: `tests/integration` (`navigator_integration_tests`, ctest prefix
`integration.`). It runs `navigator::core` and `navigator::platform` against the real kernel
(inotify, `/proc/self/mountinfo`) in mkdtemp fixture directories. Gates: `docs/qa-gates.md`
(Phase 1 additions).

Measured on the development machine on 2026-11-16: Linux 6.8, GCC 13.3 and clang 18, ext4 `/tmp`
on NVMe, 8 cores, default inotify limits (`max_queued_events` 16384).

## Tests

| Test (ctest name, prefix `integration.`) | What it checks |
|---|---|
| `list_directory while another thread mutates the directory` | 2 writer threads create, unlink, rename, mkdir, rmdir and symlink in one directory for 1.5 s. The main thread lists it continuously, varying `show_hidden` and `follow_symlinks`. Never a failure. Names are sane and come from the writer's pool. `hidden` matches the name. `metadata_error` holds only documented codes, and the metadata fields then stay at their defaults. Otherwise `kind`, `mode` and `id` are set. `target_kind` is never Symlink. |
| `list_directory of a directory that is renamed, replaced and removed concurrently` | The listed directory itself is moved away and back, replaced by a regular file, and removed. Each call returns a sane listing or a documented error code with a message. NotFound and InvalidArgument (ENOTDIR) both occur. |
| `list_directory cancelled from another thread: success or Cancelled, never partial` | 50,000 files. A stop that is already requested gives Cancelled. In 24 trials a second thread calls `request_stop()` after a random delay between 0 and 2× the full listing time. Every result is either the complete 50,000 entries or Cancelled. The test also measures cancel latency. |
| `watch + relist stays in sync with the directory under a concurrent writer` | 5 seeds × {eager, lazy} consumer. The watch is added before the first listing. A name model applies Created, Deleted, Renamed, MovedFrom and MovedTo, and re-lists on Overflow. The writer runs random create, delete, rename, mkdir, rmdir, move-out, move-in, write and chmod operations: 3,000 for eager, 80,000 for lazy (polls 150 ms apart, so the queue overflows mid-stream). After the writer stops and the queue drains, the model must equal a fresh `list_directory`. |
| `burst of creates then deletes: every event delivered or an Overflow` | 5,000 and 12,000 files are created and then unlinked with no poll in between. Without an Overflow, exactly N Created and N Deleted must arrive, in per-name order. With one, the model re-lists. Both cases end with the reconciled model empty and equal to the disk, and the watch still delivers afterwards. |
| `watched directory removed while busy: WatchedDirGone, other watches unaffected` | Directories `a` and `b` are both churned by writer threads. `a` is then rm -rf'd or renamed. There is exactly one WatchedDirGone for `a` and no event for `a` after it. `b`'s model stays in sync. `remove(gone)` is a no-op. A new watch and `b` both still deliver. Re-adding the re-created `a` gives a fresh handle. |
| `list_volumes and volume_for from 4 threads at once` | 4 threads × 60 iterations. Threads 0–1 use a default source per call. Threads 2–3 share one `create_proc_mount_source()`. Each iteration does `list_volumes` and `volume_for` on `/`, a file in the fixture, and a missing path under it. No errors, Root is always present, and the results match a single-threaded baseline. |
| `volume_for rejects an empty path from concurrent callers` | InvalidArgument on every thread. |

Runtime: about 11 s for the whole suite (GCC Debug); see the results below.

## Results

| Run | Result |
|---|---|
| GCC 13 Debug `-Werror`, `ctest -R '^(core\|platform\|integration)\.'` | 97/97 pass |
| `ctest -R '^integration\.' --repeat until-fail:10` (serial), run twice | 2 × 10 clean, about 108 s per pass |
| `-j8 --repeat until-fail:5` over core, platform and integration | 97/97, no flake (about 27 s) |
| ASan + UBSan (`NAVIGATOR_SANITIZE=ON`), `--repeat until-fail:3` | 97/97, no report |
| TSan (GCC 13, RelWithDebInfo, `setarch -R`), `--repeat until-fail:3` | 97/97, no `WARNING: ThreadSanitizer` |
| clang 18 Debug `-Werror` | 8/8 integration pass |
| clang-tidy-18 (repo `.clang-tidy`, plus `--header-filter=tests/integration`) | 0 findings |
| clang-format-18 `--dry-run -Werror` on `tests/integration/*` | clean |

Numbers from one GCC Debug run of the suite (10.7 s wall):

- Mutation listing: 4,156 listings with 688k entries in total. 15,452 entries had a
  `metadata_error` (vanished between readdir and stat; all NotFound-class). 0 duplicate names.
- Replaced directory: 51,835 listings succeeded and 181,221 failed. The failures were 146,169
  NotFound and 35,052 InvalidArgument (ENOTDIR). No undocumented code occurred.
- Cancellation (50k entries, full listing 69–143 ms): 10–12 of 24 trials cancelled. Mean latency
  about 1.0–1.5 ms. Max 2–8 ms with GCC, 4.8 ms with ASan, 8.8–11 ms with TSan. The test limit is
  100 ms.
- Burst: 5,000 files gave exactly 15,000 events and no Overflow. 12,000 files gave 16,385 events
  (the 16,384-record queue plus one Overflow), and the model reconciled after the re-list.
- Sync, eager: 1,560–1,707 events and 0 Overflows per seed. Lazy: about 32k events and 2 Overflows
  per seed. The model matched the disk on all 10 runs.
- Removed while busy: about 65k events per run, with exactly one WatchedDirGone for both rm -rf
  and rename.

## Findings

No product bug was found. No test is tagged `[!mayfail]`.

Observations for the owners. They are not contract violations:

1. **IN_DELETE_SELF / IN_IGNORED can be lost with an Overflow (platform, documentation).** The
   kernel queues IN_IGNORED like any other event, so a full queue can drop it. The consumer then
   gets Overflow but never WatchedDirGone, and the handle stays registered in the watcher.
   That is correct under the contract ("Overflow: re-list every watched directory"; the re-list
   fails with NotFound). But the UI must treat a NotFound re-list after Overflow as "gone" and call
   `remove()` itself. The removed-while-busy test accepts this branch, but on this machine it
   always got WatchedDirGone without an Overflow. Suggest one sentence in `watch.hpp` or
   `platform-limitations.md`: "after Overflow a WatchedDirGone may be missing".
2. **Cancel latency** is about 1 ms on average and stays under 11 ms in the worst case, even under
   TSan. The 256-entry check interval is fine; nothing needs tuning.
3. **readdir during rename churn** returned no duplicate names on ext4 in roughly 800 listings.
   POSIX allows them, so the test only counts duplicates and does not assert on them.
4. **Build hazard (integration).** If a test TU is built with C++20 while the libraries use C++23,
   it picks `detail::Expected` instead of `std::expected`, and every call that returns a `Result`
   crashes (SEGFAULT or abort). This happened in the first scratch-wrapper build. Inside the root
   project scope this cannot happen. A guard would help: for example, a link-time check, or
   `navigator_headers` exporting `cxx_std_23` when available. Filed for contracts and packaging in
   `docs/integration-notes.md`.
5. **`(void)` does not silence `warn_unused_result`** for `write()` and `symlink()` under
   `-O2 -D_FORTIFY_SOURCE` (GCC). Debug builds miss it, so a release `-Werror` build catches it
   first. The tests use `[[maybe_unused]] const auto rc = ...`.
