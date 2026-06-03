# Scholion — TODO / Backlog

Items are ordered by priority. Completed items kept for history.

---

## Pending work

### Performance optimization — Make program more lightweight
**Goal:** optimize Scholion to run smoothly on more limited hardware with reduced CPU/GPU/VRAM usage.

**Areas to explore:**
- Rasterization pipeline: batch MuPDF calls, reduce background thread wake frequency
- LOD strategy: adaptive tier selection based on GPU memory availability
- Texture caching: memory pooling, compression, or streaming from disk
- Rendering: instancing, batch draw calls, reduce shader overhead
- UI overhead: ImGui draw-call optimization, defer panel rendering
- Frame timing: frame-skipping, adaptive refresh rates
- Input handling: reduce per-frame hit-testing cost for large canvases
- Profiling: identify bottleneck with `SCHOLION_DEBUG` instrumentation

**Status:** research and measurement phase pending.

---

### ✓ Rubber-band select for text boxes — DONE (M23)
**Status:** fully implemented.

**Approach used:** `mouse_button_callback` captures `was_box_selecting` before calling
`on_mouse_button`. On LMB release, if rubber-band just ended (`was_box_sel && !box_selecting()`),
iterates `g_text_boxes` and inserts any box whose world rect falls inside the rubber-band rect.
`box_start_world()` / `box_cur_world()` remain valid after `finalize_box_selection` clears
`m_box_selecting`. `box.w / zoom` converts screen-px width to world units; `h == 0`
(auto-height) falls back to top-left-only check. Additive in both plain and Cmd+drag modes
(selection cleared at press time, not finalize time — consistent with page behavior).

### ✓ Native file picker multi-select — ALREADY DONE
**Status:** was already implemented before this session.

All "Add PDF" call sites use `tinyfd_openFileDialog(..., allowMultipleSelects=1)`.
`load_pdfs_from_selection` already splits the pipe-separated result correctly.
Users can Cmd+click / Shift+click in the native macOS file picker to load multiple PDFs
at once.

---

### ✓ Documentation pass — DONE (M25)
**Goal:** the codebase should be self-explanatory to a future maintainer who has not read
this conversation history. Two levels of work:

#### In-code comments (priority: high)
Prefer short WHY comments over WHAT narration. Target the non-obvious invariants,
coordinate systems, and state machines. Suggested areas:

- **`src/main.cpp`** — largest file (3100+ lines); needs section banners and targeted
  comments on:
  - The drag state machine: three overlapping systems (`m_drag_pending_page` deferred
    threshold, `m_multi_drag_active` page group, `g_box_dragging` text-box group) and how
    they interact; `g_page_drag_states` vs `g_box_drag_states` split.
  - The rasterization pipeline: worker thread → `g_rast_tasks` → `g_rast_ready` →
    `drain_rast_results()` GL upload on main thread; why `glfwPostEmptyEvent()` is called.
  - The undo stack design: per-type records, why TextBoxMove is one record per box while
    PageMove batches all pages in one record.
  - The save/load parser: why it is position/record-based rather than line-based; the
    `doc_map` re-keying for annotation attachment after missing-PDF slot shifts.
  - LOD budget enforcement: when High tiers are evicted unconditionally vs Low tiers
    evicted only above 350 MB.
  - The autosave timer: 60 s, resets on manual save or project open.
  - The startup chooser suppression logic (Apple event grace window in `main()`).

- **`include/input.h` / `src/input.cpp`** — comment the selection state machine:
  which fields are mutually exclusive, when `m_box_selecting` and `m_multi_drag_active`
  can be simultaneously true, and why `start_multi_drag` clears `m_box_selecting`.

- **`src/renderer.cpp`** — comment the coordinate system: world space vs screen space,
  how `world_to_screen` / `screen_to_world` relate to the canvas zoom/pan, and the
  `draw_rect_dashed` screen-space offset invariant (why the offset is in screen px,
  not world units).

- **`src/pdf_loader.cpp`** — comment the three LOD tiers (DPI values, expected memory per
  page) and the `needs_lod` guard that prevents over-uploading.

- **`src/texture_cache.cpp`** — comment the eviction policy: High tier unconditional,
  Low tier budget-gated, Thumb tier never evicted.

#### Architecture document (priority: medium)
Create `docs/ARCHITECTURE.md` covering:
- The three-layer model (Canvas Engine / Document Model / Renderers) with concrete file
  mappings.
- Coordinate systems: screen px (GLFW), world units (canvas), and page-normalized [0,1]²
  (annotation coords) — with transformation formulas.
- The threading model: what is main-thread-only (GL, ImGui, GLFW callbacks) vs what runs
  on the worker (MuPDF rasterize).
- The project file format (.scholion JSON schema): top-level keys, per-doc fields,
  per-page fields, annotation arrays, text_boxes array.
