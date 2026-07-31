#pragma once
// Canvas text boxes — the "sticky note" overlay: render, hit-test, press-drag-release creation,
// in-place editing, and the unified page+box drag reconciliation. Transient interaction state
// (drag snapshots, creation, hover, undo-session snapshots) lives privately in src/text_boxes.cpp;
// the persistent boxes (g_text_boxes) + selection/editing ids live in app_state.h, and the box
// style template in canvas_text_box.h. The global "delete selected items" concern stays in main.cpp
// (reconcile_selection_and_delete) because it owns document removal.
struct CanvasTextBox;

int  text_box_at(float sx, float sy);   // synchronous top-most box under a point; -1 = none
void draw_canvas_text_boxes();          // render + create + edit; call once per frame
void update_item_drag_reconcile();      // continue/finish a unified page+box drag; call per frame

// Called from the main() render-loop seam when a multi-drag begins from a page (not a box):
// snapshots the selected text boxes so they follow the drag. Pages move via the caller's own
// mechanism, so this deliberately leaves the page-drag snapshot empty.
void textboxes_begin_page_initiated_drag();

int  textbox_hovered();   // id of the box under the cursor (-1 = none) — for the double-click guard
void textboxes_reset();   // clear transient drag/creation/hover state (new/loaded project)
