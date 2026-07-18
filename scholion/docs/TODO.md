# Scholion — TODO / Backlog

Items are ordered by priority.

---

## Pending

### Icon upscaling for high-DPI and large-format usage
**Goal:** the current `AppIcon.icns` / `AppIcon.ico` / Linux `.png` master artwork needs
a high-resolution source so the icon renders sharply in contexts that demand large sizes:
macOS Launchpad (128×128 / 256×256 / 512×512 @2x), Windows taskbar and Start menu
(256×256), and Linux application menus / dock launchers (128×128 or larger).

**Work required:**
- Produce or commission a vector or high-res raster master at 1024×1024 minimum
- Regenerate `scholion/resources/AppIcon.icns` via `scripts/make_icon.py` with the new master
- `make_icon.py` auto-exports `scholion-win/resources/AppIcon.ico` in the same run
- Add a `512×512.png` and `256×256.png` export to `scholion-lnx/resources/` for `.desktop` file use on Linux
- Update the Linux CI bundle step to include the icon alongside the binary

**Status:** pending.

---

### Remove benchmarking / developer-mode code before open-source release
**Goal:** strip all profiling/logging code that is inappropriate for public distribution.

**What to remove from `scholion/src/main.cpp`:**
- `get_process_ram_mb()` function
- `g_bench_file`, `g_bench_start`, `g_bench_last_write` globals
- `bench_open()` and `bench_write()` functions
- `AppSettings::developer_mode` field and its `save_prefs()` / `load_prefs()` lines
- Developer Mode checkbox block in `draw_settings_popup()`
- Main loop line: `if (g_settings.developer_mode) bench_open();`
- `#include <psapi.h>` in the `_WIN32` block
- `#include <mach/mach.h>` in the `__APPLE__` block

**What to remove from `scholion-win/CMakeLists.txt`:**
- `psapi` entry in `target_link_libraries`

**Note:** this code is currently gated behind the `SCHOLION_DEV` compile flag and is never
enabled in release builds (CI only sets it on the `develop` branch). Safe to ship as-is;
cleanup is required before the repo goes public.

**Status:** pending (safe for binary releases; required before open-sourcing).

---

### Performance optimization — lightweight hardware support
**Goal:** run smoothly on hardware with limited CPU/GPU/VRAM.

**Areas to explore:**
- Rasterization pipeline: batch MuPDF calls, reduce background thread wake frequency
- LOD strategy: adaptive tier selection based on available GPU memory
- Texture caching: memory pooling or streaming from disk
- Rendering: instancing, batched draw calls, reduced shader overhead
- Frame timing: adaptive refresh, frame-skipping on idle — **done in v1.3** (event-driven render loop)
- Input: reduce per-frame hit-testing cost on large canvases

**Status:** frame-timing/idle addressed in v1.3; rasterization/LOD/texture/rendering items still pending.

---

### Windows — `.scholion` file association
**Goal:** double-clicking a `.scholion` file in Explorer opens Scholion directly.

The `argv[1]` path handling already works; only the registry association is missing.
Requires either a simple NSIS/WiX installer step or a first-run registry write.

**Status:** pending.

---

## Completed

### ✓ Highlighter Box/Freehand modes — v1.3
Highlighter has a toolbar mode toggle (`g_hl_box_mode`, default Box). Box = rectangle drag;
Freehand = marker swipe. BOTH capture glyphs via center-in-AABB (reliable) → References. Non-text:
Box → plain highlight rect, Freehand → translucent marker stroke. `AnnotStroke` gained
`width`/`alpha` (serialized `sw`/`sa`, back-compatible) for the marker.

### ✓ Pen ortho-lock (Shift = straight line) — v1.3
Holding Shift while drawing collapses the stroke to a straight segment from its start, snapped to
the nearest 45° in world-aspect space. Shared `stroke_add_point()` helper; applies to pen + freehand
highlighter. No serialization change.

### ✓ Per-box zoom-scaling text — v1.3
`CanvasTextBox.zoom_scaled` + a "Scale" toggle in the text property strip: font and box scale
with canvas zoom while stored `w/h/font_size` stay canonical at 100%. Shared `text_box_layout()`
drives draw, hover, and hit-test so they never diverge. Serialized as `zs` (back-compatible).

### ✓ Page rotation 180° / reset — v1.3
Added "Rotate 180°" and "Reset Rotation" to the page context menu; fixed `apply_page_rotation`
to swap `world_w/h` only on odd 90° turns (the old unconditional swap was wrong for 180°/reset).

### ✓ Event-driven render loop — v1.3
Canvas renders on demand (`glfwWaitEventsTimeout` + worker `glfwPostEmptyEvent`) instead of
continuously; idle CPU/GPU drop to near zero on macOS/Linux. Overlay shows an "idle" state.
(Partial completion of the Performance item below — frame-timing/idle skipping.)

### ✓ Tool-exclusion + light-mode fixes — v1.3
Synchronous text-box hit-test (`text_box_at`) so a click on a box can't also start an annotation
on the page beneath it; double-click-to-edit disarms any active annotation tool; References notes
are readable in Light mode (edit box pins light text on its dark background; note text theme-aware).

### ✓ Linux port — v1.1
`scholion-lnx/` build tree; CI job on `ubuntu-22.04`; `Scholion-Linux.zip` release asset.
Platform-specific: GLAD loader, `xdg-open` for reveal-in-file-manager, X11/GLFW.

### ✓ Annotation coordinate fix on rotated pages — v1.1
`screen_to_page_norm()` now inverts the rotation transform so pen strokes, highlight
glyph collection, and eraser all operate in PDF-native normalized space regardless of
page rotation. Single function change in `canvas_annot.cpp`.

### ✓ Clean shutdown — v1.1
`rast_cancel_all()` called before `rast_shutdown()` so the worker thread exits
immediately on quit rather than draining the full task queue.

### ✓ Windows port — v1.0
`scholion-win/` build tree; CI job on `windows-2022` (MSYS2/MinGW64); `Scholion-Windows.zip`
release asset with DLLs bundled. GLAD 3.3 Core, AppIcon.ico, DPI-awareness manifest,
dark title bar via `DwmSetWindowAttribute`.

### ✓ Text-snapping highlights + References tab — M29
Highlight tool snaps to MuPDF character bounding boxes; captured text feeds the
References sidebar tab with filename + page number; Markdown export via save dialog.

### ✓ Panel interaction redesign — M27
Single-click selects, double-click opens panel, Space tap toggles panel.
Text boxes no longer render over the open panel.

### ✓ Rubber-band select for text boxes — M23
Rubber-band box now includes text boxes in the selection. Additive in Cmd+drag mode.

### ✓ Modifier-click multi-select — M20–M22
Cmd/Ctrl+click toggles pages and text boxes individually into a unified selection.
Shift+click remains whole-document toggle. Group drag moves all selected items rigidly.

### ✓ Save / load hardening — M17–M18
Text-box wipe on load fixed; missing-PDF index shift fixed; startup chooser;
missing-PDF placeholders with relink; position/record-based parser (CRLF-tolerant).

### ✓ Core milestones — M1–M16
Canvas + pan/zoom, PDF rasterization, stacks, annotations, multi-select, save/load,
autosave, VRAM optimization (350 MB budget, 3-tier LOD), text boxes, full-text search,
status overlay, UX polish.
