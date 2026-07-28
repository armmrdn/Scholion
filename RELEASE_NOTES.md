<!--
  RELEASE NOTES CONVENTION (read before editing):
  This file presents a COMPLETE, self-contained picture of the product AT this version — not a
  detached delta that only makes sense to someone who tracked the previous version. Use contextual
  language that reads as an update while also explaining what a feature does / the problem it solves.
  Grouped under stable //-prefixed category headers; keyboard keys in bold; //Bug Fixes last.
  Do NOT stack prior versions here. When cutting a new version, truncate and rewrite the full
  snapshot; older releases keep their own notes on their GitHub release pages.
-->

## Scholion v1.4

Scholion is an infinite-canvas workspace for reading, arranging, and annotating many PDFs at once.
This release introduces **page groups** for gathering related pages across documents, makes your
projects portable and hard to lose, lets the pen draw on the canvas itself, and adds a larger-UI
option for smaller screens.

### //Tools

**Pen**
- The pen writes freehand ink wherever you need it. A stroke started on a page stays locked to that
  page and travels with it through pan, zoom, and rotation; a stroke started on the **empty canvas**
  now stays locked to the canvas and always draws in front, so you can sketch arrows and margin notes
  that aren't tied to any single document.
- Hold **Shift** while drawing to snap the stroke to a straight line at the nearest 45°.

**Highlighter**
- Drag over text to lay down a clean, glyph-locked highlight — and the quoted text is captured into
  the References list automatically. Over a figure or blank area you get a plain highlight mark
  instead, with no reference entry.
- Two modes: **Box** (default) drags a rectangle for reliable text capture; **Freehand** swipes like
  a marker and leaves a translucent line over non-text. Freehand marker color is adjustable; text
  highlights stay yellow.

**Text Boxes**
- Place floating notes anywhere by dragging out a box with the text tool (a bare click makes a
  default box). A **fixed** box holds its on-screen size at any zoom — ideal as a label that stays
  legible when you're zoomed all the way out. Flip on **Scale** and the box instead scales with the
  page, so a note pinned beside a figure shrinks with that figure and never becomes an obstruction
  when you pull back.
- Color, font size, and the Scale toggle apply live to the selected box and become the template for
  new ones. Duplicate with **Cmd/Ctrl+C** / **Cmd/Ctrl+V**; delete with **Delete** / **Backspace**.

**Flags & Eraser**
- The Flag tool drops sequential lettered markers (A, B, … Z, 2A…) on a page for footnote-style
  cross-references. The Eraser removes strokes, highlights, and canvas marks by dragging over them.

**Tool Behavior**
- Tool keys are single-press toggles and mutually exclusive: **P** Pen · **H** Highlighter ·
  **F** Flag · **E** Eraser · **T** Text. Pressing one turns the others off.
- **Esc** follows a clear priority ladder: finish a text edit → exit the text tool → cancel an
  in-progress annotation → confirm a reference note → clear the selection → close the panel. Esc
  never quits the app.

### //Canvas

- Pan with the middle mouse or **Space**+drag, zoom on the cursor with the scroll wheel, and
  **Cmd/Ctrl+0** (or a middle-button double-click) zooms to fit everything.
- Select a page with a click; **Shift+click** selects its whole document; **Cmd/Ctrl+click** toggles
  a single page; drag an empty area to rubber-band a selection; **Cmd/Ctrl+A** selects every page.
- Right-click a page to **Return to Stack**, **Fan** pages vertically or horizontally, **Stack** them,
  align a selection, or **Zoom to Fit Document**.
- Page rotation is now a single **Rotate 90°** (clockwise) — repeat it to reach any orientation,
  keeping annotations aligned.
- New **Normalize Size** rescales a document or image that imported far too large or too small so it
  matches the standard page size of the rest of your canvas.

**Page groups**
- Gather pages from one or several documents into a movable cluster — without detaching them from
  their source. Select pages and press **Cmd/Ctrl+G** (or right-click → Group Selected Pages); a
  rounded boundary frames them, drawn in each contributing document's own color, so a group spanning
  two PDFs shows a double line.
- Drag the frame or its label to move the whole group at once, while dragging a page *inside* the
  frame still moves just that page. Drag more pages onto a group and hold briefly and it swallows
  them; double-click the label to rename the group.
- Take a single page out with right-click → **Remove Page from Group** (the rest stay grouped); the
  page briefly flashes its group color as the boundary snaps in around the remaining pages.
- Ungroup with **Cmd/Ctrl+Shift+G** (or right-click → Ungroup) — it's fully nondestructive, so every
  page returns to its original document with its position and annotations untouched. Groups are saved
  with the project.

### //Sidebar Viewer & References

- The sidebar's **Viewer** tab reads the open document page-by-page; the **References** tab collects
  every highlighted quote across all documents into one browsable list, sorted by document and page.
- Each reference shows its source ("filename · p.N") in the document's own color and has a
  **disclosure arrow** — expand it to read the full quoted passage inline, collapse it back to a
  one-line preview. Clicking a reference zooms the canvas straight to that page, and captured quotes
  now **preserve their original line breaks**. Attach a research note to any reference, and
  **Export .html** writes the whole collection to a styled, shareable file.
- The **Open Documents** list is always visible at the bottom of the tab, even before you've captured
  anything. Single-click a document to toggle its connection **threads** — which now render in that
  document's own color when toggled here (they stay maroon while you actively select or drag it);
  double-click to reveal the file in your file manager or relink a moved PDF.

### //Files & Reliability

- **Portable projects.** Move, copy, share, or sync a project folder and its PDF links keep working —
  paths are stored relative to the project file, with the absolute path as a fallback.
- **Crash-recovery backup.** The previous saved state is kept in a `.bak` beside your project and
  removed on a clean quit, so a crash or a bad save is recoverable.
- **Versioning + integrity checking.** Project files now carry a format version and a content
  checksum. A corrupt or truncated file — or one written by a newer version of Scholion — is detected
  on open: Scholion warns you and pauses autosave so it can never silently overwrite your good data.
- **Standing save status.** A corner indicator always tells you whether your work is safe: `Saved 3:42
  PM` at rest, `Editing...` when you have unsaved changes, a brief `Saved!` on every save, and a clear
  warning when autosave has been paused. Autosave still runs every 60 seconds when a project path is
  set.
- Saves are hardened against power loss — data is flushed to disk before the atomic file replace, and
  older `.scholion` files continue to open with all pages, annotations, text boxes, and references
  intact.
- **Password-protected PDFs** are recognized on import and shown as a clearly labelled placeholder page
  instead of a blank or broken one, so it's obvious why an encrypted document can't be displayed.

### //UI

- Full-text search across every open document with **Cmd/Ctrl+F**; **Cmd/Ctrl+S** saves; the
  bottom-left status overlay (**F3**) shows a health dot, the visible/total page count, and zoom level.
- New **Larger UI** toggle (Settings → Appearance) scales all text, buttons, menus, and panels up for
  smaller or high-resolution screens. The normal size is unchanged, and your choice is remembered
  between launches.
- Text throughout now renders in a bundled font with full Unicode — proper ellipses, dashes, and
  symbols (·, °, Greek) — and stays crisp at any scale, including the Larger UI setting.
- Selection borders are thin dotted outlines that stay readable at any zoom; the window title reflects
  the current project.
