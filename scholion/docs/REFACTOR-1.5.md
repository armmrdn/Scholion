# Scholion 1.5 — the "big refactor" plan (draft)

**Thesis:** 1.5 is a **behavior-preserving internal refactor** that pays down the structural debt
called out in the v1.4 backlog (Tier 2), so the *next* feature cycle — and the eventual distribution/
licensing rework — sit on solid ground. **No user-visible behavior should change.** Every step is
gated on a green build + self-test + a manual smoke pass, committed in small increments.

Scope of 1.5 (internal): **stable IDs → real JSON parser → `main.cpp` decomposition.**
Deferred to a licensing-gated follow-up (call it 2.0): **PDF backend swap + distribution rework.**

---

## Guiding principles

1. **Behavior-preserving.** If a user notices anything changed, that's a bug. The refactor is
   invisible by design.
2. **Test-gated, incremental.** Land it in small commits; after each, `Scholion --selftest` is green,
   the app builds on all three trees, and the manual smoke checklist (Phase 0) passes. Never stack two
   risky changes before verifying.
3. **One concern per commit.** Don't mix an ID migration with a file move.
4. **Keep the self-test growing.** Every phase adds coverage before it changes the thing it covers.

---

## Phase 0 — Regression safety net (do this first)  ✅ DONE (2026-07-29)

Delivered: `src/selftest.cpp` now covers headless undo of PageMove / PageRotate / DocScale /
Group / Ungroup / GroupJoin / TextBoxCreate / TextBoxDelete / Note, **plus the DocumentRemove
scrub path** (push a record referencing a removed doc's page → remove_document → undo must not
crash). `undo_last()` and `remove_document()` were de-`static`'d + declared (undo.h / app_state.h)
so the test can drive them (also needed for the Phase 3 extraction). Manual `docs/SMOKE-TEST.md`
checklist written. Self-test PASSED; it runs in CI already, so the new coverage is enforced there.

<details><summary>Original plan</summary>

**Why:** the current self-test covers serialization round-trips, but the riskiest 1.5 work
(IDs, selection, drag, undo) isn't covered. Build the net before swinging the hammer.

- **Extend `src/selftest.cpp`** to exercise the *data-model* operations headlessly (no GL needed —
  they mutate `g_documents`/`g_text_boxes`/`g_groups`/undo directly):
  - Undo/redo of a page move, a page rotate, a doc-scale, a text-box create/edit/delete, a group
    create / ungroup / join / per-page remove — assert state returns exactly.
  - Selection-set operations (add/toggle/clear; whole-doc vs single-page) via `InputHandler`'s public
    API where reachable without a window.
  - Document removal + undo (the raw-`Page*` scrub path — the exact thing Phase 1 replaces).
- **Write `docs/SMOKE-TEST.md`** — a 5-minute manual checklist (open a multi-doc project; draw/erase;
  group across docs, move the frame, add/remove pages; text boxes fixed + scaled; references + notes;
  save/reload; undo everything). Run it after each phase.

**Risk:** none. **Size:** small. **Exit:** self-test covers undo + selection; smoke checklist exists.
</details>

---

## Phase 1 — Stable page/document IDs (the foundation)  ✅ DONE (2026-07-29)

Delivered (sub-steps 1–5): `Page::id` (serialized, migrated for legacy files, carried across
relink); `page_by_id()` resolver + `selected_pages()` helper; **undo records** (`PagePos`/`PageRot`/
`GroupMember`) and **`InputHandler::m_selection`** and the **renderer selection hint** all migrated
from raw `Page*` to stable ids; the defensive `remove_document` undo-scrub **deleted** (stale ids
resolve to nullptr). `s_editing_ref_hl` is validated each frame against live highlights (can't dangle
if a doc is removed mid-note-edit). The only remaining `Page*` are the strictly drag-lifetime snapshots
(`g_multi_drag_snaps` / `g_page_drag_states` / `g_drag_snap_page`), which can't span a reallocation and
are safe by construction. Build clean; self-test PASSED (round-trip incl. ids + all undo cases).
**Needs a manual smoke pass** (selection/drag/undo in the real UI — not headless-testable).

<details><summary>Original plan</summary>

