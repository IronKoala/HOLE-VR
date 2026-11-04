# White-square stage capture

This diagnostic compares the image immediately before the mod copies it into
the OpenXR eyes with the two images immediately after that copy. It does not
identify an Unreal shader and does not capture runtime reprojection or later
OpenXR composition layers.

## Settings

The startup log line `build: <git revision>` names the code that ran (the
revision is generated at every build; `-dirty` means uncommitted changes).
These hole-vr.ini lines are added on while the white squares are being
investigated; set paired_capture_enabled=0 for performance comparisons and
once the squares are fixed:

```ini
paired_capture_enabled=1
paired_capture_limit=3
paired_capture_on_dots=1
```

Press **Ctrl+F11 while the dots are visible** to request a capture. With
on-dots enabled, a qualifying square-watch dot event requests one
automatically. Set paired_capture_on_dots=0 if all requests should be
reserved for the manual key.

A detection requests the next successfully copied frame; it cannot recover
the earlier frame that the background watcher just analyzed. Keep the view
steady for a moment. Requests while another capture is pending/writing are
ignored. The limit counts accepted requests, including requests later dropped
because allocation failed; restart for a new bounded run. The hard maximum
is eight per game process.

The log reports acceptance and later completion. Each capture writes, beside
hole-vr.log, a shared pair-<timestamp>-p<process>-<number>-f<frame>-<tag> prefix:

- source-before.bmp: full side-by-side source, before the eye copies.
- left-after.bmp and right-after.bmp: each copied eye.
- metadata.txt: build ID, frame number, predicted display time, source kind,
  dimensions, formats, source state, acquired image indices, exact submission
  fence, xrEndFrame result, and per-eye pixel differences.

All three readbacks are recorded in the same GPU command list. Each eye is
temporarily changed from COPY_DEST to COPY_SOURCE for its readback, then
returned to COPY_DEST before the existing restoration to RENDER_TARGET.
Only the exact completed fence allows mapping. Pending buffers are retained
until completion, including across resize, and never reused after a timeout.
A failed queue signal retains the buffers and blocks further captures, rather
than claiming completion.

One capture can be in flight/writing. Supported images are ordinary 2D,
single-sample, single-array RGBA8/BGRA8/RGB10 textures with a source no larger
than 16 million pixels; the source must split evenly into identically sized
eyes. Unsupported dimensions/formats are logged and skipped. Readbacks add GPU
traffic and CPU/disk work, so use a short diagnostic run rather than a
performance comparison.

## Interpret a capture

First confirm both comparison_valid values and all three image_saved values
are 1. Check the build ID, frame and source_kind. An xr_end_frame_result other
than XR_SUCCESS (0) means the submission itself also needs investigation.

- Dots visible in source-before, with both different_pixels values 0:
  the copied eye bytes match the already affected source. Investigate the
  render/source stage preceding this copy.
- Source clean, with nonzero exact pixel differences:
  investigate the copy/resource synchronization path. The metadata records
  the first differing pixel in each eye's coordinates.
- Source and copied eyes clean, while the player still sees the problem:
  capture again while it is visible. If reliably simultaneous, investigate
  subsequent composition/reprojection/display stages.
- Nonzero differences alone do not identify a shader or prove a visible
  artifact; inspect their position and values.

The comparison uses the original packed 32-bit pixels, excludes row padding,
and compares each eye against the corresponding source half. The BMPs are
full resolution but use 8-bit RGB previews; RGB10 quantization can conceal a
small numeric difference that the exact comparison still reports.

Existing capture-dots-N.bmp files are assembled from strips of successive
frames. A capture-dots-N-strip.bmp is one strip from a single source frame.
Neither is a paired before/after capture.

## Validation

Portable test:
```sh
clang++ -std=c++17 -Isrc tests/test_frame_capture.cpp -o test_frame_capture
./test_frame_capture
```

It covers left/right separation, different row strides, a last row without
padding, an exact single-pixel discrepancy, swapped eyes, incomplete buffers,
invalid dimensions, and fence completion/device-removal checks.

Before trusting a headset investigation, perform these manual checks:

1. With the feature disabled, Ctrl+F11 and watcher events create no pair files.
2. Enable it; request one clean static scene. Three BMPs share one prefix and
   frame, dimensions match, both comparisons are valid with zero differences.
3. Request while artifacts are visible; inspect all three images together.
4. Request repeatedly during writing and past the limit; no extra captures or
   background writer threads are started.
5. Resize or leave a level after requesting. Existing readbacks complete from
   their original dimensions; the next request uses the new dimensions.
6. Under a slow GPU/fence timeout, no incomplete image is mapped or mislabeled
   as complete. The ordinary legacy capture also now defers until its own
   submission fence completes.

The portable checks and syntax build do not substitute for these in-game
checks. No headset run has been performed for this change.
