#pragma once
// The sidebar's References tab — every captured highlight quote grouped by document then page,
// with disclosure arrows, click-to-zoom, per-reference research notes, and HTML export. The
// note-edit state (which highlight's note is focused) lives privately in src/references_panel.cpp;
// key_callback and new_project reach it through the hooks below. Called from draw_panel_ui (main.cpp).

void draw_references_tab();

bool references_note_editing();   // true while a per-ref note field is focused (suppress other input)
void references_commit_note();    // write the buffer back to the highlight + close the note (ESC)
void references_reset();          // clear note-edit state on new/loaded project