**Why:** the root cause of the whole recurring bug class. Selection, drag origins, hover, undo
records, group membership, and per-frame drag snapshots all store **raw `Page*`** into
`std::vector<Page>` (inside `std::vector<Document>`), which **reallocates on any add/remove** —
forcing defensive scrubs (`remove_document` undo-scrub, `clear_selection` on `set_documents`, the
relink carry-over). An integer handle that's resolved to a pointer *at point of use* eliminates the
dangling-pointer hazard entirely.

**Approach (incremental, additive-first):**
1. Add `uint64_t id` to `Page` (and `Document`), assigned from a monotonic counter at creation
   (`load_pdf`, relink, placeholder build). Persist it in `.scholion` (a page `id` field; the parser
   already tolerates new fields). On load of a pre-1.5 file with no ids, **assign fresh ids** in
   document→page order (deterministic).
2. Add a resolver: an `unordered_map<uint64_t, Page*>` (and doc map) rebuilt whenever the document set
   changes structurally (load, add, remove, relink) — the one place reallocation happens. Helpers:
   `Page* page_by_id(uint64_t)`, `uint64_t id_of(const Page*)`.
3. **Migrate the persistent stores from `Page*` to id**, one at a time, self-testing after each:
   - `InputHandler::m_selection` and drag origins → sets/lists of ids; resolve to `Page*` only inside
     the frame that uses them (no reallocation possible mid-frame).
   - `UndoRecord` `page_moves` / `page_rots` / `group_members` → store ids; resolve in `undo_last`.
     This **deletes the undo-scrub-on-doc-removal hack** (a stale id simply resolves to `nullptr` and
     is skipped — safe by construction).
   - `g_multi_drag_snaps`, `g_drag_snap_page` → ids.
   - Transient-only state (`m_hovered_page`, in-progress `m_dragged_page`) may stay `Page*` (never
     outlives its frame) or move to ids for uniformity — decide during implementation.
4. The reference-note editing pointer (`s_editing_ref_hl`, an `AnnotHighlight*`) is a sub-case: give
   `AnnotHighlight` an id too, or key editing by `(page id, highlight index)`. Small, do it here.

**Risk:** **high** (core interaction). **Mitigations:** additive first (ids alongside pointers), then
migrate one store per commit with the Phase-0 self-test + smoke pass between each; the serialization
round-trip guards the persisted id.
**Files:** `document.h`, `input.h`/`input.cpp`, `undo.h`, `main.cpp` (drag reconciliation, undo,
groups, selection glue), `project_io.cpp` (serialize/parse the id + migration).
**Exit:** no raw `Page*` survives across a structural mutation; the defensive scrubs are gone; all
tests green.
</details>

---

## Phase 2 — Real JSON parser (retire the hand-rolled scanner)  ◑ PARSER DONE (2026-07-29)

Vendored PicoJSON (`include/picojson.h`, BSD-2). `load_project_from_path` now parses via a picojson
tree that fills the same intermediate structures — the scanner (Tok/section_at/while-loop/`sscanf`/
`char s[4096]`) is gone, killing the buffer-overflow class and the substring section-inference. Apply
half untouched; hash/format/heuristic still run on raw bytes. Self-test PASSED (found + fixed a `zs`
int-vs-bool round-trip bug). **Remaining (optional):** rewrite the serializer to emit via picojson too
(the manual emitter already produces valid JSON, so this is cleanup, not correctness); remove the now-
dead `extract_json_text`.

<details><summary>Original plan</summary>

**Why:** `project_io.cpp` parses with a substring-section-inference + per-record `sscanf` scanner —
brittle (the fixed buffer-overflow was a symptom; section detection can be fooled by token-like
substrings in user text) and impossible to evolve. A real parser removes the entire class.

**Approach:**
- Vendor a permissive header-only JSON library (MIT/BSL) into `third_party/`. Rewrite
  `build_project_json()` to emit via the library and `load_project_from_path()` to parse a proper
  tree and read typed fields (documents, pages incl. the Phase-1 id, annotations, text boxes, groups,
  viewport, note counter, header/hash).
