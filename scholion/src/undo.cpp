// Undo stack + apply logic. Extracted from main.cpp (1.5 Phase 3). Undo records reference
// pages by stable id, resolved to a live Page* via page_by_id() at apply time, so records
// survive document/page vector reallocation (a removed page resolves to nullptr).
#include "app_state.h"
#include "undo.h"
#include "project_io.h"    // g_dirty
#include "canvas_annot.h"  // g_next_note_idx

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

static std::vector<UndoRecord> g_undo_stack;
static constexpr int           UNDO_LIMIT = 60;

// Clear the undo history (called by new_project).
void clear_undo_stack() { g_undo_stack.clear(); }

void push_undo(UndoRecord r) {
    g_dirty = true;   // any undoable edit marks the project modified since last save
    g_undo_stack.push_back(std::move(r));
    if ((int)g_undo_stack.size() > UNDO_LIMIT)
        g_undo_stack.erase(g_undo_stack.begin());
}

void undo_last() {
    if (g_undo_stack.empty()) return;
    g_dirty = true;   // undoing is itself a change to the working state
    UndoRecord r = std::move(g_undo_stack.back());
    g_undo_stack.pop_back();

    auto safe_page = [&]() -> Page* {
        if (r.doc_idx  < 0 || r.doc_idx  >= (int)g_documents.size()) return nullptr;
        auto& doc = g_documents[r.doc_idx];
        if (r.page_idx < 0 || r.page_idx >= (int)doc.pages.size())   return nullptr;
        return &doc.pages[r.page_idx];
    };

    switch (r.type) {
        case UndoRecord::Type::PenStroke:
            if (auto* p = safe_page(); p && !p->annots.strokes.empty())
                p->annots.strokes.pop_back();
            break;
        case UndoRecord::Type::Highlight:
            if (auto* p = safe_page(); p && !p->annots.highlights.empty())
                p->annots.highlights.pop_back();
            break;
        case UndoRecord::Type::Note:
            if (auto* p = safe_page(); p && !p->annots.notes.empty()) {
                p->annots.notes.pop_back();
                g_next_note_idx = r.note_idx_before;
            }
            break;
        case UndoRecord::Type::PageMove:
            for (auto& pm : r.page_moves)
                if (Page* p = page_by_id(pm.page_id)) p->world_pos = pm.old_pos;
            break;
        case UndoRecord::Type::PageResize:
            for (auto& pm : r.page_moves)
                if (Page* p = page_by_id(pm.page_id)) { p->world_w = pm.old_w; p->world_h = pm.old_h; }
            break;
        case UndoRecord::Type::DocScale:
            for (auto& pm : r.page_moves)
                if (Page* p = page_by_id(pm.page_id)) { p->world_pos = pm.old_pos;
                                                        p->world_w = pm.old_w; p->world_h = pm.old_h; }
            break;
        case UndoRecord::Type::PageRotate:
            for (auto& pr : r.page_rots)
                if (Page* p = page_by_id(pr.page_id)) {
                    p->rotation = pr.old_rot;
                    p->world_w  = pr.old_w;
                    p->world_h  = pr.old_h;
                }
            break;
        case UndoRecord::Type::TextBoxCreate:
            g_text_boxes.erase(
                std::remove_if(g_text_boxes.begin(), g_text_boxes.end(),
                               [&](const CanvasTextBox& b){ return b.id == r.box_id; }),
                g_text_boxes.end());
            if (g_selected_box == r.box_id) g_selected_box = -1;
            if (g_editing_box  == r.box_id) g_editing_box  = -1;
            break;
        case UndoRecord::Type::TextBoxMove:
            for (auto& b : g_text_boxes)
                if (b.id == r.box_id) { b.world_pos = r.old_box_pos; break; }
            break;
        case UndoRecord::Type::TextBoxDelete:
            g_text_boxes.push_back(r.deleted_box);
            break;
        case UndoRecord::Type::TextBoxEdit:
            for (auto& b : g_text_boxes)
                if (b.id == r.box_id) {
                    strncpy(b.text, r.prev_text.c_str(), sizeof(b.text) - 1);
                    b.text[sizeof(b.text) - 1] = '\0';
                    break;
                }
            break;
        case UndoRecord::Type::TextBoxStyle:
            for (auto& b : g_text_boxes)
                if (b.id == r.box_id) {
                    b.r = r.old_r; b.g = r.old_g; b.b = r.old_b; b.font_size = r.old_fs;
                    break;
                }
            break;
        case UndoRecord::Type::ErasedStroke:
            if (auto* p = safe_page())
                p->annots.strokes.push_back(r.erased_stroke);
            break;
        case UndoRecord::Type::ErasedHighlight:
            if (auto* p = safe_page())
                p->annots.highlights.push_back(r.erased_highlight);
            break;
        case UndoRecord::Type::ErasedNote:
            if (auto* p = safe_page()) {
                int at = std::clamp(r.erased_note_at, 0, (int)p->annots.notes.size());
                p->annots.notes.insert(p->annots.notes.begin() + at, r.erased_note);
            }
            break;
        case UndoRecord::Type::DocumentRemove: {
            int idx = std::clamp(r.removed_doc_idx, 0, (int)g_documents.size());
            g_documents.insert(g_documents.begin() + idx, r.removed_doc);
            g_loaders.insert(g_loaders.begin() + idx, r.removed_loader);
            // The GPU textures were deleted when the doc was removed; zero the
            // stale handles so stream_lod() re-rasterizes on the next frame.
            for (auto& page : g_documents[idx].pages) {
                page.tex_thumb = 0;
                page.tex_low   = 0;
                page.tex_high  = 0;
            }
            g_input.set_documents(g_documents.empty() ? nullptr : &g_documents);
            break;
        }
        case UndoRecord::Type::CanvasStroke:
            if (!g_canvas_strokes.empty()) g_canvas_strokes.pop_back();
            break;
        case UndoRecord::Type::ErasedCanvasStroke: {
            int at = std::clamp(r.canvas_idx, 0, (int)g_canvas_strokes.size());
            g_canvas_strokes.insert(g_canvas_strokes.begin() + at, r.erased_stroke);
            break;
        }
        case UndoRecord::Type::Group:
            // Undo a group creation: revert members' group_id, then drop the group row.
            for (auto& gm : r.group_members) if (Page* p = page_by_id(gm.page_id)) p->group_id = gm.old_group;
            g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(),
                           [&](const PageGroup& g){ return g.id == r.group_row.id; }),
                           g_groups.end());
            break;
        case UndoRecord::Type::Ungroup:
            // Undo an ungroup: restore members' group_id and re-add the group row.
            for (auto& gm : r.group_members) if (Page* p = page_by_id(gm.page_id)) p->group_id = gm.old_group;
            if (r.group_row.id != 0) g_groups.push_back(r.group_row);
            break;
        case UndoRecord::Type::GroupJoin:
            // Undo a drag-to-add: revert members to their prior group (no row change —
            // the target group already existed and keeps its original members).
            for (auto& gm : r.group_members) if (Page* p = page_by_id(gm.page_id)) p->group_id = gm.old_group;
            break;
    }
}
