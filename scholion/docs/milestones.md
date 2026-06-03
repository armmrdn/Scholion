# Scholion — Milestone Details

## Milestone 1: Infinite Canvas (Current)
**Goal:** Prove the rendering pipeline — a GLFW window with an infinite 2D canvas.

**Deliverables:**
- GLFW window at 1280x800 (resizable, Retina-aware)
- Pan: middle mouse drag, or space + left-click drag
- Zoom: scroll wheel, centered on cursor position
- Background grid that adapts spacing to zoom level
- Placeholder rectangles simulating fanned document stacks
- Dark neutral background (PureRef aesthetic)

**Done when:** You can smoothly pan and zoom around the canvas with placeholder
"pages" visible. No PDF loading, no interaction beyond navigation.

---

## Milestone 2: PDF Rasterization
**Goal:** Render actual PDF pages as GPU textures on the canvas.

**Key tasks:**
- Integrate MuPDF (C library, link via CMake)
- Load a single PDF, rasterize each page to an RGBA pixel buffer
- Upload pixel buffers as OpenGL textures
- Display textured quads on the canvas in place of placeholder rects
- Handle DPI: rasterize at 150 DPI for canvas view, 300 DPI for close zoom
- Implement texture cache (all 100 pages fit in VRAM at 150 DPI)

**Risks:** MuPDF build integration on macOS. May need to build from source.

---

## Milestone 3: Multi-Document Stacks
**Goal:** Load a folder of PDFs and display each as a fanned stack on the canvas.

**Key tasks:**
- Folder picker dialog (native macOS dialog via GLFW/Cocoa interop or tinyfiledialogs)
- Scan folder for .pdf files, load each with MuPDF
- Stack data structure: ordered list of pages, world position, fan offset
- Render stacks: bottom page drawn first, each subsequent page offset by ~4px
- Color-coded top stripe per document (auto-assigned hue)
- Position stacks in a grid layout on first load

---

## Milestone 4: Interaction
**Goal:** Click stacks to view documents, drag pages out of stacks.

**Key tasks:**
- Hit testing: determine which page/stack is under cursor
- Click stack → open split-screen PDF viewer (right panel shows full-page scroll view)
- Click + drag page → detach from stack, freely position on canvas
- Selection highlight (subtle glow or border on hovered/selected items)
- Right-click context menu (Dear ImGui popup): "Return to stack", "Delete", etc.
- Cursor changes: grab hand when hovering draggable items

---

## Milestone 5: Annotations
**Goal:** Pen tool for freehand drawing on pages.

**Key tasks:**
- Tool mode system: Select mode vs. Pen mode (toggle with keyboard shortcut or menu)
- Pen input: capture mouse points while drawing, smooth into polyline/bezier
- Stroke data: list of points, color (user-chosen), width
- Bind strokes to the page they're drawn on (world-to-page coordinate transform)
- Strokes drawn outside any page → bind to nearest/focused page
- Render strokes as GL line strips over page textures
- Undo/redo for strokes (simple stack)

---

## Milestone 6: Thread Lines
**Goal:** Visual connection between loose pages and their parent stacks.

**Key tasks:**
- Each page stores a reference to its parent document/stack
- On click/hover of a loose page, draw a thin bezier curve from page to parent stack
- Line style: thin, semi-transparent, colored to match document hue
- Animate: fade in on hover/click, fade out on deselect
- Future: click thread line to snap page back into stack

---

## Milestone 7: Project File
**Goal:** Persist all workspace state to a JSON sidecar file.

**Key tasks:**
- Define JSON schema for project state (see CLAUDE.md for field list)
- Serialize on: autosave timer (60s), structural changes, app quit
- Deserialize on: app startup if .scholion_project.json exists in folder
- Store: stack positions, loose page positions, parent refs, annotations, viewport state
- Startup flow: folder picker → scan PDFs → load or create project file
- Handle PDF additions/removals (new PDFs get a default stack, missing PDFs get flagged)

---

> Milestones 8–14 shipped (see the status list in `../README.md` and `../CLAUDE.md`
> for details); they were not written up as planning entries here.

---

## Milestone 15: Status Overlay & Cmd+S Save  ✅ (done)
**Goal:** A live status overlay and one-key project saving on top of the existing
`.scholion` persistence system.

**Key tasks:**
- `PerformanceOverlay` (`src/overlay.cpp` / `include/overlay.h`): bottom-left,
  F3-toggle, FPS health dot (green ≥ 50 / amber 30–50 / red < 30), page count,
  zoom %. Click-through, semi-transparent, no decoration.
- `Canvas::get_zoom_percentage()` — 100 % = print size (`m_zoom * 100`), display
  pixel density independent.
