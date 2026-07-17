## Scholion v1.3

### //Highlighter
- The highlighter is now a single unified tool driven by a freehand swipe. Swipe across text and it locks onto the underlying glyphs, capturing the text into the References tab as before. Swipe across a figure, chart, or any non-text region and it leaves a persistent translucent marker so non-text content can be highlighted too.
- Highlighter color picker added to the tool property strip (applies to freehand marks; text highlights stay yellow).

### //Text Boxes
- Text boxes can now scale with zoom. A per-box "Scale" toggle in the text tool's property strip makes a box's text stay proportional to the page as you zoom, for margin notes that belong to the document. Existing boxes keep their fixed on-screen "sticky note" size by default.

### //Page Rotation
- Added "Rotate 180°" and "Reset Rotation" to the page right-click menu, alongside the existing 90° rotate options.

### //Performance
- The canvas now renders on demand instead of continuously, dropping idle CPU and battery use to near zero during long reading sessions (macOS/Linux). The window still responds instantly to input.

### //Bug Fixes
- A page annotation (pen/highlight/note flag) can no longer be accidentally placed on the page beneath a text box when clicking that box.
- Double-clicking a text box to edit it now cleanly puts away any active annotation tool.
- Reference notes are now readable in Light mode — the note editor and note text no longer render dark-on-dark or wash out.

---

## Scholion v1.2

### //Tools
- Tools are now mutually exclusive — activating any tool (pen, highlight, eraser, text) automatically deactivates the previous one. Switching via toolbar button or keyboard shortcut no longer leaves two tools active simultaneously.
- ESC correctly exits all tool modes regardless of UI focus state. Priority: finish text edit → exit text tool → clear selection → cancel annotation tool.
- E key activates the eraser tool (was unimplemented).
- T key is now properly suppressed while typing in any text field.
- Pen tool color picker added to the toolbar property strip (right of tool buttons).
- Text color and font size controls moved to the same unified property strip, with a visual separator between tool buttons and their properties.

### //Bug Fixes
- Canvas text boxes no longer disappear while the color picker popup is open.
- Reference panel notes now activate on a single click (previously required two clicks to focus the window first).
- Reference panel notes auto-focus the text cursor immediately on open.
- Clicking outside a reference note box saves and closes the note without a separate confirmation step.
- Reference note text wraps correctly to the panel width and no longer bleeds past the right edge.

### //UI
- Version number now appears in the window title bar, on the startup screen, and in the bottom corner of the Settings window.

---

## Scholion v1.1

### //Linux
-Scholion is now available as a native Linux build (x86_64).
-Distributed as Scholion-Linux.zip; requires libcurl4 and libGL at runtime.
-File open/save dialogs use the system file manager (zenity/kdialog).
-Reveal in File Manager opens the containing folder via xdg-open.

### //Bug Fixes
-Fixed pen strokes, highlights, and eraser becoming misaligned on rotated pages. Annotations now stick exactly where placed regardless of page rotation and can be erased from the correct position.
-Fixed the highlighter failing to collect text glyphs on rotated pages, restoring the ability for highlighted text to appear in the References sidebar tab.
-Fixed a delay on quit when many pages were queued for background rasterization; the app now exits immediately after the final save.

---

## Scholion v1.0

### //Canvas Viewer Behavior
-All tools (Text box, pen, highlight, flags, search, rotate) all work in the canvas space.
-Individual pages can now be rotated in 90 degree increments in the canvas space and annotations persist rotations.
-Search (ctrl+F) now renders orange highlights on hits inside of canvas space as well as sidebar view if its open.
-Pages render in tiles to relieve GPU strain on more limited systems, only noticeable when zoomed into high resolution pages.
-Fixed an issue where canvas-pan or page-drag gets stuck to cursor if mouse released outside of app window.

### //Sidebar Viewer Behavior
-Sidebar viewer pops out less often; only when a page is double-clicked, or selected and spacebar is tapped.
-Sidebar viewer's document and references view can be accessed via tabs on the right edge of app window.
-Sidebar viewer resizes more reliably via dragging the edge.
-References panel in the sidebar viewer has note-taking attached to each reference that is collected by the highlighter tool.
-All references can be exported as editable HTML for integrating or pasting into a word processor document.
-Bottom of reference list includes list of all open documents -their threads can be toggled by clicking on them.
-Items in the list of open documents that are moved or deleted will be shown in red to indicate it needs to be re-liked.

### //UI Polish
-Revised text, labels and tooltips across all UI elements
-Keyboard shortcuts are suppressed when typing in any text-field that is accepting keyboard input.
