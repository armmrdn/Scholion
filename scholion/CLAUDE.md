# Scholion — Infinite Canvas PDF Workspace

## What This Is
A native desktop application for research professionals to view, arrange, and annotate
multiple PDF documents on a GPU-accelerated infinite canvas. Inspired by PureRef's UX
model but designed for multi-page PDF documents with document-awareness features.

## Target Use Case
Researchers working with ~10 peer-reviewed scholarly articles (up to 10 pages each, ~100
pages max on canvas simultaneously). Solo-use, local-first, project-file based.

## Tech Stack
- **Language:** C++17
- **Windowing/Input:** GLFW 3.x
- **Rendering:** OpenGL 3.3+ (portable; Metal backend possible later)
- **PDF Rasterization:** MuPDF (AGPL — swap to pdfium if distribution needed)
- **UI Overlays:** Dear ImGui (for menus, dialogs, toolbars)
- **Build System:** CMake 3.20+
- **Target Platforms:** macOS first, then Windows/Linux

## Repository Layout
```
~/Desktop/Scholion/
  ├── scholion/              — macOS build + all shared cross-platform source
  │   ├── include/           — public headers
  │   ├── src/               — implementation files (main.cpp, renderer.cpp, …)
  │   ├── resources/         — AppIcon.icns, Info.plist
  │   ├── third_party/       — imgui, mupdf, tinyfiledialogs
  │   ├── scripts/           — make_icon.py
  │   └── build/             — CMake output; Scholion.app lives here
  ├── scholion-win/          — Windows CMake overlay + platform stubs
  ├── .github/workflows/     — CI/CD (build.yml)
  └── RELEASE_NOTES.md       — release notes embedded into each CI release
```

## Architecture (Three Layers)
1. **Canvas Engine** — pan/zoom/culling, GPU texture management, input handling
2. **Document Model** — stacks, loose pages, positions, parent relationships, annotations
3. **Renderers** — PDF rasterizer, annotation overlay, threads (node-editor Bezier wires)

## Key UX Behaviors (PureRef-inspired)
- Minimal chrome, canvas is the entire window
- Right-click context menus instead of persistent toolbars
- Items feel physical (shadows, depth, grab-and-move)
- Zoom centers on cursor position
- Smooth pan with middle-mouse or space+drag

## Keyboard & Mouse Reference
Modifier is Cmd on macOS / Ctrl elsewhere unless noted (Cmd+A is Super-only today).

**Navigation**
| Action | Binding |
|---|---|
| Pan | Middle-mouse drag, or Space+left-drag |
| Zoom (cursor-centred) | Scroll wheel |
| Zoom to fit all | Cmd+0 |
| Zoom to fit one document | Right-click page → Zoom to Fit Document |
| Toggle status overlay (FPS/pages/zoom) | F3 |
| Full-text search | Cmd+F |

**Selection & movement** (pages / documents)
| Action | Binding |
|---|---|
| Select page (shows threads, selection border) | Left-click page |
| Open document panel (scrolled to page) | Double-click page |
| Toggle panel for selected document | Space (tap, no drag) |
| Close panel | Click empty canvas, or Space tap again |
| Move a page | Left-drag page (4 px deferred threshold) |
| Select whole document | Shift+click any of its pages (toggles in/out) |
| Add/toggle individual page in selection | Cmd+click page (Ctrl on non-macOS) |
| Select all pages on canvas | Cmd+A |
| Rubber-band select | Left-drag empty canvas |
| Move the whole selection | Left-drag any selected page |
| Clear selection / close panel | Escape |
| Remove a document | Right-click page → Remove Document |

**Text boxes** (toolbar **T**)
| Action | Binding |
|---|---|
| Create box | With T active, drag on empty canvas (bare click = default box) |
| Edit text | Double-click box |
| Select box (show its style) | Single-click box |
| Confirm + close edit | Escape, or click off the box |
| Exit text tool | Escape again |
| Duplicate selected box | Cmd+C then Cmd+V |
| Delete selected box | Delete / Backspace |
| Restyle | Color/size picker in toolbar (applies to selected box) |