- Cmd+S — silent overwrite of the current project, or Save-As dialog if none set;
  also save once on clean quit. Reuses `save_to_path` / `save_project`; no format
  change, no new dependency.
- Text-tool fixes: Escape confirms text and exits the tool; font size/color persist
  between uses and apply live to the box being edited.

**Deliberately deferred:** working-folder auto-load on startup, and change-triggered
(dirty-flag) autosave — current cover is the 60 s timer plus save-on-quit.

---

## Milestone 16: Text-Box Overhaul  ✅ (done)
**Goal:** PureRef-style text boxes with predictable selection, styling, and undo.

**Key tasks:**
- Data model: `CanvasTextBox` gains `w`/`h` (px; 0 = auto). `.scholion` records carry
  `w`/`h`, back-compatible (older files omit → auto-size).
- Creation: press-drag-release sizes the box — drag fixes width (text wraps), height
  is a floor that grows to fit; a bare click makes a default-width, auto-height box.
- Default style: red `(0.82, 0.06, 0.06)`, 16 pt — same red as the pen.
- ESC is two-stage: while typing, confirm + close the box (tool stays active); a
  second ESC exits the text tool. Clicking out also confirms + closes.
- Selection: single-click selects and surfaces that box's color/size in the picker;
  picker edits apply to the selected box only and are then inherited by new boxes.
  Double-click edits the text.
- Copy/paste: Cmd/Ctrl+C then V duplicates the selected box at an offset; copy/paste
  of text into/out of an open box is handled natively by the edit field.
- Undo: new `TextBoxEdit` (one per editing session) and `TextBoxStyle` (one per
  selection session) records, alongside existing create/move/delete.

**Deliberately deferred:** multi-box selection (single-box only for now); zoom-scaling
of text (font stays a fixed pixel size, consistent with prior behavior).

---

## Milestone 17: Save/Load Fixes  ✅ (done)
**Goal:** Make the project file actually round-trip everything it claims to.

**Key tasks:**
- Fix text boxes vanishing on load: they were pushed straight into `g_text_boxes`
  during parse, then erased by the post-parse `g_text_boxes.clear()`. Now parsed
  into a temp vector and assigned after the reset (like annotations).
- Fix annotation loss when a referenced PDF is missing: skipped documents shifted
  `g_documents` indices, so annotations attached to the wrong doc or were dropped.
  Load now builds a `doc_map` (saved index → actual index) and re-keys all
  highlights/notes/strokes through it.
- Open `.scholion` from the CLI/Finder as a project (was routed to `load_pdf`).
- Startup chooser on bare launch: Open Project / Add PDF(s) / Add Folder / blank.
- Right-click Save shows `Cmd+S` and updates the existing file (Save-As if unset).
- Load/save print restored/written counts (docs, text boxes, highlights, notes,
  strokes) for quick diagnosis.

**Missing-PDF placeholders (done):** a document whose PDF can't be found at load is
kept as a placeholder occupying its original slot — pages (at saved positions, with
a default Letter size) and annotations are preserved, the loader slot is `nullptr`
(so it's skipped by rasterization), and it renders as a distinct grey page. On
re-save the reference and its annotations are written back out, so a moved/renamed
PDF no longer loses that document's data. Only the path is stored — never PDF bytes.

---

## Milestone 18: Code Audit & Hardening  ✅ (done)
**Goal:** Clear latent bugs, dead code, and fragility surfaced by a full review.

**Key tasks:**
- **Viewport restore (bug):** `"viewport"` is written before `"documents"`, but the
  loader only parsed it inside the docs section → zoom/pan never restored. Now parsed
  independently of section.
- **Double-load (redundancy):** on macOS a file argument is delivered via the
  open-document Apple event, so the `argv` loop is `#ifndef __APPLE__` — the file
  loads once (Finder launches were already single).
- **Dead code:** removed unused `Renderer::m_proj`. `-Wall -Wextra` is now clean
  across all translation units.
- **Diagnostic logging:** load/save counts gated behind `SCHOLION_DEBUG` env var.
- **Page w/h persisted:** page records now store `w`/`h`, so missing-PDF placeholders
  use the real page size (back-compatible: older files fall back to Letter).
- **Clipboard reset:** `new_project()` clears the text-box entity clipboard.
- **Relink:** right-click a missing-PDF page → "Relink PDF…" → file picker loads the
  real file into that slot, carrying over page positions and annotations.
- **Parser hardening:** load is now position/record-based rather than line-based —
  tolerant of compact/pretty/reordered layouts, CRLF, and arbitrary whitespace.
  Verified across all four layouts plus missing-PDF and the canonical save format.
