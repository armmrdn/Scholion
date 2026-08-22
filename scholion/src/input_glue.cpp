#include "input_glue.h"
#include "app_state.h"
#include "canvas_annot.h"
#include "groups.h"
#include "text_boxes.h"
#include "references_panel.h"
#include "side_panel.h"
#include "actions.h"
#include "project_io.h"
#include "rast_pipeline.h"
#include "undo.h"
#include "dialogs.h"
#include "imgui.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

// ===== Module state (g_clip_valid is extern in input_glue.h; new_project clears it) =====
static Page*         g_nav_focus  = nullptr; // focused page for arrow navigation (panel closed)
static CanvasTextBox g_clip_box   = {};      // text-box clipboard (Cmd+C/V)
bool                 g_clip_valid = false;

void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static int g_doc_z_counter = 0;


void mouse_button_callback(GLFWwindow* w, int button, int action, int mods) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    // A text box under the cursor owns this click, so no annotation (pen/highlight/note)
    // starts on the page beneath it. Synchronous hit-test replaces the old g_hovered_box
    // guard, which was set one frame late in draw_canvas_text_boxes — a same-frame
    // move-then-click could slip past it (e.g. dropping a stray Note flag under a box).
    {
        double hcx, hcy; glfwGetCursorPos(w, &hcx, &hcy);
        if (text_box_at((float)hcx, (float)hcy) >= 0) return;
    }

    // Annotation tool: intercept LMB press on a page before InputHandler sees it.
    // The page owns the click — InputHandler does not receive it, so no drag/select starts.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS
            && g_annot_tool != AnnotTool::None) {
        double cx, cy;
        glfwGetCursorPos(w, &cx, &cy);
        int doc_idx = -1;
        Page* page = hit_test_page((float)cx, (float)cy, &doc_idx);
        if (page) {
            Vec2 norm = screen_to_page_norm(*page, (float)cx, (float)cy);
            if (g_annot_tool == AnnotTool::Note) {
                int snap = g_next_note_idx;
                page->annots.notes.push_back({note_label(g_next_note_idx++)});
                UndoRecord r;
                r.type            = UndoRecord::Type::Note;
                r.doc_idx         = doc_idx;
                r.page_idx        = page->page_index;
                r.note_idx_before = snap;
                push_undo(r);
            } else {
                g_ann_drawing  = true;
                g_ann_doc_idx  = doc_idx;
                g_ann_page_idx = page->page_index;
                // Pen and the freehand highlighter capture a point path; the box highlighter
                // (and eraser) just track the start/current corner via g_ann_hl_start/cur_norm.
                if (g_annot_tool == AnnotTool::Pen ||
                    (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode)) {
                    g_ann_cur_stroke = {};
                    if (g_annot_tool == AnnotTool::Pen) {
                        g_ann_cur_stroke.r = g_pen_r; g_ann_cur_stroke.g = g_pen_g; g_ann_cur_stroke.b = g_pen_b;
                    } else {
                        g_ann_cur_stroke.r = g_hl_r; g_ann_cur_stroke.g = g_hl_g; g_ann_cur_stroke.b = g_hl_b;
                        g_ann_cur_stroke.width = MARKER_HALF_W; g_ann_cur_stroke.alpha = MARKER_ALPHA;
                    }
                    g_ann_cur_stroke.pts.push_back(norm);
                }
                g_ann_hl_start = norm;   // box highlighter / eraser radius origin
                g_ann_cur_norm = norm;
            }
            return; // page owns this click — don't pass to InputHandler
        } else if (g_annot_tool == AnnotTool::Pen) {
            // Pen started on empty canvas → a world-locked canvas stroke (drawn in front).
            g_ann_drawing = true; g_ann_canvas = true;
            g_ann_doc_idx = -1; g_ann_page_idx = -1;
            g_ann_cur_stroke = {};
            g_ann_cur_stroke.r = g_pen_r; g_ann_cur_stroke.g = g_pen_g; g_ann_cur_stroke.b = g_pen_b;
            g_ann_cur_stroke.pts.push_back(g_canvas.screen_to_world({(float)cx, (float)cy}));
            return;
        }
    }

    bool was_box_sel = g_input.box_selecting();
    bool tools_active = g_text_tool || g_annot_tool != AnnotTool::None;

    // Group frame handle: a left-press on a group's frame outline or label grabs the
    // whole group and moves it as a unit. Opt-in and gated on no tool active / not
    // panning, so dragging a page interior or a document (Shift+drag) is unchanged.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS
            && !tools_active && !g_input.is_space_held()) {
        double gcx, gcy; glfwGetCursorPos(w, &gcx, &gcy);
        int gid = group_handle_at((float)gcx, (float)gcy);
        if (gid != 0) {
            std::vector<Page*> pages = group_pages(gid);
            if (!pages.empty()) {
                g_input.begin_group_drag(pages, {(float)gcx, (float)gcy});
                return;   // group handle owns this press
            }
        }
    }

    g_input.on_mouse_button(w, button, action, mods, tools_active);

    // Bring clicked page's document to front
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        const Page* pressed = g_input.pending_drag_page();
        if (pressed) {
            for (auto& doc : g_documents) {
                for (const auto& page : doc.pages) {
                    if (&page == pressed) {
                        doc.z_layer = ++g_doc_z_counter;
                        goto done_z;
                    }
                }
            }
            done_z:;
        }
    }

    // After rubber-band ends, also add text boxes whose world rect is inside the selection rect.
    // box_start/cur_world remain valid after finalize_box_selection clears m_box_selecting.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE
            && was_box_sel && !g_input.box_selecting()) {
        float x0 = std::min(g_input.box_start_world().x, g_input.box_cur_world().x);
        float y0 = std::min(g_input.box_start_world().y, g_input.box_cur_world().y);
        float x1 = std::max(g_input.box_start_world().x, g_input.box_cur_world().x);
        float y1 = std::max(g_input.box_start_world().y, g_input.box_cur_world().y);
        float zoom = g_canvas.get_zoom();
        auto& sel = const_cast<std::unordered_set<int>&>(g_input.selected_text_boxes());
        for (const auto& box : g_text_boxes) {
            float bx0 = box.world_pos.x,  by0 = box.world_pos.y;
            // box.w/h are screen px; world extent = screen px / zoom.
            // h==0 means auto-height (unknown without rendering) — check top-left only.
            float bx1 = (box.w > 0.0f) ? bx0 + box.w / zoom : bx0;
            float by1 = (box.h > 0.0f) ? by0 + box.h / zoom : by0;
            if (bx0 >= x0 && by0 >= y0 && bx1 <= x1 && by1 <= y1)
                sel.insert(box.id);
        }
    }

    // Update g_nav_focus when selection becomes a single page
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        const auto& sel = g_input.selection();
        if (sel.size() == 1 && !g_input.panel_open()) {
            g_nav_focus = page_by_id(*sel.begin());
        }
    }
}

