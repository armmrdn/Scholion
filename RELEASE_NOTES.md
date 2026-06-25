### First Release of Scholion PDF viewer.
Updates from previous pre-release: 

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
