# Scholion — manual smoke test

A ~5-minute hands-on pass to run **after each refactor step** (alongside `Scholion --selftest`).
The self-test covers the data model + serialization headlessly; this checklist covers the
interaction/rendering layer it can't reach. Nothing here should change during the 1.5 refactor —
if any step behaves differently than before, that's a regression.

> Tip: keep a small 2–3 PDF sample project handy (varied page sizes, some text, ideally one
> password-protected PDF and one you can rename/move to force a relink).

## 0. Launch
- [ ] Bare launch shows the **startup chooser** (logo, title, prompt, four buttons) — all centered.
- [ ] "Add PDF File(s)" / "Add Folder" / "Open Project" each work; "Blank Canvas" opens empty.

## 1. Navigate & select
- [ ] Scroll zooms on the cursor; middle-drag and **Space+drag** pan; **Cmd/Ctrl+0** fits all.
- [ ] Click selects a page (dotted border + threads); **Shift+click** selects the whole document;
      **Cmd/Ctrl+click** toggles one page; rubber-band from empty canvas selects; **Cmd/Ctrl+A** all.
- [ ] Drag a selected page (or group of selected pages) — moves rigidly. **Esc** clears selection.

## 2. Tools (each key toggles; only one active at a time)
- [ ] **P** pen on a page (stays glued through pan/zoom) and on empty canvas (world-locked, in front);
      **Shift** while drawing snaps to a straight 45° line.
- [ ] **H** highlighter Box over text → clean highlight + it appears in **References**; Freehand over a
      figure → translucent marker (no reference).
- [ ] **F** drops a lettered flag; **E** erases strokes/highlights/canvas marks.
- [ ] **T** drag creates a text box; a **fixed** box holds size at any zoom; a **Scale** box grows with
      the page. Double-click edits; **Cmd/Ctrl+C/V** duplicates; **Delete** removes.
- [ ] With any tool active, dragging the canvas does **not** move pages/boxes underneath.

## 3. Grouping (the v1.4 feature — exercise it fully)
- [ ] Select pages from **two** documents → **Cmd/Ctrl+G** → colored boundary appears (double line, one
      hue per document); label tab clears the rounded corner.
- [ ] Drag the **frame/label** → whole group moves. Drag a page **inside** the frame → only that page.
- [ ] Drag another page onto the group and **hold ~0.8s** → it flashes and swallows the page (new ring).
- [ ] **Double-click the label** → rename (Enter/click-away/Esc all behave).
- [ ] Right-click a member → **Remove Page from Group** → page flashes its color, boundary snaps in.
- [ ] **Cmd/Ctrl+Shift+G** (or right-click → Ungroup) dissolves it; pages return to their document.

## 4. Sidebar & references
- [ ] Double-click a page opens the panel; **Viewer** reads pages; **References** lists captured quotes.
- [ ] Disclosure arrow expands the full quote; clicking a reference **zooms to that page**.
- [ ] Attach a note (placeholder → type → click away); the **X** deletes it reliably; re-edit works.
- [ ] Notes stay attached to the **correct** quote after adding/removing other highlights.
- [ ] **Export .html** writes a readable file. Open Documents list: single-click toggles colored
      threads; double-click reveals in Finder / relinks a moved PDF.

## 5. Arrange & rotate
- [ ] Right-click a page: **Rotate 90°** (annotations stay aligned), **Normalize Size**, Fan V/H, Stack,
      Return to Stack, Zoom to Fit Document, Copy Page Text.

## 6. Persistence & undo (the critical one for the refactor)
- [ ] **Cmd/Ctrl+S** saves; the corner status shows `Saved <time>` / `Editing…` / a brief `Saved!`.
- [ ] Reload the project → pages, annotations, text boxes (incl. Scale flag), references + notes,
      groups (incl. colors/names), and the viewport all restore intact.
- [ ] Move/copy the whole project folder → PDF links still resolve; move only the `.scholion` → falls
      back; rename a PDF → grey placeholder + **Relink** restores it (grouping survives the relink).
- [ ] **Cmd/Ctrl+Z** repeatedly unwinds everything: annotations, moves, rotate, normalize, text boxes,
      group create/join/remove/ungroup — with no crash and correct state at each step.

## 7. UI & edge cases
- [ ] Settings → **Larger UI** scales all chrome up and persists; light/dark both readable.
- [ ] Text renders real Unicode (ellipsis …, dashes, ·, °) — no "?" boxes.
- [ ] A **password-protected PDF** imports as a labelled indigo placeholder (context menu shows it).
- [ ] Right-click menus, tooltips, and dialogs draw **on top of** text boxes.

---

**Pass criteria:** every box behaves exactly as it did before the change under test. Log anything that
differs — that's the regression the refactor introduced.