**Annotations** (panel header buttons, applied in the side panel)
| Tool | Behaviour |
|---|---|
| Pen / Highlight / Flag / Erase | Draw on the page open in the panel |
| Shift (while drawing Pen or Freehand highlight) | Lock the stroke to a straight line, snapped to nearest 45° |
| Highlighter Box / Freehand toggle | Box = rectangle drag (reliable glyph capture); Freehand = marker swipe (toolbar property strip) |

**Project & undo**
| Action | Binding |
|---|---|
| Save (overwrite, or Save-As if none set) | Cmd+S; also right-click canvas → Save Project |
| Open / New project | Right-click canvas → Open/New Project |
| Undo (annotations, moves, text-box create/edit/style/delete) | Cmd+Z |
| Relink a missing PDF | Right-click its grey placeholder → Relink PDF… |
| Page context menu | Right-click page |
| Canvas context menu | Right-click empty canvas |

> Escape priority (top first): finish text edit → exit text tool → clear selection → close panel → cancel annotation tool.

## Project File Model
```
/user-chosen-folder/
  ├── article1.pdf
  ├── article2.pdf
  └── project.scholion
```
Project file stores: stack positions, loose page positions, annotations
(strokes, highlights, notes), canvas viewport state, text boxes.
Autosaves every 60 s when a project path is set.
Recent projects persisted to `~/.scholion_recents` (10 entries).

