#pragma once
// The sidebar panel — the right-hand Viewer (per-page render + in-panel annotation) and its chrome
// (tab bar, page nav, resize handle, and the closed-state edge tabs). The References tab lives in
// references_panel.cpp and is dispatched from here. Implementation in src/side_panel.cpp.

void draw_panel_ui();             // the panel window (Viewer + References tabs); call once per frame
void draw_panel_resize_handle();  // draggable left edge; rendered after the panel so it sits on top
void draw_panel_edge_tabs();      // closed-state re-open tabs; visible only when the panel is closed

// Panel state that main.cpp also touches (arrow-key page nav; canvas-stroke gating). Owned by
// side_panel.cpp.
extern int  g_panel_nav_page;     // current page index in the open panel (0-based)
extern bool g_panel_ann_active;   // true while a stroke is being drawn from inside the panel
