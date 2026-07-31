#include "text_boxes.h"
#include "app_state.h"
#include "canvas_text_box.h"
#include "canvas_annot.h"
#include "undo.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>

// ===== Transient module state (see DEVLOG 2026-07-30; persistent boxes live in app_state.h) =====

static int  g_hovered_box    = -1;  // id under cursor (set per-frame); -1 = none
static bool g_box_dragging    = false;
struct TextBoxDragState { int id; Vec2 initial_pos; };
static std::vector<TextBoxDragState> g_box_drag_states = {};
struct PageDragState { Page* page; Vec2 initial_pos; };
static std::vector<PageDragState>    g_page_drag_states = {};
static Vec2 g_box_drag_start_world = {};  // world position where drag began (grab point)
// Press-drag-release creation of a new text box (text tool active)
static bool g_tbox_creating     = false;
static Vec2 g_tbox_create_start = {};   // screen pos where the drag began
static char  g_edit_text0[2048]  = "";
static float g_style_r0 = 0, g_style_g0 = 0, g_style_b0 = 0, g_style_fs0 = 0;
// --- Canvas text boxes -------------------------------------------------------

static constexpr float TBOX_W        = 280.0f;  // editing window width (fixed fallback)
static constexpr float TBOX_MIN_W    = 80.0f;
static constexpr float TBOX_MAX_W    = 420.0f;
static constexpr float TBOX_PAD      = 4.0f;
static constexpr float TBOX_MIN_H    = 28.0f;
static constexpr float TBOX_DEFAULT_W = 200.0f; // bare-click box width
static constexpr float TBOX_MIN_DRAG  = 8.0f;   // px; smaller drags count as a click

// Screen-space layout of a text box, shared by draw_canvas_text_boxes (render + hover)
// and text_box_at (the synchronous hit-test in mouse_button_callback) so the two can
// never diverge. `scale` is 1.0 today; Part C multiplies it by the canvas zoom for boxes
// flagged zoom_scaled, so glyph size and box dimensions grow with the page.
struct TextBoxLayout { ImVec2 tl, br; float box_w, box_h, wrap, font_px, pad; };
static TextBoxLayout text_box_layout(const CanvasTextBox& box) {
    ImFont* font = ImGui::GetFont();
    Vec2 sp = g_canvas.world_to_screen(box.world_pos);
    // Fixed boxes render at a constant on-screen size; zoom_scaled boxes grow with the
    // canvas zoom so their text stays proportional to the page they annotate. Stored
    // w/h/font_size are always canonical at 100% zoom.
    float scale   = box.zoom_scaled ? g_canvas.get_zoom() : 1.0f;
    float pad     = TBOX_PAD * scale;
    float font_px = box.font_size * scale;
    const char* content = box.text[0] ? box.text : " ";
    float box_w, wrap, box_h;
    if (box.w > 0.0f) {
        box_w = box.w * scale;
        wrap  = box_w - 2.0f * pad;
        ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, wrap, content);
        box_h = std::max(box.h * scale, ts.y + 2.0f * pad);
    } else {
        float natural_w = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, content).x + 2.0f * pad;
        box_w = std::clamp(natural_w, TBOX_MIN_W * scale, TBOX_MAX_W * scale);
        wrap  = box_w - 2.0f * pad;
        ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, wrap, content);
        box_h = std::max(TBOX_MIN_H * scale, ts.y + 2.0f * pad);
    }
    return { {sp.x, sp.y}, {sp.x + box_w, sp.y + box_h}, box_w, box_h, wrap, font_px, pad };
}

// Top-most text box whose screen rect contains (sx, sy), or -1. Iterates in draw order
// so the last (visually top) box wins, matching draw_canvas_text_boxes' hover logic.
int text_box_at(float sx, float sy) {
    if (g_settings_open || g_search_open) return -1;   // boxes not interactive under overlays
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    if (sx >= canvas_right) return -1;                 // clicks inside the panel excluded
    int found = -1;
    for (const auto& box : g_text_boxes) {
        if (g_editing_box == box.id) continue;         // editing box is an ImGui window
        TextBoxLayout L = text_box_layout(box);
        if (sx >= L.tl.x && sx <= L.br.x && sy >= L.tl.y && sy <= L.br.y)
            found = box.id;
    }
    return found;
}