- Keep the on-disk **structure** identical so v1.0–v1.4 files still load (they're already JSON); the
  integrity `hash` + `format` header stay. No `format` bump required — but this is the natural moment
  to add a real **migration hook** for future changes.
- The **self-test round-trip is the safety net** — extend the corpus with a couple of real old
  `.scholion` files and confirm byte-for-identical reload of every field.

**Risk:** medium (all persistence flows through here) — but fully covered by the round-trip test.
**Files:** `project_io.cpp`, `third_party/<json>/`, all three `CMakeLists` (new include path).
**Exit:** no `sscanf`/substring parsing remains; old files load; self-test green.
</details>

---

## Phase 3 — Decompose `main.cpp`  ◑ IN PROGRESS (2026-07-30)  — 4,840 → 4,461 lines so far

Carves landed (each: build green + self-test PASSED, one-line add to `cmake/sources.cmake`):
- **Step 1 — CMake dedup:** shared source list factored into `cmake/sources.cmake`; all three
  CMakeLists `include` it + `list(TRANSFORM PREPEND)`. Adding a shared `.cpp` is now a one-line change.
- **Step 2 — `undo.cpp`:** the undo stack + `push_undo` / `undo_last` / `clear_undo_stack`.
- **Step 3 — `groups.cpp` (+ `include/groups.h`, ~405 lines):** the whole page-group subsystem —
  constants, `g_editing_group` / drag-add / remove-flash statics, and every group function
  (`group_pages`, `group_handle_at`, `draw_page_groups`, `create_group_from_selection`,
  `ungroup_group`, `remove_page_from_group`, `update_group_drag_add`, `draw_group_remove_flashes`).
  Exposed two lifecycle hooks — `groups_reset()` (called from `new_project`) and `groups_animating()`
  (called from `app_wants_animation`) — so no group static leaks back into `main.cpp`. Required
  promoting three `main.cpp` statics to `app_state.h` externs: `g_settings_open`, `g_search_open`,
  and `selected_pages()`.

- **Step 4 — `settings.cpp` (+ `include/settings.h`):** the Settings modal (`draw_settings_popup`,
  ~163 lines). Cleanest possible carve — `g_settings`, `g_settings_open`, `save_prefs()`,
  `apply_appearance()` were already header-visible; only `g_logo_tex` needed promoting to an
  `app_state.h` extern (`unsigned int`, the repo's "GLuint = unsigned int" convention).

Running total: **main.cpp 4,840 → 4,298 lines.**

### Remaining plan (ordered; full strategy + coupling map in DEVLOG 2026-07-30 "carve strategy")

> **Line numbers drift after every carve — key on symbols, re-grep at execution time.**
> Two state classes decide where a static goes: *shared style/config read across TUs* → a
> header-visible struct/extern (never into the carved module); *transient interaction state owned
> by one subsystem* → a module-static exposed via accessors + a `reset()` hook.

- **Step 5 — `text_boxes.cpp`** (highest value; tangled → 3 sub-commits):
  - 5a: move the generic `imgui_dashed_rect` helper out to a shared util/renderer TU (de-interleaves
    the region; `draw_locked_doc_labels` stays in main for now).
  - 5b: split the two state classes — text-box **style defaults** (`g_tbox_r/g/b`,
    `g_tbox_font_size`, `g_tbox_zoom_scaled`) are shared with the toolbar → promote to a
    header (alongside the `canvas_annot.h` pen/hl style); text-box **transient** state
    (`g_box_dragging`, `g_box_drag_states`, `g_box_drag_start_world`, `g_tbox_creating`,
    `g_hovered_box`) will move into the module.
  - 5c: move `text_box_layout` / `text_box_at` / `draw_canvas_text_boxes` + transient statics into
    `text_boxes.cpp`. Replace the main-loop drag-init seam (the `if (!g_box_dragging &&
    …selected_text_boxes()…)` block) with an exported `textboxes_begin_selection_drag()` so the loop
    never touches box statics. Expose `textboxes_reset()` (drag + clipboard) for `new_project`.
- **Step 6 — `side_panel.cpp`** (Viewer + References + Open Docs: `draw_panel_ui`,
  `draw_references_tab`, `draw_panel_resize_handle`, `draw_panel_edge_tabs`; ~950 lines). Move the
  references-edit statics (`s_editing_ref_hl`, `s_ref_note_buf`, `s_prev_editing_hl`) in; expose
  `sidepanel_reset()`. Coupling is mostly outbound **action calls** (`relink_document`,
  `reveal_in_file_manager`, `load_pdfs_from_selection`, zoom-to-page/doc, copy-text, export-refs) →
  introduce a small **`actions.h`** declaring them.
- **Step 7 — `toolbar.cpp`** (small — most state already externed in `canvas_annot.h`). Needs
  `actions.h` (Step 6) + text-box style defaults header-visible (Step 5b), then it's a clean move of
  `draw_toolbar_ui` (+ `s_toolbar_bottom`).
- **Step 8 — `dialogs.cpp`** (optional, lowest value): `draw_url_modal`, `draw_startup_chooser`,
  `draw_quit_dialog`. Thin modals over `actions.h`. Defensible to leave in main as app-shell.
- **Step 9 — stop at the shell.** After 5–7, main.cpp = platform/GL/GLFW init + the GLFW callbacks +
  the frame loop + `new_project`/`app_wants_animation`. Target **~1,500–2,000 lines**. Extracting the
  callbacks into `input_glue.cpp` is possible but highest-risk/lowest-value (they touch every
  subsystem) — do it only if there's appetite; otherwise declare Phase 3 done.

<details><summary>Original plan</summary>

**Why:** the single biggest maintainability liability; every feature currently touches one file.
Purely **mechanical** once Phases 1–2 have loosened the coupling.

**Approach:** extract cohesive units into their own translation units, **one per commit**, moving the
functions *and their file-scope statics*, exposing a small header, updating `app_state.h` externs, and
adding the `.cpp` to **all three** CMakeLists (or, better, do the "shared source list → one `.cmake`
include" dedup first so it's a single edit). Candidate seams, roughly in dependency order:
- `undo.cpp` (the undo stack + `undo_last`) · `groups.cpp` · `text_boxes.cpp` ·
  `references_panel.cpp` (References/Viewer/Open-Documents) · `toolbar.cpp` (toolbar + property strip)
  · `dialogs.cpp` (settings, startup chooser, quit/URL dialogs) · `input_glue.cpp` (the GLFW
  callbacks + drag-reconciliation loop). What remains in `main.cpp` is init + the frame loop.

**Risk:** low–medium (link/visibility churn), but mechanical and individually verifiable.
**Exit:** `main.cpp` is init + loop; each subsystem is its own TU; builds on all three trees.
</details>

---

## Phase 4 — DEFERRED / conditional (licensing-gated; likely a separate 2.0)

Not part of 1.5. Blocked on the distribution/licensing decision (v1.4 TODO Tier 1a):
- **PDF backend swap** — MuPDF (AGPL) → pdfium (BSD) or a commercial MuPDF license, *only if going
  closed-source*. Big port across `pdf_loader.cpp` (raster, text extraction, char quads, search).
- **Distribution rework** — notarize (macOS) + sign (Windows), installer (`.scholion` association),
  auto-update. Pairs naturally with the backend decision.

---

## Sequencing & rationale

```
Phase 0 (net)  →  Phase 1 (IDs)  →  Phase 2 (JSON)  →  Phase 3 (carve main.cpp)
                                                                └─ then, gated on licensing: Phase 4
```
- **Net first** so the risky work is caught, not discovered by users.
- **IDs before JSON** so the parser rewrite handles ids from the start (one rewrite, not two) and
  serialization is clean.
- **JSON before/with carve** since `project_io` shrinks and simplifies, making the split easier.
- **Carve last** — it's the most mechanical and lowest-risk, best done once coupling is reduced.

**Is 1.5 too big?** Possibly. Natural split points if we want smaller releases:
**1.5 = Phase 0 + 1 + 2** (the correctness-critical core), **1.6 = Phase 3** (the carve). Either way,
Phase 4 waits on the licensing call.

**Ship consideration:** a pure-refactor release gives users no visible benefit but carries regression
risk. Options: (a) ship it quietly as a stability release, or (b) fold in one or two small, low-risk
user wins (e.g. persist `doc.hue`, an icon-DPI refresh) so 1.5 has a headline. Decide before cutting.

## Open decisions (for you)
1. **1.5 = Phases 0–3, or split** (0–2 now, carve in 1.6)?
2. **JSON library** preference (I'll recommend one when we start; permissive + header-only + small).
3. **Any user-facing wins** to fold into 1.5, or ship as a pure stability release?
4. **Licensing (Tier 1a)** — not needed to start 1.5, but it unblocks Phase 4; worth deciding in
   parallel.
