# Phase 2: scrolling a 50k-entry folder

Acceptance item from `docs/yol_haritasi.md` (Faz 2): "a 50k-entry folder scrolls and the UI does
not freeze (developer note, plus a simple measurement if possible)". Target set for the `ui`
work: 60 fps, i.e. a frame under 16.7 ms.

## Method

- `navigator --bench-scroll 50000` (development flag, hidden from `--help`). `src/app` creates a
  fixture with `mkdtemp` under `$TMPDIR` (or `/tmp`): 50,000 entries, every 20th a folder, the
  rest empty files with mixed stems, extensions and lengths (Turkish names and one name long
  enough to be elided on every row it appears in). The fixture is removed when the app exits.
- The app opens the fixture normally (listing worker, sort, watch), waits until the listing is
  committed, then drives the Details view itself, with vsync off:
  1. **page-per-frame**: jump one viewport per frame from the top to the bottom (2,084 frames).
     Every frame shows only rows that were never shaped before, so this is the worst case for
     text shaping, ellipsis fitting, date/size formatting and glyph rasterization.
  2. **smooth-6dp**: 600 frames scrolling 6 dp per frame from the top (normal wheel/drag
     scrolling: mostly cached text, one new row every few frames).
- Per frame it records the wall time from the start of the frame build to after
  `SDL_RenderPresent` (frame start work, view update + draw, present, frame-end intents and
  command drain), and separately the **ui-build** time (view update + draw, before present). It
  prints the average, median, 95th percentile and maximum to stdout and quits.
- Window 1280x800 logical, display scale 1 and 2 (`--scale 2`, i.e. 2560x1600 pixels), English UI,
  Release build (`-O3`, LTO off), run under `xvfb-run` (never on the desktop session).

```sh
xvfb-run -a -s "-screen 0 2700x1800x24" ./build/release/src/app/navigator --bench-scroll 50000
SDL_RENDER_DRIVER=opengl   xvfb-run ... --bench-scroll 50000 --scale 2
SDL_RENDER_DRIVER=software xvfb-run ... --bench-scroll 50000
```

## Machine

11th Gen Intel Core i7-11370H (4 cores / 8 threads), Linux Mint 22.3, GCC 13. Xvfb has no GPU:
the Vulkan path runs on lavapipe and the OpenGL path on llvmpipe, i.e. **every pixel is
rasterized on the CPU**. On a real GPU the present cost below mostly disappears; the ui-build
column is the part that does not depend on the GPU.

## Results (2026-11-16, Release)

| Renderer (xvfb) | Scale | Phase | avg | p50 | p95 | max | ui-build avg / p95 |
|---|---|---|---|---|---|---|---|
| OpenGL (llvmpipe) | 1x | page-per-frame | 6.47 ms | 6.22 ms | 8.35 ms | 26.5 ms | 0.81 / 1.17 ms |
| OpenGL (llvmpipe) | 1x | smooth-6dp | 5.82 ms | 5.40 ms | 7.86 ms | 23.3 ms | |
| Vulkan (lavapipe) | 1x | page-per-frame | 7.54 ms | 7.15 ms | 10.74 ms | 24.9 ms | 0.96 / 1.56 ms |
| Vulkan (lavapipe) | 1x | smooth-6dp | 6.38 ms | 6.27 ms | 7.97 ms | 12.4 ms | |
| OpenGL (llvmpipe) | 2x | page-per-frame | 15.47 ms | 14.88 ms | 19.76 ms | 51.5 ms | 1.19 / 1.62 ms |
| OpenGL (llvmpipe) | 2x | smooth-6dp | 14.97 ms | 14.54 ms | 18.94 ms | 48.5 ms | |
| Software (SDL) | 1x | page-per-frame | 16.63 ms | 16.23 ms | 19.86 ms | 59.2 ms | 1.69 / 2.37 ms |
| Software (SDL) | 1x | smooth-6dp | 15.88 ms | 15.37 ms | 19.11 ms | 46.5 ms | |

Debug build, Vulkan/lavapipe, 1x: page-per-frame 11.2 ms avg (p95 14.2 ms), smooth 10.5 ms avg.

## Reading

- **The UI itself costs about 1 ms per frame** at 50k entries (0.8 ms at 1x, 1.2 ms at 2x), even
  when every visible row is new: the list is virtualized (only rows intersecting the viewport are
  formatted, shaped and drawn), text goes through the shape cache (2048 layouts) and the ellipsis
  cache, and nothing walks the 50k entries per frame. This is the GPU-independent part and stays
  far below the 16.7 ms budget.
- The rest is rasterization on the CPU (llvmpipe/lavapipe/SDL software). At 1x every renderer
  holds 60 fps on average; with a CPU rasterizer at 2x (4 megapixels) frames sit right at the
  budget. That is a property of the headless test machine, not of the list; on the developer's
  desktop with a GPU the Phase 0 capture showed 120 fps at 8.3 ms with vsync.
- The software renderer was ~24 ms/frame before the Phase 2 renderer change that fills the
  straight parts of pixel-aligned rounded rectangles (rows, selection, panel) with plain rect
  fills and only the corners with geometry (`Renderer::fill_rounded_rect`); after it, ~16 ms.
- Maxima are single frames (first frame of a phase, X server hiccups under xvfb); they do not
  repeat.
- The UI thread never lists, stats or sorts: those run on the listing/watch workers, so a
  listing of the 50k folder does not stall frames (the previous list stays visible until the new
  one is committed at frame start).
