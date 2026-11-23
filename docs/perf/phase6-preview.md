# Phase 6: preview decode timings (src/preview)

Numbers for `docs/architecture.md` "Phase 6" (memory and performance budgets): how long one
worker needs for a list thumbnail and for the Inspector image of typical files. Owner: preview.

## Method

- Test: `Decode timings` in `tests/preview/perf_test.cpp`, hidden tag `[.perf]` (not part of the
  normal `ctest` set). It generates every fixture in a fresh
  `temp_directory_path()/navigator-preview-test-<random>` directory, prints one line per case and
  removes the directory.
- Fixtures (generated with the same libraries the decoders use):
  - 24 MP JPEG: 6000 x 4000, gradient plus per-pixel noise (so the entropy-coded data is not
    trivially small), libjpeg-turbo quality 90, 4:2:0, baseline: 9.2 MB.
  - 12 MP PNG: 4000 x 3000 RGB (8-bit, no alpha), same content, zlib level 6.
  - 12 MP WebP: 4000 x 3000, same content, lossy quality 90.
  - Video: 1280 x 720 MPEG-4 part 2 in Matroska, 100 frames at 10 fps, GOP 5 (generated with
    libavcodec / libavformat in the test).
- Each case is the best of 5 runs of `detail::decode_image_file` / `detail::decode_video_file`
  (open, sniff, decode, scale, orient), file data in the page cache. Single thread.
- Release build (`-O3`, GCC 13.3, no LTO), Intel Core i7-11370H (4C/8T, 3.3 GHz base),
  Ubuntu 24.04, libjpeg-turbo 2.1.5, libpng 1.6.43, libwebp 1.3.2, FFmpeg 6.1.

Run it yourself:

```
cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/rel --target navigator_preview_tests
build/rel/tests/preview/navigator_preview_tests "[perf]"
```

## Results

| Case | Output | Time |
|------|--------|------|
| JPEG 24 MP, thumbnail 128 box (DCT 1/8, then box filter) | 128 x 85 | 81 ms |
| JPEG 24 MP, thumbnail 256 box (DCT 1/8) | 256 x 171 | 81 ms |
| JPEG 24 MP, Inspector fit 1920 x 1080 (DCT 1/2, then box filter) | 1620 x 1080 | 138 ms |
| JPEG 24 MP, Inspector 1:1 (full resolution, DCT 1/1) | 6000 x 4000 | 168 ms |
| PNG 12 MP RGB, thumbnail 256 box (streamed rows) | 256 x 192 | 174 ms |
| PNG 12 MP RGB, Inspector fit 1920 x 1080 | 1440 x 1080 | 189 ms |
| WebP 12 MP lossy, thumbnail 256 box (libwebp scaled decode) | 256 x 192 | 255 ms |
| WebP 12 MP lossy, Inspector fit 1920 x 1080 | 1440 x 1080 | 269 ms |
| Video 720p poster, 256 box (open, probe, seek 10%, one frame, swscale) | 256 x 144 | 2.5 ms |
| Video 720p poster, 1920 x 1080 box (no upscale) | 1280 x 720 | 2.1 ms |
| Freedesktop cache: write one 256 x 171 thumbnail (PNG encode, temp file, rename) | | 14 ms |
| Freedesktop cache: lookup + validate + decode one 256 x 171 thumbnail | | 1.2 ms |

## Reading the numbers

- A 24 MP camera JPEG costs about 80 ms per list thumbnail the first time; afterwards the
  Freedesktop thumbnail is read back in about 1 ms (and thumbnails made by Nautilus / Nemo /
  Thunar are reused at the same cost). With the default 2-6 workers a folder of 24 MP photos
  fills at roughly 25-75 new thumbnails per second; already-cached folders are I/O bound.
- The JPEG thumbnail time is dominated by entropy decoding (Huffman), which DCT scaling cannot
  skip; the 1/8 IDCT and the box filter on 750 x 500 are a few milliseconds.
- PNG and WebP cannot skip work by scale: PNG is zlib-bound (every row is inflated and
  streamed into the scaler, never held whole), WebP decodes every macroblock (libwebp scales
  while emitting rows, so memory is the output only). Both stay well inside a worker's budget
  and never touch the UI thread.
- The video poster is cheap for this short intra-heavy clip; real H.264 / HEVC files depend on
  the GOP length (decoding from the key frame before the 10% point to the first output frame)
  and on probing; the hard bounds are the 10 s interrupt deadline, 3000 packets and the stop
  token.
- The full-resolution JPEG path is faster than "fit 1920 x 1080" because the 1:1 output needs
  no resampling (straight copy of decoded rows), while the fit path box-filters 6 MP.

## Bounds checked by tests (tests/preview)

- 100 MP JPEG (12000 x 8400): thumbnail decoded at DCT 1/8, full-resolution request capped at
  `kMaxOutputPixels` (32 MP); `[slow]` case, about 5 s in a Debug build (mostly encoding the
  fixture).
- Crafted headers: PNG 100000 x 10 and 60000 x 60000, JPEG 60000 x 60000 and GIF
  65535 x 65535 are refused before allocating; interlaced PNG and GIF canvases over the
  full-decode limit (64 MP) return `OutOfMemory`; a 30000 x 30000 BMP with no pixel data and a
  100 MP PNG header with no data decode bounded (blank rows) or fail.
- Mutation fuzzing: 600 mutations per format in the normal run (9 seeds: baseline / progressive
  JPEG, palette / interlaced 16-bit PNG, lossless / lossy WebP, interlaced animated GIF, 32-bit
  and 8-bit BMP, plus EXIF blocks, cache PNGs and UTF-16 text); 3000 per format under
  ASan + UBSan (`NAVIGATOR_FUZZ_ITERATIONS=3000`, 59 s): no crash, leak or UB report.
