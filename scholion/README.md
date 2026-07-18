# Scholion

An infinite-canvas workspace for researchers working with multiple PDF documents.
Inspired by PureRef — minimal chrome, canvas-first, physical feel.

---

## Build

**Dependencies (macOS):**
```bash
brew install glfw
# cmake: use the bundled binary if brew cmake requires a newer CLT
```

**Build:**
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DSCHOLION_WITH_MUPDF=ON
make -j$(sysctl -n hw.ncpu)
```

**Run:**
```bash
open build/Scholion.app
```

---

## Loading Documents

| Action | Result |
|--------|--------|
| Drag PDF file onto window | Load PDF document |
| Drag folder onto window | Load all PDFs in folder |
| Click `+` button (top-left) | Open file picker |

---

## Canvas Navigation

| Input | Action |
|-------|--------|
| Middle mouse drag | Pan |
| Space + left drag | Pan (PureRef style) |
| Scroll wheel | Zoom centered on cursor |

---

## Page Interaction

| Input | Action |
|-------|--------|
| Left click page | Open panel viewer; scroll to that page |
| Left drag page | Move page freely on canvas (4 px threshold before drag activates) |
| Hover over page | Hand cursor + white highlight outline |
| Hover 1 second | Tooltip showing filename and file path |

---

## Multi-Page Selection

| Input | Action |
|-------|--------|
| Shift + left click page | Toggle entire document in/out of selection |
| Drag on empty canvas | Rubber-band box selection (full containment required) |
| Drag any selected page | Move all selected pages as a group |
| Cmd + A | Select all pages on canvas |
| Escape | Clear selection (if any selected), then close panel |

Selected pages show a blue outline. A translucent blue rectangle appears during rubber-band drag.

---

## Context Menus

### Right-click on a page
| Item | Action |
|------|--------|
| Return to stack | Snap page back to its stacked diagonal position |
| View in panel | Open sidebar panel scrolled to that page |
| Fan pages vertically | Lay all pages of the document in a vertical column |
| Fan pages horizontally | Lay all pages in a horizontal row |
| Stack pages | Reset all pages to the original diagonal fan layout |
| Rotate CW / CCW / 180° / Reset | Rotate the page in 90°/180° steps, or reset to upright — annotations follow |
| Reveal in Finder | Open containing folder in macOS Finder |

### Right-click on empty canvas
| Item | Action |
|------|--------|
| Save project… | Save layout to a `.scholion` JSON file |

**Cmd + S** saves without the menu: it silently overwrites the current project,
or opens a Save-As dialog if no project file is set yet. The project also saves
automatically every 60 s and once more on quit (once a project file exists).

On a bare launch (opening the app directly, with no document), Scholion shows a
**startup chooser**: Open Project File, Add PDF File(s), Add Folder of PDFs, or
start on a blank canvas. Launching by double-clicking a `.scholion` (or passing
one on the command line) opens that project directly and skips the chooser.

---

## Panel Viewer (Sidebar)

Click any page to open the panel viewer on the right side of the window.

| Input | Action |
|-------|--------|
| Click page on canvas | Open panel; scroll to that page |
| Click page in panel | While panel is open for that doc, scrolls to clicked page |
| Escape | Close panel (when no selection active) |
| Click empty canvas | Deselect document and close panel |
| Drag resize strip | Resize panel width |

### Annotation Tools (in panel header)

| Tool | Shortcut | Behavior |
|------|----------|---------|
| Pen | P | Freehand stroke on the active page; hold **Shift** to lock the line straight (snaps to 45°) |
| Highlight | H | Two modes via toolbar toggle: **Box** drags a rectangle to reliably capture text into References; **Freehand** swipes like a marker. Both capture the text they cover; over non-text, Box leaves a highlight rectangle and Freehand a persistent translucent marker |
| Note / Flag | F | Stamp a sequentially-labeled flag (A, B … Z, 2A, 2B …) |
| Eraser | — | Drag over strokes/highlights to erase them |

- Hold **Shift** while drawing with the pen (or the Freehand highlighter) to lock the line
  straight, snapped to the nearest 45° (horizontal, vertical, or diagonal); release to resume freehand.
- The highlighter's **Box / Freehand** toggle and the Freehand marker color live in the toolbar
  property strip (text highlights are always yellow).
- While Note tool is active, a flag icon follows the cursor.
- Clicking an existing flag badge in the panel removes it (Cmd+Z to undo).
- Escape deactivates the active tool.

Annotations are stored in page-normalized coordinates and appear in both the panel viewer and on the canvas.

---

## Text Boxes

The **T** button in the top-left toolbar activates the text tool, shows a `T`
glyph at the cursor, and reveals the color/size picker (default **red, 16 pt**,
matching the pen). Style changes persist to every future box until changed.

| Input | Action |
|-------|--------|
| Drag on empty canvas (tool active) | Create a box the size of the drag, then type |
| Click (no drag) on empty canvas | Create a default-width box, then type |
| Escape (while typing) | Confirm + close the box; tool stays active |
| Escape (again) | Exit the text tool |
| Click off the box | Confirm + close the box; tool stays active |
| Single-click a box | Select it; picker shows that box's color/size |
| Double-click a box | Edit its text (and restyle via the picker) |
| Cmd/Ctrl + C, then V | Duplicate the selected box at an offset |
| Delete / Backspace | Remove the selected box |
| Cmd/Ctrl + Z | Undo a delete, text edit, style change, or move |

Editing the picker affects only the **selected** box; the changed style is then
inherited by the next new box. A dragged box fixes its width (text wraps) and
grows downward if the text overflows. A per-box **Scale** toggle in the property
strip makes the box's text scale with zoom, so margin notes stay proportional to
the page; boxes default to a fixed on-screen "sticky note" size. Boxes — including size and color — are
saved in the `.scholion` project file. While a box is open, the usual copy/paste
keys move text into and out of the field.

---

## Thread Lines

When a document is selected, cubic Bezier wires connect its pages in reading order —
similar to node-editor connection wires in Blender, DaVinci Resolve, etc.

Each wire exits from the nearest edge of its source page and enters the nearest edge
of the destination page, perpendicular to that edge. Handle length scales with
inter-page distance so the curve is always smooth regardless of layout.

---

## Project File Format

`.scholion` files are plain JSON:
```json
{
  "viewport": { "x": 0.0, "y": 0.0, "zoom": 0.6 },
  "documents": [
    {
      "path": "/path/to/file.pdf",
      "stack_origin": [100.0, 200.0],
      "pages": [
        { "index": 0, "x": 100.0, "y": 200.0 },
        { "index": 1, "x": 120.0, "y": 220.0 }
      ]
    }
  ]
}
```

---

## LOD / Performance

| Zoom level | Raster tier | DPI | Approx. VRAM per page |
|------------|-------------|-----|-----------------------|
| < 0.5 | Thumb | 72 DPI | ~1 MB |
| 0.5 – 2.5 | Low | 150 DPI | ~4.3 MB |
| ≥ 2.5 | High | 300 DPI | ~17 MB |

At 100 pages in Low tier: ~430 MB VRAM. Thumb fallback keeps it at ~110 MB.
High tier only loads for pages actually being read close-up.

---

## Status Overlay

A small overlay pins to the **bottom-left** corner showing a performance health
dot (green ≥ 50 FPS, amber 30–50, red < 30), a page count, and the current zoom.
Zoom is shown as a percentage where **100 % = print size** (1 PDF point = 1 logical
screen point, ~1/72 inch), independent of display pixel density.

| Input | Action |
|-------|--------|
| F3 | Toggle the status overlay (visible by default) |

---

## Architecture

```
Canvas Engine   — pan/zoom, coordinate conversion, frustum culling
Document Model  — stacks, loose pages, world positions, annotations
Renderer        — OpenGL 3.3 core (color + textured-quad shader programs)
InputHandler    — GLFW callbacks, hit-testing, single/multi-page drag, selection
Panel UI        — Dear ImGui sidebar, annotation tools, resize handle
```

**Tech stack:** C++17 · GLFW 3.4 · OpenGL 3.3 core · MuPDF 1.24.11 (AGPL) · Dear ImGui · tinyfd

> MuPDF is AGPL licensed. For closed binary distribution, swap to pdfium.

---

## Milestone Status

- [x] M1 — Infinite canvas, pan/zoom, GL 3.3 core, frustum culling
- [x] M2 — PDF rasterization (MuPDF), drag-and-drop loading, LOD tiers
- [x] M3 — Multi-document stacks, fanned layout, folder drop, hue stripe, thread wires
- [x] M4 — Hover highlight, deferred drag, panel viewer, right-click context menus
- [x] M5 — Annotations (pen/highlight/note), node-editor threads, multi-select, rubber-band
- [x] M6 — Multi-page selection, rubber-band box, group drag, Cmd+A
- [x] M7 — Save project (right-click canvas → writes `.scholion` JSON)
- [x] M8 — Project file load (open `.scholion` → restore viewport + positions)
- [x] M9 — Autosave every 60 s
- [x] M10 — Annotation persistence (strokes/highlights/note flags in `.scholion`)
- [x] M11 — .app bundle, Info.plist, AppIcon, ad-hoc codesign, `.scholion` file association
- [x] M12 — VRAM optimization (background LOD streaming, 350 MB budget, accurate eviction)
- [x] M13 — UX polish (remove document, zoom-to-fit, recent projects, window title, undo coverage)
- [x] M14 — Text & search (copy page text, Cmd+F full-text search, single-doc fit, panel nav)
- [x] M15 — Status overlay + Cmd+S save (F3 overlay, zoom %, Cmd+S, save-on-quit; text-box ESC/style)
- [x] M16 — UI polish: ESC tool deactivation, vignette toggle, flag cursor icon, flag removal by click, spacebar panel close, settings credits
- [x] M17 — v1.3: highlighter Box/Freehand modes (glyph-lock → References + freehand markers), pen Shift ortho-lock (45° straight lines), per-box zoom-scaling text, page rotation 180°/reset, event-driven render loop (near-zero idle CPU), tool-race + light-mode readability fixes
