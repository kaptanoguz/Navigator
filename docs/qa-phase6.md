# QA: Phase 6 findings (Inspector, thumbnails, previews)

Owner: qa. Suites:

- `navigator_browser_integration_tests` (ctest prefix `integration.`, label `integration`):
  `keymap_e2e_test.cpp` now knows six phase tables and drives `view.inspect` on the headless
  pieces the shell uses (`Workspace::set_inspector`, `inspector::Controller`,
  `ui::create_preview_backend` without a thumbnail root).
- `navigator_preview_integration_tests` (new; prefix `integration.`, label `integration`):
  `thumbnail_interop_test.cpp`, `thumbnailer_e2e_test.cpp`, `preview_robustness_test.cpp`,
  helpers in `preview_support.hpp`. Links `navigator_preview`, `navigator_platform`,
  `navigator_core` only. It uses the real preview service, the real process runner, an
  independent PNG chunk reader (signature, IHDR, tEXt, CRC-32) and the system `md5sum`, so spec
  checks do not go through Navigator's own PNG / MD5 code.
- `integration.app_preview_rules` (`tests/integration/preview_app_smoke.sh`): the real binary
  on `SDL_VIDEODRIVER=dummy`, temporary `HOME` / XDG dirs.

Fixtures are made at run time with `ffmpeg` (`lavfi` test pattern: PNG, JPEG, WebP, GIF, BMP,
TIFF, ICO, MP4, MKV), `gdk-pixbuf-thumbnailer` and `ffmpegthumbnailer`. A case SKIPs (the script
exits 77) when its tool is missing. Every cache root is a temporary directory; nothing touches
`~/.cache/thumbnails`.

Gates: `docs/qa-gates.md`, "Phase 6 additions" (41 to 46).

Measured on 2026-11-23 on the development machine: Linux 6.8, GCC 13.3, clang 18, bubblewrap
usable (user namespaces allowed), 8 threads. Scratch build dirs only, never `./build`. The runs
used the tree at `ad3b6df` (ui) on top of `2f70aa0` (platform: read-only sandbox root).

## Tests