## Milestones
1. ✅ Window + infinite canvas with pan/zoom (GL 3.3 core, frustum culling)
2. ✅ PDF rasterization to GPU textures, display pages on canvas
3. ✅ Multi-document stacks — fanned page layout, folder drop, hue stripe, node-editor thread wires
4. ✅ Interaction — hover highlight, deferred drag (4px), panel viewer, right-click context menus
5. ✅ Annotations — pen (freehand), highlight (rect), note flags (A–Z, 2A…); page-normalized coords; visible on canvas and in panel
6. ✅ Multi-page selection — Shift+click doc, rubber-band box, group drag, Cmd+A; blue outline on selected pages
7. ✅ Save project — right-click canvas → "Save project…" → writes .scholion JSON
8. ✅ Project file load — right-click canvas → "Open project…" → tinyfd picker → restore viewport + page positions
9. ✅ Autosave every 60 s; timer resets on manual save or project open
10. ✅ Annotation persistence — strokes/highlights/notes serialized in .scholion; g_next_note_idx restored on load
11. ✅ .app bundle — MACOSX_BUNDLE, Info.plist, AppIcon.icns, ad-hoc codesign; .scholion file association via CFBundleDocumentTypes
12. ✅ VRAM optimization — per-document PdfLoader; Thumb-only on load; background rasterization thread streams LOD tiers; 350 MB budget enforced; accurate eviction accounting
13. ✅ UX polish — remove document, zoom-to-fit (Cmd+0), recent projects, window title shows project name, New Project action, undo covers all annotation/move/resize/textbox ops
14. ✅ Text & search — Copy Page Text (right-click page or panel "Copy Text" button); full-text search across all docs (Cmd+F); zoom-to-fit single document (right-click page); panel page navigation (< p.N/Total > buttons); Add PDF/folder directly from canvas right-click menu
15. ✅ Status overlay & Cmd+S save — bottom-left F3-toggle overlay (FPS health dot, page count, zoom %); `Canvas::get_zoom_percentage()` (100% = print size, `m_zoom * 100`); Cmd+S (silent overwrite or Save-As) + save-on-quit; font/color persist between uses and sync to the edited box. Overlay lives in `src/overlay.cpp` / `include/overlay.h`. Startup Open-Project picker on bare launch only — a kAEOpenDocuments event (double-clicked `.scholion`/PDF) or CLI arg suppresses it (brief event-pump grace window in `main()` before the loop).
16. ✅ Text-box overhaul — `CanvasTextBox` gains `w`/`h` (px; 0 = auto); press-drag-release creation sizes the box (drag fixes width, height grows to fit; bare click = default width); default color red `(0.82,0.06,0.06)` like the pen; two-stage ESC (confirm+close, then exit tool); single-click select shows the box's style in the picker and picker edits apply to the selected box only (then inherited by new boxes); Cmd+C/Cmd+V duplicate the selected box (text copy/paste inside the field handled by ImGui); undo extended with `TextBoxEdit` (per edit session) and `TextBoxStyle` (per selection session). `.scholion` text_boxes records gain `w`/`h` (back-compatible: older files omit them → auto-size).
17. ✅ Save/load fixes — **text boxes were wiped on load** by the post-parse `g_text_boxes.clear()` (parse pushed directly into the live vector); now parsed into a temp and assigned after the reset. **Missing-PDF document-index shift** dropped/misattached annotations on load; load now builds a `doc_map` (saved index → actual index, skipping missing PDFs) and re-keys annotations through it. `.scholion` passed on the CLI (or via Finder) now opens as a project, not a PDF. Bare launch shows an in-app **startup chooser** (Open Project / Add PDF(s) / Add Folder / blank) instead of a single Open dialog. Right-click **Save Project** shows the `Cmd+S` shortcut and updates the existing file (Save-As only if none set). `save_to_path`/`load_project_from_path` print restored/written counts for diagnostics. **Missing-PDF placeholders:** a document whose PDF can't be found at load is kept as a placeholder `Document` (`Document::missing`) in its original slot — pages (saved positions, default Letter size) and annotations preserved, `g_loaders` slot `nullptr` (skipped by `stream_lod`), rendered as a distinct grey page; re-save writes the reference + annotations back so moved/renamed PDFs don't lose data. Only the path is stored, never PDF bytes.
18. ✅ Code audit & hardening — viewport (zoom/pan) now restored on load (was parsed only inside the docs section, but written before it); macOS `argv` open guarded with `#ifndef __APPLE__` so a CLI/Finder file loads once (the Apple event already delivers it); removed unused `Renderer::m_proj` (`-Wall -Wextra` clean across all TUs); diagnostic load/save counts gated behind `SCHOLION_DEBUG` env; per-page `w`/`h` persisted (placeholders use real size, Letter fallback); `new_project()` clears the text-box clipboard; **relink** — right-click a missing-PDF page → "Relink PDF…" loads the real file into that slot, carrying over positions + annotations; **parser hardened** — `load_project_from_path` is now position/record-based (tolerant of compact/pretty/reordered layouts, CRLF, whitespace) instead of line-based.
19. ✅ Selection/visual polish — double-click an existing text box now edits it (the just-opened edit window no longer self-closes via a stale `WantCaptureMouse` on its first frame; guarded by `g_prev_editing_box`); selection borders are now **thin dotted grey**, offset in screen-space (`draw_rect_dashed` in the renderer; `imgui_dashed_rect` for text boxes) so they're visible at any zoom and not bold; thread wires reduced to ~40% more transparent (core α `0.42→0.25`, glow `0.07→0.04`); the bottom-left overlay page counter now shows live **visible/total** pages.
20. ✅ Unified text-box selection + save feedback — **Cmd+click text boxes** now toggle them into the unified selection set (alongside page selection); single-click replaces the selection; delete/backspace removes all selected items (pages + text boxes); dotted borders shown on all selected boxes. **Save feedback:** manual save (Cmd+S, right-click) shows **"Saved!"** in top-right corner, fades over 1 second; autosave shows subtle **"saved"** text, fades over 0.5 seconds.
21. ✅ Polish multi-box dragging — **rigid group movement:** replaced frame-delta tracking with grab-point-relative delta; store initial positions of all selected boxes at drag start, compute `delta = current_world - grab_point`, apply to all boxes each frame, maintaining relative offsets. Push TextBoxMove undo records for each moved box.
22. ✅ Cmd/Ctrl+click individual pages — **modifier-click multi-select for pages:** `Cmd+click` (macOS) / `Ctrl+click` (other) toggles an individual page in/out of the selection without clearing other selected items. Complements existing `Shift+click` (whole-document toggle). Consistent with Cmd+click for text boxes (M20). After Cmd+clicking, drag is immediately ready if selection is non-empty. Cmd+click on empty canvas starts rubber-band without clearing selection.
27. ✅ Panel interaction redesign + text-box occlusion fix — **single-click on a page now selects it** (adds to `m_selection`, shows threads + selection border) without opening the panel. **Double-click on a page opens the panel** scrolled to that page. **Space tap** (press+release with no drag) toggles the panel for the selected document (opens scrolled to first selected page; closes if already open). Canvas click still closes the panel (existing). **Text boxes no longer render on top of the panel**: `draw_canvas_text_boxes` wraps all `ForegroundDrawList` calls in a `PushClipRect`/`PopClipRect` pair that excludes the panel region; hit-testing also excludes the panel area so boxes behind it aren't clickable.
28. ✅ Windows port — Phase 1 (cross-platform source + Windows build tree). **Bug fix:** space
    key-up was swallowed by `key_callback`'s `action != GLFW_PRESS` early return before
    `g_input.on_key()` — `m_space_held` stuck `true`, all left-drags panned instead of moving
    items. Fixed by calling `on_key()` before the PRESS-only guard; added
    `glfwSetWindowFocusCallback` to clear stuck keys on focus loss. **Cross-platform source:**
    `#ifdef _WIN32` / `#ifdef __APPLE__` / `#else` (Linux stub) in `main.cpp` for Windows
    headers (GLAD + Win32 shell APIs), `download_dir()`, `recents_file_path()`,
    `reveal_in_file_manager()`; GLAD loader init after `glfwMakeContextCurrent`;
    `renderer.cpp` + `texture_cache.cpp` use `<glad/glad.h>` on non-Apple. **`../scholion-win/`
    build tree created:** CMakeLists.txt (references shared `../scholion/src/`), GLAD 3.3 Core
    source generated, AppIcon.ico converted (6 sizes), DPI-awareness manifest, Windows resource
    file, `setup_windows.ps1` one-script environment bootstrap. CI builds MuPDF from source
    (cached). `Scholion-Windows.zip` ships with all MinGW DLLs bundled.