// Draw a dotted rectangle on an ImGui draw list (no native dashed support).
static void imgui_dashed_rect(ImDrawList* dl, ImVec2 tl, ImVec2 br, ImU32 col,
                              float dash = 6.0f, float gap = 4.0f, float thickness = 1.0f) {
    ImVec2 c[4] = { {tl.x, tl.y}, {br.x, tl.y}, {br.x, br.y}, {tl.x, br.y} };  // clockwise
    const float period = dash + gap;
    for (int e = 0; e < 4; ++e) {
        ImVec2 p0 = c[e], p1 = c[(e + 1) & 3];
        float ex = p1.x - p0.x, ey = p1.y - p0.y;
        float len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-3f) continue;
        float ux = ex / len, uy = ey / len;
        for (float s = 0.0f; s < len; s += period) {
            float d1 = std::min(s + dash, len);
            dl->AddLine({p0.x + ux * s, p0.y + uy * s},
                        {p0.x + ux * d1, p0.y + uy * d1}, col, thickness);
        }
    }
}


// Label locked (password-protected) documents' placeholder pages so the distinct
// indigo rect reads clearly instead of looking like a blank page.
void update_item_drag_reconcile() {
    if (g_settings_open || g_search_open) return;
    ImVec2 mouse = ImGui::GetMousePos();
    // Continue drag if LMB still held
    if (g_box_dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Vec2 w = g_canvas.screen_to_world({mouse.x, mouse.y});
            Vec2 delta = w - g_box_drag_start_world;
            for (auto& box : g_text_boxes) {
                for (const auto& state : g_box_drag_states) {
                    if (box.id == state.id) { box.world_pos = state.initial_pos + delta; break; }
                }
            }
            // Move any selected pages that were recorded when the drag started
            for (const auto& ps : g_page_drag_states)
                ps.page->world_pos = ps.initial_pos + delta;
        } else {
            // Drag ended — push undo for moved text boxes
            for (const auto& state : g_box_drag_states) {
                for (const auto& box : g_text_boxes) {
                    if (box.id == state.id) {
                        if (box.world_pos.x != state.initial_pos.x ||
                            box.world_pos.y != state.initial_pos.y) {
                            UndoRecord r;
                            r.type        = UndoRecord::Type::TextBoxMove;
                            r.box_id      = state.id;
                            r.old_box_pos = state.initial_pos;
                            push_undo(r);
                        }
                        break;
                    }
                }
            }
            // Push undo for moved pages (text-box-initiated drag)
            if (!g_page_drag_states.empty()) {
                UndoRecord r;
                r.type = UndoRecord::Type::PageMove;
                for (const auto& ps : g_page_drag_states)
                    if (ps.page->world_pos.x != ps.initial_pos.x ||
                        ps.page->world_pos.y != ps.initial_pos.y)
                        r.page_moves.push_back({ps.page->id, ps.initial_pos});
                if (!r.page_moves.empty()) push_undo(r);
                g_page_drag_states.clear();
            }
            g_box_drag_states.clear();
            g_box_dragging = false;
        }
    }

}