| Test (prefix `integration.`) | What it checks |
|---|---|
| `docs/keymap.md and load_default_keymap agree row for row` | 6 tables, 3 + 25 + 21 + 5 + 4 + 2 = 60 chord rows in `Keymap::bindings()` order; the exact set of 21 `(unbound)` rows (Phase 6 adds the 7 `inspector.*` header-button commands). |
| `Every default chord resolves ...` | `Space` and `Alt+KeypadEnter` resolve to `view.inspect`, the Workspace does not handle it (nothing changes); the shell's toggle flips `InspectorState::visible` (persisted), the controller follows the cursor and the app's headless backend delivers a Text preview for `c.txt`, then a Folder preview for `alpha`; again: hidden, nothing outstanding, listing untouched. Near misses add `Shift+Space`, `Alt+Space`, `Ctrl+Shift+Space`, `Alt+Enter`, `Shift+KeypadEnter`, `Ctrl+Alt+KeypadEnter`. |
| `Space typed into a focused text field never inspects` | `field_consumes_as_text` for `Space` / `Shift+Space` only; type-to-filter never starts with a space; `F`, `Space`, `1`, `Shift+Space` into the filter field give `f 1 ` (12 rows: two terms), the address bar and GoTo take `a b`; 0 inspects, InspectorState and persist serial unchanged, 0 submissions; control: `Space` with the list focused resolves to `view.inspect`. |
| `Thumbnails written by Navigator are spec-valid and read back` | `fotoğraf 1.png`: URI `.../foto%C4%9Fraf%201.png`, name = `md5sum` of it, `large/` for a 192 px request, 256 x 192, tEXt (URI, MTime, Size, Mimetype, Image::Width/Height 640 x 480, `Software` Navigator ...) all before IDAT, CRCs valid, 0600 file, 0700 dirs, exactly two files, ffmpeg decodes it; a 100 x 50 source stays 100 x 50; a fresh service reads it back (disk hit, 0 decodes, 0 writes). |
| `A GNOME-style thumbnail from gdk-pixbuf-thumbnailer ...` | `gdk-pixbuf-thumbnailer -s 128` output plus `Thumb::URI`, `Thumb::MTime`, `Software=GNOME::ThumbnailFactory` in `normal/<md5>.png`: Valid, reused (origin ThumbnailCache, downscaled 96 x 72, the file left untouched); wrong URI: Invalid; no MTime: not Valid; after a 10 s mtime shift: Stale, regenerated and overwritten with Navigator's keys, Valid again. |
| `Failure markers: ...` | A corrupt PNG: one marker in `fail/navigator-1/<md5>.png` (1 x 1, URI + MTime, 0600, dir 0700), nothing else; next run: KnownFailure without a decode, the Inspector still decodes (and fails); after a touch: retried and a new marker; a `fail/gnome-thumbnail-factory` marker for a good file is ignored. |
| `A read-only cache root is used but never written` | The decoded thumbnail is still delivered; nothing written. |
| `Installed gdk-pixbuf thumbnailer through the real runner: TIFF and ICO` | The installed entry chosen for `image/tiff`; direct run: 128 x 96, `sandboxed` == bubblewrap usable, source in `read_only_paths`, `RLIMIT_FSIZE` = 32 MiB; through the service: TIFF and ICO thumbnails (origin ExternalThumbnailer, provider `thumbnailer.*`) written to the temporary root with Navigator's keys, reused next run without a process; no child left. |
| `Installed ffmpegthumbnailer on a clip made with the ffmpeg CLI` | 2 s H.264 MP4 at 320 x 240: a landscape image within 256 px, sandboxed. |
| `A hung thumbnailer is killed at the timeout with its process group` | 800 ms timeout: IoError after 0.7..5 s, `timed_out`, the script AND its background `sleep` are gone; through the service a failure marker is written; with a 60 s timeout, `cancel()` kills the running child within 3 s, `shutdown()` returns, no marker for the cancelled file. |
| `Thumbnailer output: size cap, bad output, non-zero exit, no output` | Control (`cp` of a PNG): 128 x 64. Over the 256 KiB cap: `SIGXFSZ` -> IoError; the kill hidden by `exit 0`: rejected (file stopped at the cap is no image); PNG + padding: IoError; garbage: InvalidArgument; empty, missing, exit 3 with a valid image, `SIGSEGV`: IoError; every temporary output dir removed. |
| `At most max_concurrent thumbnailer processes run at once` | 8 jobs, 6 workers, `max_concurrent` 2: all 8 delivered, never more than 2 scripts registered at once. |
| `A sandboxed thumbnailer sees only its input and output` | `/bin/sh -c` inside bubblewrap: the input is readable, a sibling of the input and the fixture's other files are invisible, the output dir is writable; `sandboxed` true. |
| `Corrupt, truncated and huge-header files through service, cache and writer` | ~240 files: per format (PNG, JPEG, WebP, GIF, BMP, MKV) 5 truncations, a 33-byte header, 30 bit-flip variants (seeded) and crafted dimensions (PNG 60000², 100000 x 10, 0 x 0, 2³¹-1; JPEG SOF 65535², 65000 x 1, 0 x 0; GIF screen and frame 65535²; BMP 10⁶², 30000², negative and 2³²-1 sizes; WebP VP8 16383², RIFF size 4 GiB), plus invalid UTF-8, odd UTF-16, NULs, empty, 200k newlines; each as Thumbnail 128, Inspector 1024 x 768 and 1:1 (~730 jobs, 4 workers): all complete, keys match, results within bounds, no Cancelled; cache budget kept; every cache file a complete 0600 PNG keyed to a corpus file, markers only in `fail/navigator-1`; a second run writes nothing. |
| `Cancellation storm while scrolling ...` | 240 JPEGs, 400 frames of a 20-row window + 6 prefetch rows, `cancel_older` on both classes every frame, single cancels, a second thread submitting / cancelling / invalidating / reading stats for the first 380 frames: the final window is delivered as images, Cancelled never delivered, no foreign keys, pending = running = 0, fewer decodes than submissions, no partial cache files, no ticket after shutdown. |
| `Memory budget with many large full-resolution images` | A budget of 1 is raised to 16 MiB; 24 x 1600 x 1200 at 1:1 (7.3 MiB each): peak <= budget + one unpolled completion per worker, at rest <= budget with 1..2 entries; results held by the caller stay intact; 128 MiB keeps >= 10 entries and gives memory hits, back to 16 MiB evicts at once. |
| `app_preview_rules` | Real binary: Inspector `ready` for PNG (image), MP4 (video), text, folder; TIFF without thumbnailers: placeholder; `key:Space` shows / hides, `Alt+KeypadEnter` shows, `Space` and `Shift+Space` with the filter focused do not toggle, `Space` after leaving the field shows again. Three headless runs without `--thumbnail-dir` / `--cache-dir` (with a fake `.thumbnailer` for `image/tiff;image/png;video/mp4;text/plain` in `XDG_DATA_DIRS` and a fake `bwrap` first on `PATH`): nothing under `HOME`, `XDG_CACHE_HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, no marker. Control: `--thumbnail-dir --no-external-thumbnailers` spawns nothing; `--thumbnail-dir` runs the fake (marker) and its failure marker lands in that dir only. |

## Results

| Run | Result |
|---|---|
| GCC 13 Debug `-Werror`, `ctest -R '^integration\.' -j8` | 83/83 pass (no Phase 6 `[!mayfail]` case) |
| New and changed cases (17 Catch2 cases + `app_preview_rules` + the keymap cases), `--repeat until-fail:10` | 10 clean passes (315 s) |
| All `integration.*`, `-j8 --repeat until-fail:5` | 5 clean passes of 83 (165 s) |
| ASan + UBSan (GCC Debug), new and changed cases incl. `app_preview_rules`, `-j3 --repeat until-fail:2` | all pass, 0 sanitizer reports |
| TSan (GCC 13 RelWithDebInfo + `-Wno-error=unused-result` for BUG-P6-1, `setarch -R`, `halt_on_error=1`), the same set, `-j3 --repeat until-fail:2` | 17/17 Catch2 cases pass, 0 `WARNING: ThreadSanitizer`; the app scripts (`app_preview_rules`, and the matched `app_places_chords`) stop on a lock-order inversion inside uninstrumented `libdbus-1` during SDL init (`main.cpp:108` -> `app.cpp:2681`), the known environment reason the app scripts are excluded under TSan (Phase 5: `-E app_`); with a `deadlock:libdbus-1.so` suppression `app_preview_rules` passes with no report |
| clang-format-18 dry run on `tests/integration/*`; clang-tidy-18 (clang build dir) on the four touched test files | clean |

## Bugs

### BUG-P6-1 (high for release builds, platform): optimized GCC builds fail in process_runner.cpp

`src/platform/process_runner.cpp:328` (since `871006f`):

```cpp
(void)::write(wake_fd, &one, sizeof one);
```

With `_FORTIFY_SOURCE` (Ubuntu's GCC enables it at `-O1` and above) glibc declares `write`
`warn_unused_result`, and a `(void)` cast does not silence it in GCC, so `-Werror` stops the
build. Debug builds (no fortify) pass, which is why Phase 6 gates did not see it.

Repro (any scratch dir):

```sh
cmake -S . -B /tmp/nav-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=<deps prefix>
cmake --build /tmp/nav-rel --target navigator_platform
# error: ignoring return value of 'ssize_t write(int, const void*, size_t)' declared with
#        attribute 'warn_unused_result' [-Werror=unused-result]
```

Also breaks the TSan gate (RelWithDebInfo). Fix idea for platform: keep the result
(`const ssize_t n = ::write(...); (void)n;` or `if (::write(...) < 0) {}`), as other eventfd wakes
in the tree do. The qa TSan dir was built with `-Wno-error=unused-result` meanwhile. No test can
express this; a release-configuration CI job would.

## Notes

- N-P6-1 (contract reading, not a bug): `ProcessRequest::max_file_bytes` makes the kernel stop a
  writer at the cap. A thumbnailer that ignores the `SIGXFSZ` failure and exits 0 leaves a file of
  exactly `max_output_bytes`, which `run_thumbnailer` accepts as "1..max_output_bytes" and then
  rejects as undecodable (InvalidArgument), not as "oversized output" (IoError). The header's
  error list mentions oversized output under IoError; both codes write a failure marker, so the
  behaviour is fine. The test accepts either code for that case.
- N-P6-2: completions waiting for `poll()` hold their results, so the memory cache may exceed its
  budget by up to the unpolled completions (documented: held entries count against the budget).
  With the ui polling every frame that is at most one entry per worker.
- The headless app does not probe `bwrap` or load thumbnailers without a thumbnail root
  (`create_preview_backend`): checked with the fake `bwrap` marker.

## Gaps

- Thumbnailers for `video/*` are never chosen through the service when the build has FFmpeg (the
  native provider wins); ffmpegthumbnailer is exercised through `run_thumbnailer` directly.
- The sandbox case covers visibility of files; network isolation and `--die-with-parent` are
  left to `tests/platform`.
- Wayland / X11 runs of the Inspector remain the manual check of `docs/qa-gates.md`.