## History

The project is on GitHub (`armmrdn/Scholion`, private). Use `git log` and `git checkout` for history.

Full archive of every plan, decision, removal, and idea lives in `../DEVLOG.md` (local-only, never committed).

## LOD / VRAM Strategy
Zoom level drives which raster tier is loaded by the background worker (see `include/document.h`):
- `LodTier::Thumb` (72 DPI)  — zoom < 0.5: loaded at startup, ~1.1 MB/page
- `LodTier::Low`  (150 DPI)  — zoom 0.5–2.5: normal reading, ~4.3 MB/page
- `LodTier::High` (300 DPI)  — zoom ≥ 2.5: close reading, ~17 MB/page

Pages appear immediately as warm-white placeholders; a single background thread
rasterizes and posts pixel buffers; the main thread uploads results each frame.
Off-screen High tiers are evicted unconditionally; Low tiers are evicted when
VRAM exceeds 350 MB. `TextureCache` tracks per-texture byte counts accurately.

## Build Instructions

```bash
# macOS — one-time dependency
brew install glfw
# MuPDF is bundled in third_party/mupdf — no Homebrew install needed

# Configure + build (from repo root)
cmake -S scholion -B scholion/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSCHOLION_WITH_MUPDF=ON
cmake --build scholion/build -j$(sysctl -n hw.ncpu)

open scholion/build/Scholion.app
```

Ad-hoc signing (`codesign -s -`) is applied automatically at build time — sufficient for local use. For notarized distribution, replace `-` in CMakeLists.txt with a Developer ID certificate and run `xcrun notarytool submit`.

