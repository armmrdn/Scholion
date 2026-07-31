#pragma once
// Floating top-left toolbar — tool selection (pen/highlight/flag/eraser/text), the active
// tool's property strip (color, size, highlighter box/freehand, text-box style), and the
// Add PDF / folder / URL menu. Reads/writes the shared tool state (canvas_annot.h,
// canvas_text_box.h) and reports its measured bottom edge via g_toolbar_bottom (app_state.h)
// so the sidebar can sit beneath it. Implementation in src/toolbar.cpp.

void draw_toolbar_ui();   // call once per frame