- The undo stack: record types, what each covers, the "session" concept for TextBoxEdit.
- The selection model: `m_selection` (pages), `m_selected_text_boxes` (IDs),
  `g_selected_box` (text-tool focus) and how they interact.

---

### Windows port
**Goal:** build and run on Windows 10/11 with identical feature set.

**Already portable:** all canvas, rendering, PDF, annotation, input, save/load logic.
No platform ifdefs needed in those files.

**Work required:**

| Area | macOS today | Windows equivalent |
|---|---|---|
| `src/scholion_osx.mm` | ObjC++ Apple Events for file-open / `.scholion` association | New `src/platform_win.cpp` — `WM_DROPFILES` / `IDropTarget` for drag-drop; registry `.scholion` association via installer |
| CMakeLists.txt target | `MACOSX_BUNDLE`, Info.plist, AppIcon.icns, codesign | `WIN32` subsystem, `.rc` resource with `.ico`, no bundle |
| App icon | `AppIcon.icns` | `AppIcon.ico` + `.rc` file |
| Recent projects path | `~/.scholion_recents` | `%APPDATA%\Scholion\recents` — wrap in a `platform_paths.h` |
| Modifier key guards | already `GLFW_MOD_SUPER \|\| GLFW_MOD_CONTROL` throughout | no change needed |
| OpenGL linking | `-framework OpenGL` | `opengl32.lib` (CMake handles via `find_package(OpenGL)`) |
| Distribution signing | `codesign` + `notarytool` | `signtool` (optional for personal use) |

**Tasks:**
- Create `src/platform_win.cpp` with `Win32` file-open handling.
- Add `include/platform_paths.h` abstracting `recents_path()` and `app_data_dir()`.
- Update `CMakeLists.txt` with `if(WIN32)` / `if(APPLE)` target blocks.
- Create `resources/AppIcon.ico` from existing PNG sources.
- Test full build on Windows with Visual Studio 2022 or MSYS2/MinGW.
- Cross-compatibility debugging pass (path separators, font rendering, VRAM query).

---

## Completed

### ✓ Unified drag for mixed selection (pages + text boxes) — M22+
**Status:** fully implemented. Cmd/Ctrl+click builds a unified selection of pages and text
boxes; dragging any selected item moves the entire group rigidly.

**Design:**
- Text-box-initiated drag: `g_box_dragging` block owns movement; `g_page_drag_states`
  records page initial positions at click time (avoids `m_multi_drag_active` timing race
  with glfwPollEvents/ImGui frame boundary).
- Page-initiated drag: `on_cursor_move` moves pages via `m_multi_drag_active`; the
  `!g_was_multi_drag && cur_multi` detection in the main loop sets `g_box_dragging = true`
  and populates `g_box_drag_states` so text boxes follow.
- Clicking an already-selected text box without modifier keeps the selection and starts
  group drag (mirrors page behavior).
- Plain click on empty canvas calls `clear_selection()` — clears both pages and text boxes.

### ✓ Modifier-click multi-select (canvas) — M22
- ✓ Cmd/Ctrl+click pages — toggles individual page in/out of selection
- ✓ Cmd/Ctrl+click text boxes — toggles in/out (M20)
- ✓ Shift+click = whole-document toggle (unchanged)
- ✓ Cmd+click on empty canvas starts rubber-band without clearing selection

### ✓ Polish multi-box dragging — M21
Rigid group movement with grab-point-relative delta; undo per moved box.

### ✓ Unified text-box selection + save feedback — M20
Cmd+click text boxes into unified selection; "Saved!" / "saved" toast with fade.

### ✓ Selection/visual polish — M19
Double-click text-box edit fix; dotted offset selection borders; fainter thread wires;
live visible/total page counter in overlay.

### ✓ Code audit & hardening — M18
Viewport restore on load; single-load guard; dead-code removal; relink; hardened parser.

### ✓ Save/load fixes — M17
Text-box wipe on load fixed; missing-PDF doc-index shift fixed; startup chooser;
missing-PDF placeholders.

### ✓ Text & search — M14–M16
Full-text search (Cmd+F); status overlay (F3); Cmd+S save; text-box overhaul.

### ✓ Core milestones — M1–M13
Window + canvas, PDF rasterization, stacks, interaction, annotations, multi-select,
save/load, autosave, VRAM optimization, UX polish.

---

## Notes
- Keymap reference lives in `../CLAUDE.md` — update it when new bindings land.
- `Cmd+A` uses only `GLFW_MOD_SUPER`; generalize to `GLFW_MOD_CONTROL` on non-macOS
  when the Windows port lands.
- No git in this tree — snapshots are tarballs in `~/Dropbox/Scholion/`. See CLAUDE.md
  Restore Points section for the full list.
