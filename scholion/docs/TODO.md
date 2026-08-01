# Scholion — TODO / Backlog

Consolidated backlog (updated 2026-07-28). Tiered by when it must happen. The **Completed**
archive is at the bottom.

---

## Tier 0 — v1.4 ship readiness (nothing blocking if pushing as-is)

v1.4 is feature-complete and builds clean on all three trees; the headless save/load self-test
(`Scholion --selftest`) runs in CI. If pushing v1.4 as-is, **no engineering work is required** — the
only true gate items (Tier 1) are decisions, and the current AGPL/private-repo posture is internally
consistent (see Tier 1). Remaining polish is Tier 3+.

- [ ] **Finalize `RELEASE_NOTES.md` + commit + tag** — the notes are drafted (full v1.4 snapshot).
      Sequence: commit → push `main` → create `v1.4` tag via the GitHub API (branch protection blocks
      direct tag push). CI builds/publishes all three platforms. (Task #7.)

---

## Tier 1 — DECIDE & ACT (final items; owner: you)

These are the real gate for a *public* release. They are decisions, not code — though (a) can spawn
a large engineering effort depending on the answer.

- [ ] **(a) Licensing / distribution model — AGPL vs. closed.** MuPDF is **AGPL-3.0**. Distributing
      binaries linked against it obliges offering complete corresponding source under AGPL.
      Two consistent paths:
      - **Open-source under AGPL** (repo goes public, source offered) → v1.4 can ship now as-is.
      - **Keep closed / proprietary** → must swap the PDF backend (see Tier 2e) *before* any public
        binary distribution. Until then, keep the repo **private** and distribution **restricted**.
      The current state (private repo, restricted releases) is fine as a holding pattern. This
      decision drives (b), Tier 2e, and the whole distribution roadmap.
- [ ] **(b) Notarize (macOS) + code-sign (Windows).** Today: ad-hoc macOS signing → Gatekeeper
      "unidentified developer"; unsigned `.exe` → SmartScreen warning. For a professional audience:
      Apple Developer ID + `notarytool` (replace the ad-hoc `-` in `scholion/CMakeLists.txt`), and an
      Authenticode cert for Windows. Requires paid certs (Apple $99/yr; Windows OV/EV cert). Do
      before wide public release; not needed while distribution is restricted.

> Leaning per the owner: **push v1.4 as-is now**, and defer (a)/(b) + Tier 2 to a later major version
> that reworks distribution.

---

## Tier 2 — Deferred to a future major version (large refactor / restructure)

High structural value, high regression risk — intentionally **not** in v1.4. Group these into a
"v2 / restructure" milestone.

- [ ] **(e) PDF backend swap — ONLY IF going closed-source.** Replace AGPL MuPDF with pdfium (BSD)
      or a commercial MuPDF license. This is the single dependency blocking proprietary distribution;
      every other bundled dep is already permissive (ImGui MIT, GLFW zlib, tinyfiledialogs zlib, GLAD
      MIT/PD, DejaVu Sans Bitstream Vera). Sizeable port (raster + text-extraction + search paths in
      `pdf_loader.cpp`).
- [x] **Stable page/document IDs (replace raw `Page*` identity).** ✅ DONE (1.5 Phase 1): `uint64_t Page::id` + `page_by_id()` resolve-at-use; selection/undo/renderer keyed by id. Selection, drag, undo, hover, and
      thread ordering all store raw `Page*` into `std::vector<Page>`, which reallocates on add/remove
      — the root cause of the whole class of pointer-invalidation bugs patched over time (undo-scrub
      on doc removal, `clear_selection` on `set_documents`, relink carry-over). Introduce integer
      handles + lookup. Also gives the file format a real stable key (helps the note-migration story).
- [x] **Carve up `main.cpp` (~4,900 lines).** ✅ DONE (1.5 Phase 3): 4,840 → ~2,276 across 9 TUs —
      undo, groups, settings, toolbar, text_boxes, references_panel, side_panel, dialogs, input_glue.
      Extract input handling, the undo system, the sidebar/
      panels, and the toolbar into their own translation units. Biggest maintainability win.
- [x] **Replace the hand-rolled tolerant JSON parser** ✅ DONE (1.5 Phase 2): PicoJSON tree parser +
      serializer; retired the sscanf scanner and the snprintf writer. (`project_io.cpp`) with a real JSON parser
      (e.g. a single-header lib). Removes the substring-based section inference and per-record
      `sscanf` brittleness (the fixed buffer-overflow was a symptom); enables a proper schema/
      migration framework beyond the current `format:1` + tolerant reads.
- [ ] **Distribution restructure** — installer (Windows `.scholion` association, Start-menu entry),
      auto-update mechanism (Sparkle on macOS / equivalent), and the signing pipeline from Tier 1b.

---

## Tier 3 — Smaller pending features

- [ ] **Windows `.scholion` file association.** `argv[1]` handling already works; only the registry
      association is missing (NSIS/WiX installer step or first-run `HKEY_CLASSES_ROOT` write).
- [ ] **Icon upscaling for high-DPI / large formats.** Produce a 1024×1024+ master (we now have
      `resources/scholion_icon_1024.png`); regenerate `.icns`/`.ico` via `scripts/make_icon.py`; add
      256/512 PNGs for Linux `.desktop` launchers and include them in the Linux CI bundle.
- [ ] **Performance for low-end hardware.** Frame-timing/idle was done in v1.3 (event-driven loop).
      Still open: batch MuPDF calls, adaptive LOD by available VRAM, texture pooling/streaming,
      batched draw calls, and reducing per-frame hit-test cost on large canvases (see robustness).

---

## Tier 4 — Robustness, QA & sustainability (recommended for future releases)

- [ ] **Expand the test corpus.** The self-test covers a happy-path round-trip + overflow + note
      migration. Add: a corpus of real/old `.scholion` files, more crafted-corrupt inputs, and light
      fuzzing of the parser. (Largely subsumed by the "real JSON parser" refactor.)
- [ ] **Spatial index for scale.** Every overlay iterates all documents/pages each frame (O(pages)).
      Fine at the stated ~100-page ceiling; a spatial index is needed to scale materially beyond it.
- [ ] **Keep MuPDF current.** MuPDF has a real CVE history; track upstream and bump the bundled
      version periodically (calls are already `fz_try`/`fz_catch`-wrapped, so crashes are contained).
- [ ] **`doc.hue` stability.** Document color is derived from load order, so a doc's hue (and its
      group's boundary color) can shift if docs are added/removed between sessions. Persist the hue
      per document to fix the cosmetic instability.
- [ ] **Build-system dedup.** A new source file must be added to all three CMakeLists — a documented
      footgun. Factor the shared source list into one `*.cmake` include.
- [ ] **Repo consolidation / de-cruft before the next major push.** Sweep the whole repository and
      keep only assets and files that are actively used: prune stale/duplicate docs (SESSION_SUMMARY,
      redundant CLAUDE/README overlap), dead scripts, orphaned resources, leftover local backup
      tarballs and `.icloud` stubs, and any unreferenced headers/assets. Goal: a lean tree where
      everything present is built, shipped, or documented — nothing vestigial.
- [ ] **Embedded-asset weight.** `include/font_data.h` (~1.6 MB) + `include/logo_data.h` (~230 KB)
      are generated headers. Acceptable, but a subsetted font (fonttools) would shrink the font
      embed substantially if compile time/repo size becomes a concern.
- [ ] **Optional, longer-horizon:** localization/i18n (English-only today), an opt-in crash reporter
      or update check, and accessibility passes beyond the Larger-UI toggle.

---

## Notes on IP protection (only relevant if you go closed-source)

- The **only** legal blocker to proprietary distribution is MuPDF (AGPL) — swap it (Tier 2e). All
  other bundled components are permissively licensed and bundle-able in a closed binary; keep their
  license texts shipped (DejaVu's is in `third_party/fonts/DejaVu-LICENSE.txt`).
- Until the swap, protect the IP simply by **keeping the repo private and restricting binary
  distribution** — do not publish AGPL-linked binaries to a public release page.
- Once closed: signed/notarized binaries (Tier 1b) double as authenticity/anti-tamper identity.
  Heavy obfuscation is generally not worth the effort for a desktop app of this kind.

---

## Completed

### ✓ Dev/benchmark code stripped — 2026-07-28
Removed the `--benchmark` `run_benchmark()` block + arg handler, the CSV-logger/Developer-Mode code
and its globals/includes (psapi, mach), and the `SCHOLION_DEV` CMake option + block in all three
trees. No references remain; release binaries unaffected.

### ✓ Parser buffer-overflow fix + headless round-trip self-test — v1.4
Bounded the unbounded `sscanf("%[^\"]")` path/rel/note reads (`%4095[^\"]`). Added `src/selftest.cpp`
(`--selftest`): headless save/load round-trip over every persisted field + an over-long-path overflow
regression + a legacy note-migration case. Wired into CI (build-macos; non-zero exit fails the build).

### ✓ Reference-note keying rewrite + bundled font — v1.4
Notes now live on `AnnotHighlight::note` (was keyed by fragile list position → mis-keyed/orphaned).
Editing tracks a stable `AnnotHighlight*`; serialized as per-highlight `rn`; pre-v1.4 files migrated.
Bundled DejaVu Sans (embedded, ImGui-compressed) replacing the ProggyClean bitmap font → real Unicode
(ellipsis, dashes, ·, °, Greek) + crisp Larger-UI scaling.

### ✓ Cross-document page grouping — v1.4
`group_id` on Page + a groups table; document-colored dim rounded boundary (concentric per doc);
renamable label; frame/label-handle move; **Cmd/Ctrl+G** group, **Cmd/Ctrl+Shift+G** ungroup;
drag-and-hold (~0.8s) to add; right-click **Remove Page from Group** (with a fading leave-group
flash); nondestructive, undoable; back-compatible save/load; relink preserves membership.

### ✓ Larger-UI toggle + startup-chooser centering + app logo — v1.4
Settings → Appearance "Larger UI" scales all chrome (FontGlobalScale + ScaleAllSizes), persisted.
Startup chooser re-centered (measures widest element) in both UI modes. Embedded app logo in the
splash + settings footer.

### ✓ Canvas z-layering fix — v1.4
Text boxes + cursor icon moved from the Foreground to the Background draw list so context menus /
tooltips / dialogs render on top of them (was: text boxes painted over the right-click menu).

### ✓ Encrypted / password-protected PDF placeholder — v1.4
`PdfLoader::load` checks `fz_needs_password`; a locked PDF stands in as a distinct indigo placeholder
page labelled "Password-protected PDF" (context menu shows it too), skips rasterization, re-derived
on load.

### ✓ Rotation simplified to a single Rotate 90° — v1.4
Replaced CW/CCW/180/Reset with one clockwise 90° step (repeat to reach any orientation). 45° confirmed
infeasible without a cross-cutting rotated-geometry refactor.

### ✓ References sidebar polish — v1.4
Open Documents list always visible; document threads take the doc's hue when toggled from the sidebar;
per-reference disclosure arrow; captured references preserve line breaks.

### ✓ Normalize Size — v1.4
Right-click a document → scale it to the median page height of the other docs (fallback Letter),
about its centroid. New `UndoRecord::Type::DocScale`.

### ✓ Canvas pen marks — v1.4
Pen strokes started on empty canvas become world-locked `g_canvas_strokes` (drawn in front); page
strokes unchanged. Own render pass, eraser, undo, and `.scholion` persistence (annots `doc=-1`).

### ✓ Tool movement-lock — v1.4
While any annotation/text tool is active, item drag/selection is suppressed so drag-creating a box no
longer moves the page.

### ✓ File compatibility + durability hardening — v1.4
`.scholion` header gains `format`/`app`/`hash`; FNV-1a integrity verify + newer-format guard →
suspect loads warn + suppress autosave (`g_load_ok`); portable relative `rel` paths; atomic write +
`fsync` + one-deep `.bak` (removed on clean quit); standing save-status indicator.

### ✓ Highlighter Box/Freehand modes — v1.3
Toolbar mode toggle (default Box). Both capture glyphs via center-in-AABB → References. Non-text:
Box → plain rect, Freehand → translucent marker. `AnnotStroke` gained `width`/`alpha` (serialized).

### ✓ Pen ortho-lock (Shift = straight line) — v1.3
Shift collapses the stroke to a straight segment snapped to the nearest 45°. Shared
`stroke_add_point()`; applies to pen + freehand highlighter.

### ✓ Per-box zoom-scaling text — v1.3
`CanvasTextBox.zoom_scaled` + a "Scale" toggle: font and box scale with zoom while stored values stay
canonical at 100%. Shared `text_box_layout()`; serialized as `zs`.

### ✓ Event-driven render loop — v1.3
Canvas renders on demand (`glfwWaitEventsTimeout` + `glfwPostEmptyEvent`); idle CPU/GPU near zero.

### ✓ Tool-exclusion + light-mode fixes — v1.3
Synchronous `text_box_at`; double-click-to-edit disarms annotation tools; References notes readable
in Light mode.

### ✓ Linux port — v1.1 · Windows port — v1.0
`scholion-lnx/` + `scholion-win/` build trees, CI jobs, zipped release assets.

### ✓ Annotation coordinate fix on rotated pages — v1.1
`screen_to_page_norm()` inverts the rotation transform so annotations operate in PDF-native space.

### ✓ Clean shutdown — v1.1
`rast_cancel_all()` before `rast_shutdown()` so the worker exits immediately on quit.

### ✓ Text-snapping highlights + References tab — M29
Highlight snaps to MuPDF char boxes; captured text feeds the References tab; HTML export.

### ✓ Panel interaction redesign — M27
Single-click selects, double-click opens panel, Space tap toggles; text boxes no longer over the panel.

### ✓ Modifier-click multi-select — M20–M22 · Rubber-band text boxes — M23
Cmd/Ctrl+click toggles items into a unified selection; Shift+click = whole document; rigid group drag.

### ✓ Save / load hardening — M17–M18
Text-box wipe fixed; missing-PDF index shift fixed; startup chooser; placeholders + relink;
record-based CRLF-tolerant parser.

### ✓ Core milestones — M1–M16
Canvas + pan/zoom, PDF rasterization, stacks, annotations, multi-select, save/load, autosave, VRAM
optimization (350 MB budget, 3-tier LOD), text boxes, full-text search, status overlay, UX polish.