void draw_canvas_text_boxes() {
    // BackgroundDrawList: text boxes live on the canvas layer (above pages/marks but
    // BELOW every ImGui window), so context menus, tooltips, and dialogs correctly draw
    // on top of them. The panel-clip below keeps them from bleeding through a translucent
    // sidebar. These early-outs skip work under the full-screen Settings/Search overlays.
    if (g_settings_open) return;
    if (g_search_open) return;

    ImDrawList* dl    = ImGui::GetBackgroundDrawList();
    ImFont*     font  = ImGui::GetFont();
    ImVec2      mouse = ImGui::GetMousePos();
    ImGuiIO&    io    = ImGui::GetIO();

    // Per-box rendering and hit-testing
    int new_hovered = -1;
    ImVec2 vp = ImGui::GetMainViewport()->Size;

    // Clip all ForegroundDrawList drawing to the canvas area so text boxes don't
    // render on top of the panel when it is open. The panel occupies the rightmost
    // g_panel_w pixels; boxes that overlap it are clipped at the panel's left edge.
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    dl->PushClipRect({0.0f, 0.0f}, {canvas_right, vp.y}, true);

    for (auto& box : g_text_boxes) {
        if (g_editing_box == box.id) continue;  // editing box shown as ImGui window below

        TextBoxLayout L = text_box_layout(box);
        // Off-screen cull using the box's actual on-screen extent (correct for zoom-scaled
        // boxes, whose footprint can be much larger than the fixed-size constants).
        if (L.br.x < 0 || L.tl.x > vp.x || L.br.y < 0 || L.tl.y > vp.y) continue;
        ImVec2 tl = L.tl, br = L.br;
        float  wrap = L.wrap;

        bool hit = !io.WantCaptureMouse
                && mouse.x >= tl.x && mouse.x <= br.x
                && mouse.y >= tl.y && mouse.y <= br.y
                && mouse.x < canvas_right;  // exclude clicks inside the panel

        if (hit) new_hovered = box.id;

        // Draw text (or placeholder). Font size + padding come from the shared layout so
        // a zoom_scaled box (Part C) renders larger; today L.font_px == box.font_size.
        if (box.text[0]) {
            dl->AddText(font, L.font_px, {tl.x + L.pad, tl.y + L.pad},
                        IM_COL32((int)(box.r*255), (int)(box.g*255), (int)(box.b*255), 220),
                        box.text, nullptr, wrap);
        } else {
            dl->AddText(font, L.font_px, {tl.x + L.pad, tl.y + L.pad},
                        IM_COL32(160, 160, 160, 130), "Double-click to edit...");
        }

        // Selection border — thin dotted grey on any selected text box (unified with
        // page selection). The single `g_selected_box` is still used by the text tool
        // for editing/styling, but this shows the dotted border on all selected boxes.
        bool is_selected = g_input.selected_text_boxes().count(box.id) > 0;
        if (is_selected || g_selected_box == box.id) {
            const float m = 6.0f;
            imgui_dashed_rect(dl, {tl.x - m, tl.y - m}, {br.x + m, br.y + m},
                              IM_COL32(215, 215, 222, 235));
        }

        // Click / drag / double-click
        if (hit) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                // Entering box editing disarms any annotation tool (symmetric with
                // the tool-activation paths, which already clear g_editing_box). Also
                // cancels any in-progress stroke so a stray pen line can't be committed.
                g_annot_tool     = AnnotTool::None;
                g_ann_drawing    = false;
                g_selected_box   = box.id;
                g_editing_box    = box.id;
                g_tbox_r         = box.r; g_tbox_g = box.g; g_tbox_b = box.b;
                g_tbox_font_size = box.font_size;
                g_tbox_zoom_scaled = box.zoom_scaled;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // A click that lands on a text box (select/drag) must not also leave
                // an annotation stroke running underneath it. mouse_button_callback's
                // "text box owns this click" guard uses g_hovered_box, which lags one
                // frame — a same-frame move-then-click onto a box can slip past it and
                // start a pen/highlight stroke on the page below. Cancel it here so a
                // stray line can't be drawn while the box is selected or dragged.
                g_ann_drawing = false;
                bool cmd      = (io.KeyCtrl || io.KeySuper);
                bool in_sel   = g_input.selected_text_boxes().count(box.id) > 0;
                auto& sel_ref = const_cast<std::unordered_set<int>&>(g_input.selected_text_boxes());

                // Add/toggle text box in the unified selection
                if (cmd) {
                    // Cmd+click: toggle in/out without clearing other items
                    if (in_sel) sel_ref.erase(box.id);
                    else        sel_ref.insert(box.id);
                } else if (in_sel) {
                    // Plain click on already-selected box: drag the whole group, keep selection
                } else {
                    // Plain click on unselected box: replace entire selection
                    g_input.clear_selection();
                    sel_ref.insert(box.id);
                }

                g_selected_box     = box.id;
                // Selecting a box shows its style in the picker (and becomes the
                // template for the next new box once this one is deselected).
                g_tbox_r = box.r; g_tbox_g = box.g; g_tbox_b = box.b;
                g_tbox_font_size = box.font_size;
                g_tbox_zoom_scaled = box.zoom_scaled;

                // Start multi-box drag — suppressed while any tool is active so boxes/pages
                // don't move in tool mode (selection above still applies).
                if (!g_text_tool && g_annot_tool == AnnotTool::None) {
                    Vec2 w = g_canvas.screen_to_world({mouse.x, mouse.y});
                    g_box_drag_start_world = w;
                    g_box_drag_states.clear();
                    for (int sel_id : g_input.selected_text_boxes()) {
                        for (const auto& b : g_text_boxes) {
                            if (b.id == sel_id) {
                                g_box_drag_states.push_back({sel_id, b.world_pos});
                                break;
                            }
                        }
                    }
                    // Record initial positions of selected pages so they move with the boxes
                    g_page_drag_states.clear();
                    for (Page* p : selected_pages())
                        g_page_drag_states.push_back({p, p->world_pos});
                    g_box_dragging = true;
                }
            }
        }
    }
    g_hovered_box = new_hovered;

    // Deselect text box when clicking on empty canvas
    if (!io.WantCaptureMouse && g_hovered_box < 0 && g_editing_box < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_selected_box >= 0)
        g_selected_box = -1;

    // Create a new box via press-drag-release (text tool active, empty canvas).
    // The drag rectangle sets the box size; a bare click makes a default box.
    if (g_text_tool && !io.WantCaptureMouse && g_editing_box < 0
        && !g_tbox_creating && g_hovered_box < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_tbox_creating     = true;
        g_tbox_create_start = {mouse.x, mouse.y};
    }
    if (g_tbox_creating) {
        ImVec2 a = {std::min(g_tbox_create_start.x, mouse.x), std::min(g_tbox_create_start.y, mouse.y)};
        ImVec2 b = {std::max(g_tbox_create_start.x, mouse.x), std::max(g_tbox_create_start.y, mouse.y)};
        dl->AddRect(a, b, IM_COL32(175, 195, 235, 200), 3.0f, 0, 1.5f);  // live preview

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dw = b.x - a.x, dh = b.y - a.y;
            CanvasTextBox nb;
            nb.id        = g_next_box_id++;
            nb.text[0]   = '\0';
            nb.r = g_tbox_r; nb.g = g_tbox_g; nb.b = g_tbox_b;
            nb.font_size = g_tbox_font_size;
            nb.zoom_scaled = g_tbox_zoom_scaled;
            // Drag-sized boxes: store w/h canonical at 100% zoom so a scaled box drawn
            // while zoomed in doesn't balloon. font_size and the default width are already
            // canonical sizes.
            float inv = nb.zoom_scaled ? (1.0f / g_canvas.get_zoom()) : 1.0f;
            if (dw >= TBOX_MIN_DRAG && dh >= TBOX_MIN_DRAG) {
                nb.world_pos = g_canvas.screen_to_world({a.x, a.y});
                nb.w = dw * inv; nb.h = dh * inv;
            } else {
                // Bare click → default-width, auto-height box anchored at the click.
                nb.world_pos = g_canvas.screen_to_world({g_tbox_create_start.x, g_tbox_create_start.y});
                nb.w = TBOX_DEFAULT_W; nb.h = 0.0f;
            }
            g_text_boxes.push_back(nb);
            g_selected_box = nb.id;
            g_editing_box  = nb.id;
            g_just_created = true;
            { UndoRecord r; r.type = UndoRecord::Type::TextBoxCreate; r.box_id = nb.id; push_undo(r); }
            g_tbox_creating = false;
        }
    }

    dl->PopClipRect();

    // Editing ImGui window
    if (g_editing_box >= 0) {
        CanvasTextBox* eb = nullptr;
        for (auto& box : g_text_boxes)
            if (box.id == g_editing_box) { eb = &box; break; }

        if (!eb) {
            g_editing_box = -1;
        } else {
            // Scale the editor to match how the box renders, so it doesn't visibly jump
            // between edit and display when the box is zoom_scaled.
            float escale = eb->zoom_scaled ? g_canvas.get_zoom() : 1.0f;
            Vec2 sp = g_canvas.world_to_screen(eb->world_pos);
            float ew = ((eb->w > 0.0f) ? eb->w : TBOX_W) * escale;
            float eh = std::max(((eb->h > 0.0f) ? eb->h : 130.0f) * escale, 60.0f);
            ImGui::SetNextWindowPos({sp.x, sp.y}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({ew, eh}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {TBOX_PAD * escale, TBOX_PAD * escale});
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.14f, 0.92f));

            char wid[32]; snprintf(wid, sizeof(wid), "##tbedit%d", g_editing_box);
            ImGui::Begin(wid, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetWindowFontScale(escale);  // scale the edited glyphs to match display
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // First frame of this editing session — g_prev_editing_box is updated
            // at the end of this function, so here it still holds last frame's value.
            bool first_edit_frame = (g_editing_box != g_prev_editing_box);
            if (first_edit_frame)
                ImGui::SetKeyboardFocusHere();
            // Save text before InputTextMultiline: ImGui reverts the buffer on ESC,
            // so we need our own copy to restore ("confirm") rather than discard.
            char saved_text[2048];
            strncpy(saved_text, eb->text, sizeof(saved_text));
            ImGui::InputTextMultiline("##tbtxt", eb->text, sizeof(eb->text), {-1.0f, -1.0f});
            bool esc_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

            // Border around editing window
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRect(
                wp, {wp.x + ws.x, wp.y + ws.y}, IM_COL32(100, 140, 230, 220), 3.0f, 0, 2.0f);

            // Close editing only when the user left-clicks on the canvas (outside all
            // ImGui windows). Clicking a toolbar widget must NOT close editing, because
            // the toolbar color/size controls need g_editing_box to still be set when
            // they fire in the same frame.
            // Skip on the first frame: the window was just created, so
            // WantCaptureMouse is still stale (false) and the double-click that
            // opened editing would otherwise close it immediately.
            bool lost_focus = !first_edit_frame
                              && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                              && !ImGui::GetIO().WantCaptureMouse;
            ImGui::End();

            if (esc_pressed) {
                strncpy(eb->text, saved_text, sizeof(eb->text));  // restore what ImGui reverted
                g_tbox_r = eb->r; g_tbox_g = eb->g; g_tbox_b = eb->b;
                g_tbox_font_size = eb->font_size;
                g_editing_box = -1;   // confirm + close; box stays selected, tool stays active.
                                      // A second ESC (handled globally) exits the tool.
            } else if (lost_focus) {
                g_tbox_r = eb->r; g_tbox_g = eb->g; g_tbox_b = eb->b;
                g_tbox_font_size = eb->font_size;
                g_editing_box = -1;
            }
        }
    }

    // ---- Undo session tracking -------------------------------------------------
    // Text edits: snapshot text when an edit session begins, push one TextBoxEdit
    // record when it ends (if the text changed and the box wasn't freshly created).
    if (g_editing_box != g_prev_editing_box) {
        if (g_prev_editing_box >= 0 && !g_edit_was_new) {
            for (auto& b : g_text_boxes)
                if (b.id == g_prev_editing_box) {
                    if (strcmp(b.text, g_edit_text0) != 0) {
                        UndoRecord r; r.type = UndoRecord::Type::TextBoxEdit;
                        r.box_id = b.id; r.prev_text = g_edit_text0;
                        push_undo(r);
                    }
                    break;
                }
        }
        if (g_editing_box >= 0) {
            for (auto& b : g_text_boxes)
                if (b.id == g_editing_box) {
                    strncpy(g_edit_text0, b.text, sizeof(g_edit_text0) - 1);
                    g_edit_text0[sizeof(g_edit_text0) - 1] = '\0';
                    break;
                }
            g_edit_was_new = g_just_created;
        }
        g_just_created     = false;
        g_prev_editing_box = g_editing_box;
    }

    // Style changes: snapshot color/size when a box is selected, push one
    // TextBoxStyle record when the selection ends (if the style changed).
    if (g_selected_box != g_prev_selected_box) {
        if (g_prev_selected_box >= 0) {
            for (auto& b : g_text_boxes)
                if (b.id == g_prev_selected_box) {
                    if (b.r != g_style_r0 || b.g != g_style_g0 ||
                        b.b != g_style_b0 || b.font_size != g_style_fs0) {
                        UndoRecord r; r.type = UndoRecord::Type::TextBoxStyle;
                        r.box_id = b.id;
                        r.old_r = g_style_r0; r.old_g = g_style_g0;
                        r.old_b = g_style_b0; r.old_fs = g_style_fs0;
                        push_undo(r);
                    }
                    break;
                }
        }
        if (g_selected_box >= 0) {
            for (auto& b : g_text_boxes)
                if (b.id == g_selected_box) {
                    g_style_r0 = b.r; g_style_g0 = b.g; g_style_b0 = b.b; g_style_fs0 = b.font_size;
                    break;
                }
        }
        g_prev_selected_box = g_selected_box;
    }
}


// ===== Exports for the main.cpp render-loop seams =====

int textbox_hovered() { return g_hovered_box; }

// Snapshot the selected text boxes for a drag that began from a PAGE (not a box). Pages move via
// the caller's g_multi_drag_snaps mechanism, so g_page_drag_states stays empty here.
void textboxes_begin_page_initiated_drag() {
    g_page_drag_states.clear();
    if (!g_box_dragging && !g_input.selected_text_boxes().empty()) {
        g_box_drag_start_world = g_input.multi_drag_grab_world();
        g_box_drag_states.clear();
        for (int sid : g_input.selected_text_boxes()) {
            for (const auto& b : g_text_boxes) {
                if (b.id == sid) { g_box_drag_states.push_back({sid, b.world_pos}); break; }
            }
        }
        g_box_dragging = true;
    }
}

void textboxes_reset() {
    g_box_dragging = false;
    g_box_drag_states.clear();
    g_page_drag_states.clear();
    g_tbox_creating = false;
    g_hovered_box   = -1;
}
