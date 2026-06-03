# Scholion — Architecture Reference

## Three-Layer Model

```
┌─────────────────────────────────────────────────────────┐
│  Canvas Engine                                          │
│  canvas.h / src/canvas.cpp                             │
│  include/input.h / src/input.cpp                       │
│  include/texture_cache.h / src/texture_cache.cpp       │
│  Responsibilities: pan/zoom, frustum culling, input     │
│  dispatch, GPU texture lifecycle, VRAM budgeting        │
├─────────────────────────────────────────────────────────┤
│  Document Model                                         │
│  include/document.h                                    │
│  Responsibilities: Document, Page, PageAnnotations,    │
│  LodTier, fanned-stack geometry, annotation storage    │
├─────────────────────────────────────────────────────────┤
│  Renderers                                              │
│  include/renderer.h / src/renderer.cpp                 │
│  include/pdf_loader.h / src/pdf_loader.cpp             │
│  include/overlay.h / src/overlay.cpp                   │
│  Responsibilities: GL drawing, MuPDF rasterization,    │
│  annotation overlay, thread wires (Bezier), F3 overlay │
└─────────────────────────────────────────────────────────┘
```

`src/main.cpp` is the application entry point and owns all global state:
documents, text boxes, the undo stack, GLFW callbacks, the ImGui UI, save/load,
autosave, and the background rasterization thread.

`src/scholion_osx.mm` is the macOS-only Objective-C++ shim that registers Apple
Event handlers for `kAEOpenDocuments` (file-association launches).

---

## Coordinate Systems

Three coordinate spaces are in use simultaneously:

### Screen space (pixels)
- Origin: top-left corner of the GLFW framebuffer.
- Unit: physical framebuffer pixels.
- Used by: GLFW cursor events, ImGui, GL vertex data, `Renderer::set_projection`.

### World space (world units)
- Origin: canvas origin (0, 0), visible near the center of the default view.
- Unit: 1 world unit = 1 PDF point = 1/72 inch.
- Used by: `Page::world_pos`, `Page::world_w/h`, `CanvasTextBox::world_pos`,
  annotation storage, `Canvas::pan/zoom`.

### Page-normalized space [0, 1]²
- Origin: top-left corner of the page.
- Unit: fraction of page width (x) or height (y).
- Used by: `AnnotHighlight` (x0, y0, x1, y1), `AnnotStroke::pts`.
  Stored in the project file; independent of page size changes.

### Transforms

```
world_to_screen(p) = (p - canvas_offset) * zoom + viewport_center
screen_to_world(p) = (p - viewport_center) / zoom + canvas_offset
```

`viewport_center` is `(viewport_width/2, viewport_height/2)`.

To convert annotation coords to world space:
```
world_x = page.world_pos.x + norm_x * page.world_w
world_y = page.world_pos.y + norm_y * page.world_h
```

---

## Threading Model

### Main thread (only)
- All OpenGL calls (texture uploads, draw calls, buffer updates).
- All GLFW callbacks (`mouse_button_callback`, `key_callback`, etc.).
- All ImGui frame construction and rendering.
- `drain_rast_results()` — transfers completed pixel buffers to the GPU.
- `stream_lod()` — decides which LOD tiers to enqueue or evict.

### Background rasterization thread (`rast_worker`)
- One thread; started at app launch, stopped at shutdown.
- Calls `PdfLoader::rasterize_to_buffer()` (MuPDF CPU rasterize, no GL).
- Communicates with the main thread via two mutex-protected queues:
  - `g_rast_tasks` — work items (in); consumed by the worker.
  - `g_rast_ready` — results (out); drained by `drain_rast_results()`.
- After writing a result, calls `glfwPostEmptyEvent()` to wake the main loop.

### Download thread (`download_thread_fn`)
- Created on demand when the user pastes a URL into the "Add from URL" modal.
- Uses libcurl; writes to a file in `~/Downloads`.
- Communicates state via `g_dl_state` (atomic int: 0=idle, 1=in-flight,
  2=done, 3=failed) and `g_dl_path` / `g_dl_error` (written before the
  atomic store, read after the acquire load in the main thread).

---

## Project File Format (.scholion)

Hand-written JSON; position/record-based parser (not line-based) for robustness.
File size limited to 20 MB on load. Top-level structure:

```json
{
  "version": 1,
  "viewport": { "x": 0.0, "y": 0.0, "zoom": 0.6 },
  "documents": [
    {
      "path": "/abs/path/to/file.pdf",
      "stack_origin": [0.0, 0.0],
      "pages": [
        { "index": 0, "x": 0.0, "y": 0.0, "w": 612.0, "h": 792.0 }
      ]
    }
  ],
  "text_boxes": [
    { "id": 0, "x": 100.0, "y": 200.0, "r": 0.82, "g": 0.06, "b": 0.06,
      "fs": 16.0, "w": 200.0, "h": 0.0, "text": "hello" }
  ],
  "annots": [
    { "doc": 0, "page": 1, "hl": [0.1, 0.2, 0.9, 0.25] },
    { "doc": 0, "page": 2, "note": "A" },
    { "doc": 0, "page": 3, "sr": 0.82, "sg": 0.06, "sb": 0.06 },
    { "p": [0.10, 0.20] },
    { "p": [0.15, 0.22] }
  ]
}
```

Notes:
- `path` is the absolute path as of the last save. If the file is missing at
  load time, a placeholder Document is created (`doc.missing = true`); the path
  is preserved for re-save and relinking.
- `w`/`h` in pages are world-unit dimensions (added in M18). Older files that
  omit them fall back to US Letter (612 × 792 pt).
- `text_boxes.w/h` are screen pixels at creation zoom (0 = auto-size).
- Annotations use `doc`/`page` as *saved* indices; a `doc_map` in the loader
  re-keys them to actual indices after missing-PDF slots shift everything.
- Pen stroke points follow their stroke header as consecutive `{ "p": [...] }`
  records (no wrapper array, to avoid quadratic parse cost on large strokes).

---

## LOD / VRAM Strategy

Three raster tiers per page (see also CLAUDE.md and `pdf_loader.cpp`):

| Tier  | DPI | ~VRAM/page | Zoom range    | Eviction      |
|-------|-----|-----------|---------------|---------------|
| Thumb |  72 |  1.1 MB   | < 0.5         | Never         |
| Low   | 150 |  4.3 MB   | 0.5 – 2.5     | Budget-gated  |
| High  | 300 | 17   MB   | ≥ 2.5         | Unconditional |

Budget gate: 350 MB (`VRAM_BUDGET` in `main.cpp`). Low tiers are only evicted
when total VRAM use exceeds this threshold, preserving them across zoom changes.

---

## Undo Stack

Records are typed (`UndoRecord::Type`). Key design decisions:

| Type           | Granularity    | Notes                                           |
|----------------|----------------|-------------------------------------------------|
| PageMove       | One record / drag | Batches all pages in a group drag             |
| TextBoxMove    | One record / box  | Boxes are individually addressed by ID        |
| TextBoxEdit    | One record / editing session | Snapshot on open; push on close if changed |
| TextBoxStyle   | One record / selection session | Snapshot on first click; push on deselect |
| PenStroke      | One record / stroke committed | Pushed on mouse release                  |
| Highlight      | One record / rect committed   |                                           |
| Note           | One record / stamp            | Stores `note_idx_before` to restore counter |
| ErasedStroke   | One record / erased stroke    | Full stroke copy for re-insertion         |
| ErasedHighlight| One record / erased highlight |                                           |
| TextBoxCreate  | One record / creation         | Stores full box copy for rollback         |
| TextBoxDelete  | One record / deletion         | Stores full box copy for re-insertion     |

Stack is capped at 60 records (`UNDO_LIMIT`). Raw `Page*` in `PageMove` records
are purged by `remove_document()` before the page vector is erased.

---

## Selection Model

Three fields track "what's selected," serving different purposes:

| Field                            | Location     | Meaning                                   |
|----------------------------------|--------------|-------------------------------------------|
| `InputHandler::m_selection`      | input.h      | Set of `Page*` selected for group drag/delete |
| `InputHandler::m_selected_text_boxes` | input.h | Set of text-box IDs in the unified selection |
| `g_selected_box`                 | main.cpp     | ID of the box with text-tool focus (style picker, edit trigger) |

`m_selection` and `m_selected_text_boxes` are the **unified selection** — always
cleared/modified together by `clear_selection()`. They drive group drag, the
dotted selection border, and the Delete key handler.

`g_selected_box` is the **text-tool focus** — the box whose style is shown in the
toolbar picker and that receives a double-click-to-edit. It can be set
independently of `m_selected_text_boxes` (e.g., single-click in the text tool
replaces the unified selection but also sets `g_selected_box`).

`g_editing_box` (-1 when not editing) indicates the box currently open for text
input. When set, its ImGui input window is rendered and keyboard events go to
ImGui instead of the canvas.