## CI/CD Workflow

Single workflow: `.github/workflows/build.yml`

**Triggers:**
- Push to `main` or `develop` → both platform builds run as a compile check; no release
- Push of a `v*` tag → full build + release

**Release sequence (sequential, no artifact storage):**
1. `build-macos` runs, produces `Scholion.dmg`, creates a GitHub draft release tagged with the version, attaches the DMG, and embeds `RELEASE_NOTES.md` as the release body
2. `build-windows` and `build-linux` run in parallel after macOS (`needs: build-macos`), each producing a zip and uploading it to the draft release via `gh release upload`
3. `publish` runs after both Windows and Linux (`needs: [build-windows, build-linux]`), sets `draft=false` to make the release live

No `actions/upload-artifact` is used — artifacts go directly to the GitHub release draft, avoiding the storage quota entirely.

**RELEASE_NOTES.md convention:** this file presents a **complete, self-contained picture of the
product AT the current version** — NOT a detached delta of upgrades/fixes that only makes sense to
someone who tracked the prior version. Grouped under stable `//`-prefixed category headers
(`### //Tools` with **bold sub-groups** like **Pen**/**Highlighter**/**Text Boxes**, `### //Canvas`,
`### //Sidebar Viewer & References`, `### //UI`, `### //<platform>`, and `### //Bug Fixes` last).
Keyboard keys in **bold**. Use **contextual language** that reads as an update while also explaining
what the feature does / the problem it solves — e.g. *"The pen tool now writes on the canvas as well
as pages, with marks locking to the surface their origin is on,"* not the bare *"Added canvas pen
strokes."* Do NOT stack prior versions in the file. When cutting a new version, truncate and rewrite
the full snapshot; older releases keep their own notes on their GitHub release pages.

**Cutting a release:**
```bash
# 1. Update RELEASE_NOTES.md (truncate to just this version), bump version.h, commit, push to main
git push origin main

# 2. Create a version tag via the GitHub API
#    (direct tag push is blocked by branch protection rules)
gh api repos/armmrdn/Scholion/git/refs \
  --method POST \
  --field ref="refs/tags/vX.Y" \
  --field sha="$(gh api repos/armmrdn/Scholion/git/refs/heads/main --jq '.object.sha')"
```

CI handles everything after step 2. Do not publish a manual GitHub release before CI runs — GitHub does not allow uploading assets to a published release, and there is no way to convert a published release back to draft.

**Tag immutability:** once a tag name (e.g. `v1.0`) has been used with a published release, GitHub permanently locks that name — even after deleting both the release and the tag ref. Always increment to a new tag name for each release.

## Code Conventions
- Use `snake_case` for functions and variables
- Use `PascalCase` for types/classes
- Keep headers in `include/`, implementations in `src/`
- Prefix private members with `m_`
- Use `// TODO:` comments for future work

- ## Honesty rules (read every turn)

Before claiming a function, class, or import exists, verify it by reading
the file or running a grep. Never fabricate symbols.

If you cannot verify something, say "I haven't verified this" explicitly.
Do not write code that depends on the unverified claim.

If a task asks you to use a library you've never seen referenced in this
project, ask before adding it.

If a task involved tests or builds, do not claim success unless you
actually ran the test or build command in this session.

Never invent error messages, API responses, or stack traces. If you
didn't see them, say so.

When you genuinely don't know, the correct answer is "I don't know" or
"I need to check first." Both are better than a confident guess.

## Verification protocol

Before writing or editing code that uses a symbol (function, class, type,
constant), do one of:

1. Read the file where it's defined and confirm the signature
2. Run `grep -r "symbolName" .` or use the Glob tool to find it
3. Check package.json, requirements.txt, Cargo.toml, or equivalent for
   the dependency

If you skip verification, prefix the code with a comment:
`// UNVERIFIED: I have not confirmed this symbol exists`

Plan-then-execute mode is preferred for any task touching more than one
file. Use Shift+Tab to enter plan mode before starting.