void cursor_pos_callback(GLFWwindow* w, double x, double y) {
    if (ImGui::GetIO().WantCaptureMouse) { g_input.clear_hover(); return; }
    g_input.on_cursor_move(w, x, y);
    // Canvas (world-space) pen stroke — accumulate world points.
    if (g_ann_drawing && g_ann_canvas && g_annot_tool == AnnotTool::Pen) {
        bool ortho = (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS) ||
                     (glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        stroke_add_point_world(g_canvas.screen_to_world({(float)x, (float)y}), ortho);
    }
    // Accumulate pen / freehand-highlighter-swipe points at mouse-move rate (smoother path).
    // Box-mode highlight tracks a rectangle instead (via g_ann_cur_norm), so it's excluded.
    if (g_ann_drawing && !g_panel_ann_active
            && (g_annot_tool == AnnotTool::Pen ||
                (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode))
            && g_ann_doc_idx >= 0 && g_ann_doc_idx < (int)g_documents.size()
            && g_ann_page_idx >= 0) {
        bool ortho = (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS) ||
                     (glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        const Document& doc = g_documents[g_ann_doc_idx];
        for (const auto& pg : doc.pages) {
            if (pg.page_index == g_ann_page_idx) {
                stroke_add_point(pg, screen_to_page_norm(pg, (float)x, (float)y), ortho);
                break;
            }
        }
    }
}

void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    g_input.on_scroll(w, xoff, yoff);
}

void key_callback(GLFWwindow* w, int key, int scancode, int action, int mods) {
    bool super = (mods & GLFW_MOD_SUPER) != 0;
    bool ctrl  = (mods & GLFW_MOD_CONTROL) != 0;

    // These shortcuts fire on PRESS only and are allowed even when ImGui has focus.
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_F && (super || ctrl)) { g_search_open = !g_search_open; return; }
        // Cmd+S: allowed mid text-box edit so you can save without closing the editor.
        if (key == GLFW_KEY_S && (super || ctrl)) { save_project_current(); return; }
        // Cmd+0 / Ctrl+0: zoom-to-fit regardless of active tool or text editing state.
        if (key == GLFW_KEY_0 && (super || ctrl)) { zoom_to_fit(); return; }

        // Tool keys: suppressed while any ImGui widget has keyboard focus (text input, etc.)
        bool cmd = super || ctrl;
        if (!cmd && !g_search_open && g_editing_box < 0
                && !ImGui::GetIO().WantCaptureKeyboard) {
            if (key == GLFW_KEY_P) {
                bool was_pen = (g_annot_tool == AnnotTool::Pen);
                g_annot_tool = was_pen ? AnnotTool::None : AnnotTool::Pen;
                g_ann_drawing = false;
                if (!was_pen) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_H) {
                bool was_hl = (g_annot_tool == AnnotTool::Highlight);
                g_annot_tool = was_hl ? AnnotTool::None : AnnotTool::Highlight;
                g_ann_drawing = false;
                if (!was_hl) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_F && !super && !ctrl) {
                bool was_note = (g_annot_tool == AnnotTool::Note);
                g_annot_tool = was_note ? AnnotTool::None : AnnotTool::Note;
                g_ann_drawing = false;
                if (!was_note) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_E) {
                bool was_eraser = (g_annot_tool == AnnotTool::Eraser);
                g_annot_tool = was_eraser ? AnnotTool::None : AnnotTool::Eraser;
                g_ann_drawing = false;
                if (!was_eraser) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_T) {
                bool was_text = g_text_tool;
                g_text_tool   = !g_text_tool;
                g_editing_box = -1;
                if (!was_text) { g_annot_tool = AnnotTool::None; g_ann_drawing = false; }
                return;
            }
            // ESC: deactivate active tool even when panel has keyboard focus.
            if (key == GLFW_KEY_ESCAPE && !g_search_open) {
                if (g_editing_box >= 0) {
                    g_editing_box = -1;
                    return;
                }
                if (g_text_tool) { g_text_tool = false; return; }
                if (g_annot_tool != AnnotTool::None) {
                    g_annot_tool = AnnotTool::None;
                    g_ann_drawing = false;
                    return;
                }
            }
        }
    }

    // While a ref-note is being edited in the panel, suppress all shortcuts and
    // canvas input — only Escape is allowed (to confirm and close the note).
    if (references_note_editing()) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
            references_commit_note();
        return;
    }

    // Space key must reach on_key even when the panel has keyboard focus so that
    // the press→release tap sequence that toggles the panel is always detected.
    // WantCaptureKeyboard is true whenever any ImGui window is active — but we
    // must NOT route space to on_key when the user is actually typing (text box,
    // search field, ref-note editor). WantTextInput is specifically true only when
    // a text input widget has keyboard focus and wants character input.
    if (ImGui::GetIO().WantCaptureKeyboard) {
        if (key == GLFW_KEY_SPACE && g_editing_box < 0 && !ImGui::GetIO().WantTextInput)
            g_input.on_key(w, key, scancode, action, mods);
        return;
    }

    // InputHandler needs PRESS, REPEAT, and RELEASE so that m_space_held is cleared
    // on key-up. The early "action != GLFW_PRESS" return that used to sit above this
    // call was silently swallowing the space key-up, leaving m_space_held = true
    // forever and causing all subsequent left-drags to pan instead of move items.
    g_input.on_key(w, key, scancode, action, mods);

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

    if (key == GLFW_KEY_Z && (super || ctrl)) undo_last();

    // Arrow key navigation in panel or selection
    if (g_input.panel_open()) {
        int doc_idx = g_input.panel_doc_index();
        if (doc_idx >= 0 && doc_idx < (int)g_documents.size()) {
            int total = (int)g_documents[doc_idx].pages.size();
            if (key == GLFW_KEY_DOWN || key == GLFW_KEY_RIGHT) {
                g_panel_nav_page = std::min(g_panel_nav_page + 1, total - 1);
                g_input.open_panel(doc_idx, g_panel_nav_page);
            } else if (key == GLFW_KEY_UP || key == GLFW_KEY_LEFT) {
                g_panel_nav_page = std::max(g_panel_nav_page - 1, 0);
                g_input.open_panel(doc_idx, g_panel_nav_page);
            }
        }
    } else if (!g_input.selection().empty()) {
        // Arrow key selection navigation (when panel is closed)
        if (key == GLFW_KEY_DOWN || key == GLFW_KEY_RIGHT) {
            Page* next = next_page_in_order(g_nav_focus, +1);
            if (next) {
                g_input.clear_selection();
                const_cast<std::unordered_set<uint64_t>&>(g_input.selection()).insert(next->id);
                g_nav_focus = next;
            }
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_LEFT) {
            Page* next = next_page_in_order(g_nav_focus, -1);
            if (next) {
                g_input.clear_selection();
                const_cast<std::unordered_set<uint64_t>&>(g_input.selection()).insert(next->id);
                g_nav_focus = next;
            }
        }
    }

    if (action != GLFW_PRESS) return;

    bool cmd = super || ctrl;

    // Text-box entity copy/paste. Only reachable when no ImGui text field has
    // focus (guarded above), so copy/paste inside an open box goes to the text
    // field via ImGui instead. Here it duplicates the selected box.
    if (cmd && key == GLFW_KEY_C && g_selected_box >= 0 && g_editing_box < 0) {
        for (const auto& b : g_text_boxes)
            if (b.id == g_selected_box) { g_clip_box = b; g_clip_valid = true; break; }
    }
    if (cmd && key == GLFW_KEY_V && g_clip_valid) {
        CanvasTextBox nb = g_clip_box;
        nb.id        = g_next_box_id++;
        nb.world_pos = nb.world_pos + Vec2{16.0f, 16.0f};  // offset so the copy is visible
        g_text_boxes.push_back(nb);
        g_selected_box = nb.id;
        g_editing_box  = -1;
        UndoRecord r; r.type = UndoRecord::Type::TextBoxCreate; r.box_id = nb.id; push_undo(r);
    }

    // Cmd/Ctrl+G groups the selected pages; Cmd/Ctrl+Shift+G ungroups them.
    if (cmd && key == GLFW_KEY_G) {
        bool shift = (mods & GLFW_MOD_SHIFT) != 0;
        if (shift) {
            std::unordered_set<int> gids;
            for (Page* p : selected_pages()) if (p->group_id) gids.insert(p->group_id);
            for (int gid : gids) ungroup_group(gid);
        } else {
            create_group_from_selection();
        }
    }
}

void focus_callback(GLFWwindow* /*w*/, int focused) {
    // When the window loses focus, key-up events for held keys are never delivered.
    // Clear panning state so space+drag doesn't stay active after a cmd-tab.
    if (!focused) g_input.clear_held_keys();
}

void drop_callback(GLFWwindow* /*w*/, int count, const char** paths) {
    namespace fs = std::filesystem;
    for (int i = 0; i < count; ++i) {
        fs::path p(paths[i]);
        if (fs::is_directory(p))
            load_pdfs_from_folder(paths[i]);
        else if (p.extension() == ".scholion")
            load_project_from_path(paths[i]);
        else
            load_pdf(paths[i]);
    }
}
