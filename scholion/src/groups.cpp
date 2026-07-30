#include "groups.h"
#include "app_state.h"
#include "document.h"
#include "undo.h"
#include "project_io.h"   // g_dirty
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <cfloat>
#include <vector>

// --- Page groups ------------------------------------------------------------
// A group is an ad-hoc, nondestructive cluster of pages (possibly from several
// documents) that can be moved together by grabbing its frame. Membership lives
// on Page::group_id; g_groups holds each group's color/label. Movement reuses the
// existing multi-drag machinery (begin_group_drag), so nothing about page or
// document dragging changes — the group is opt-in via its frame handle.

static constexpr float GROUP_FRAME_PAD   = 10.0f; // innermost boundary offset from member pages
static constexpr float GROUP_RING_GAP    = 4.0f;  // spacing between per-document boundary rings
static constexpr float GROUP_RING_ROUND  = 20.0f; // corner radius — bubble-like, node-editor feel
static constexpr float GROUP_RING_THICK   = 1.5f; // boundary line thickness
static constexpr int   GROUP_RING_ALPHA   = 165;  // dim, like the thread wires
static constexpr float GROUP_HANDLE_BAND = 11.0f; // grab thickness around the boundary
static constexpr float GROUP_LABEL_H     = 20.0f; // label tab height (screen px)
static constexpr float GROUP_LABEL_INSET = 14.0f; // shift the label in from the rounded corner
static constexpr double GROUP_DRAGADD_DWELL = 0.8; // seconds to hover a group before it swallows pages

// Group name being renamed inline (0 = none) + its edit buffer.
static int  g_editing_group = 0;
static char g_group_name_buf[256] = "";

// Drag-to-add-into-group state (a page held over a group for GROUP_DRAGADD_DWELL joins it).
static std::vector<Page*> g_dragadd_pages;         // pages currently being dragged
static int    g_dragadd_target = 0;                // group under the cursor that could swallow them
static double g_dragadd_since  = 0.0;              // time the cursor entered g_dragadd_target
static bool   g_dragadd_ready  = false;            // dwell satisfied — release will join
static bool   g_was_dragging_for_add = false;      // previous-frame drag state (edge detect on release)

// Brief fading outline on a page as it leaves a group — a subtle cue that its tie released.
// Keyed by a snapshot of the page's world rect + color (not a pointer), so it can't dangle.
static constexpr double GROUP_REMOVE_FLASH_DUR = 0.75;  // seconds
struct GroupRemoveFlash { float x0, y0, x1, y1; float r, g, b; double t0; };
static std::vector<GroupRemoveFlash> g_group_remove_flashes;

std::vector<Page*> group_pages(int gid) {
    std::vector<Page*> out;
    if (gid == 0) return out;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.group_id == gid) out.push_back(&p);
    return out;
}

static const PageGroup* find_group(int gid) {
    for (const auto& g : g_groups) if (g.id == gid) return &g;
    return nullptr;
}

// Distinct documents contributing pages to a group, in g_documents order. The
// count drives how many concentric boundary rings are drawn (one per document,
// in that document's hue); the order fixes which ring sits where.
static std::vector<const Document*> group_contrib_docs(int gid) {
    std::vector<const Document*> out;
    if (gid == 0) return out;
    for (const auto& doc : g_documents) {
        bool has = false;
        for (const auto& p : doc.pages) if (p.group_id == gid) { has = true; break; }
        if (has) out.push_back(&doc);
    }
    return out;
}

// Tight screen-space AABB of a group's member pages (no padding). false if empty.
static bool group_base_screen(int gid, ImVec2& tl, ImVec2& br) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool any = false;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.group_id == gid) {
                any = true;
                x0 = std::min(x0, p.world_pos.x);
                y0 = std::min(y0, p.world_pos.y);
                x1 = std::max(x1, p.world_pos.x + p.world_w);
                y1 = std::max(y1, p.world_pos.y + p.world_h);
            }
    if (!any) return false;
    Vec2 s_tl = g_canvas.world_to_screen({x0, y0});
    Vec2 s_br = g_canvas.world_to_screen({x1, y1});
    tl = {s_tl.x, s_tl.y};
    br = {s_br.x, s_br.y};
    return true;
}

// Outermost boundary ring rect (screen space) — base bounds grown by the full ring
// stack. Used as the grab region and the anchor for the label tab.
static bool group_outer_frame(int gid, ImVec2& tl, ImVec2& br) {
    ImVec2 b_tl, b_br;
    if (!group_base_screen(gid, b_tl, b_br)) return false;
    int rings = std::max(1, (int)group_contrib_docs(gid).size());
    float off = GROUP_FRAME_PAD + (rings - 1) * GROUP_RING_GAP;
    tl = {b_tl.x - off, b_tl.y - off};
    br = {b_br.x + off, b_br.y + off};
    return true;
}

// Label-tab rectangle, derived identically for draw and hit-test.
static ImVec4 group_label_screen(ImVec2 frame_tl, const char* label) {
    float font_px = ImGui::GetFontSize();
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(font_px, FLT_MAX, 0.0f, label);
    float w = std::max(ts.x + 16.0f, 44.0f);
    // Inset from the corner so the tab clears the boundary's rounded radius.
    float x0 = frame_tl.x + GROUP_LABEL_INSET;
    return { x0, frame_tl.y - GROUP_LABEL_H, x0 + w, frame_tl.y };
}

// Topmost group whose boundary or label tab is under (sx, sy); 0 = none.
int group_handle_at(float sx, float sy) {
    if (g_settings_open || g_search_open) return 0;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    if (sx >= canvas_right) return 0;
    int found = 0;
    for (const auto& grp : g_groups) {
        ImVec2 tl, br;
        if (!group_outer_frame(grp.id, tl, br)) continue;
        const char* label = grp.name.empty() ? "Group" : grp.name.c_str();
        ImVec4 lr = group_label_screen(tl, label);
        bool in_label = sx >= lr.x && sx <= lr.z && sy >= lr.y && sy <= lr.w;
        // Grab band straddling the outermost boundary line.
        float bnd = GROUP_HANDLE_BAND;
        bool in_outer = sx >= tl.x - bnd && sx <= br.x + bnd && sy >= tl.y - bnd && sy <= br.y + bnd;
        bool in_inner = sx >  tl.x + bnd && sx <  br.x - bnd && sy >  tl.y + bnd && sy <  br.y - bnd;
        if (in_label || (in_outer && !in_inner)) found = grp.id;
    }
    return found;
}

// Topmost group whose outer frame *interior* contains (sx, sy); 0 = none. Used by the
// drag-to-add dwell test (the whole enclosed area is a drop target, not just the border).
static int group_area_at(float sx, float sy) {
    if (g_settings_open || g_search_open) return 0;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    if (sx >= canvas_right) return 0;
    int found = 0;
    for (const auto& grp : g_groups) {
        ImVec2 tl, br;
        if (!group_outer_frame(grp.id, tl, br)) continue;
        if (sx >= tl.x && sx <= br.x && sy >= tl.y && sy <= br.y) found = grp.id;
    }
    return found;
}

void draw_page_groups() {
    if (g_settings_open || g_search_open) return;

    // Drop a stale rename target if its group is gone.
    if (g_editing_group != 0 && !find_group(g_editing_group)) g_editing_group = 0;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (auto& grp : g_groups) {
        ImVec2 base_tl, base_br;
        if (!group_base_screen(grp.id, base_tl, base_br)) continue;  // empty — nothing to draw

        std::vector<const Document*> docs = group_contrib_docs(grp.id);
        int rings = std::max(0, (int)docs.size() - 1);
        float outer_off = GROUP_FRAME_PAD + rings * GROUP_RING_GAP;

        // Drag-to-add feedback: a soft interior wash while a dragged page dwells over
        // this group, brightening once the dwell is satisfied (release will join).
        if (grp.id == g_dragadd_target) {
            int a = g_dragadd_ready ? 60 : 26;
            dl->AddRectFilled({base_tl.x - outer_off, base_tl.y - outer_off},
                              {base_br.x + outer_off, base_br.y + outer_off},
                              IM_COL32(255, 255, 255, a), GROUP_RING_ROUND);
        }

        // One solid, dim, rounded boundary ring per contributing document, in that
        // document's hue. Multiple documents => concentric multi-colored rings a few
        // px apart — the node-editor thread aesthetic applied to a page cluster.
        for (int i = 0; i < (int)docs.size(); ++i) {
            float off = GROUP_FRAME_PAD + i * GROUP_RING_GAP;
            ImU32 col = IM_COL32((int)(docs[i]->hue_r * 255),
                                 (int)(docs[i]->hue_g * 255),
                                 (int)(docs[i]->hue_b * 255), GROUP_RING_ALPHA);
            dl->AddRect({base_tl.x - off, base_tl.y - off},
                        {base_br.x + off, base_br.y + off},
                        col, GROUP_RING_ROUND, ImDrawFlags_RoundCornersAll, GROUP_RING_THICK);
        }

        ImVec2 outer_tl = {base_tl.x - outer_off, base_tl.y - outer_off};

        const char* label = grp.name.empty() ? "Group" : grp.name.c_str();
        ImVec4 lr = group_label_screen(outer_tl, label);

        // Label tab tinted with the group's lead document hue (darkened for legible light text),
        // so the tab reads as part of the group's color theme.
        ImU32 pill_col;
        if (!docs.empty())
            pill_col = IM_COL32((int)(docs[0]->hue_r * 0.45f * 255),
                                (int)(docs[0]->hue_g * 0.45f * 255),
                                (int)(docs[0]->hue_b * 0.45f * 255), 230);
        else
            pill_col = IM_COL32(38, 35, 32, 215);

        if (g_editing_group == grp.id) {
            // Inline rename: a small borderless InputText at the label tab.
            ImGui::SetNextWindowPos({lr.x, lr.y});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2.0f, 2.0f});
            ImGui::Begin("##grp_rename", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetNextItemWidth(std::max(120.0f, lr.z - lr.x));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            // Enter confirms (no line breaks needed); Escape reverts; clicking out commits.
            ImGui::InputText("##grpname", g_group_name_buf, sizeof(g_group_name_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemDeactivated()) {
                if (ImGui::IsItemDeactivatedAfterEdit()) { grp.name = g_group_name_buf; g_dirty = true; }
                g_editing_group = 0;
            }
            ImGui::End();
            ImGui::PopStyleVar();
        } else {
            dl->AddRectFilled({lr.x, lr.y}, {lr.z, lr.w}, pill_col, 5.0f, ImDrawFlags_RoundCornersTop);
            dl->AddText({lr.x + 8.0f, lr.y + (GROUP_LABEL_H - ImGui::GetFontSize()) * 0.5f},
                        IM_COL32(235, 231, 225, 255), label);
            // Double-click the tab to rename it.
            if (!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImVec2 m = ImGui::GetMousePos();
                if (m.x >= lr.x && m.x <= lr.z && m.y >= lr.y && m.y <= lr.w) {
                    g_editing_group = grp.id;
                    strncpy(g_group_name_buf, grp.name.c_str(), sizeof(g_group_name_buf) - 1);
                    g_group_name_buf[sizeof(g_group_name_buf) - 1] = '\0';
                }
            }
        }
    }
}

// Group the currently selected pages into a fresh group (flat: each page joins
// this group, leaving any previous one). Nondestructive and undoable.
void create_group_from_selection() {
    const auto& sel = g_input.selection();
    if (sel.empty()) return;
    int gid = g_next_group_id++;
    PageGroup grp;
    grp.id = gid;
    const float* cv = GROUP_PALETTE[(gid - 1) % GROUP_PALETTE_SIZE];
    grp.col_r = cv[0]; grp.col_g = cv[1]; grp.col_b = cv[2];

    UndoRecord r; r.type = UndoRecord::Type::Group; r.group_row = grp;
    for (Page* p : selected_pages()) {
        r.group_members.push_back({p->id, p->group_id});
        p->group_id = gid;
    }
    g_groups.push_back(grp);
    push_undo(r);
}

// Dissolve a group: members revert to ungrouped, the row is removed. Undoable.
void ungroup_group(int gid) {
    if (gid == 0) return;
    if (g_editing_group == gid) g_editing_group = 0;
    const PageGroup* g = find_group(gid);
    UndoRecord r; r.type = UndoRecord::Type::Ungroup;
    if (g) r.group_row = *g;
    bool any = false;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.group_id == gid) {
                r.group_members.push_back({p.id, gid});
                p.group_id = 0;
                any = true;
            }
    g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(),
                   [&](const PageGroup& x){ return x.id == gid; }), g_groups.end());
    if (any) push_undo(r);
}

// Add pages to an existing group (drag-to-add). Flat rule: a page joins this group,
// leaving any previous one (old_group recorded for undo). The target row already
// exists, so undo (GroupJoin) only reverts membership — it never removes the row.
static void add_pages_to_group(int gid, const std::vector<Page*>& pages) {
    if (gid == 0 || !find_group(gid)) return;
    UndoRecord r; r.type = UndoRecord::Type::GroupJoin;
    bool any = false;
    for (Page* p : pages) {
        if (!p || p->group_id == gid) continue;
        r.group_members.push_back({p->id, p->group_id});
        p->group_id = gid;
        any = true;
    }
    if (any) push_undo(r);
}

// Per-frame: while pages are being dragged, arm a target group once the cursor dwells
// over it long enough; on release, the target swallows the dragged pages. Add-only —
// dropping outside any group never removes a page (Ungroup is the removal path).
void update_group_drag_add() {
    bool dragging = (g_input.dragged_page() != nullptr) || g_input.is_multi_dragging();
    if (dragging) {
        g_dragadd_pages.clear();
        if (g_input.is_multi_dragging()) {
            for (Page* p : selected_pages()) g_dragadd_pages.push_back(p);
        } else if (const Page* dp = g_input.dragged_page()) {
            g_dragadd_pages.push_back(const_cast<Page*>(dp));
        }
        ImVec2 m = ImGui::GetMousePos();
        int cand = group_area_at(m.x, m.y);
        if (cand != 0) {
            // Only a candidate if at least one dragged page isn't already in it.
            bool any_new = false;
            for (Page* p : g_dragadd_pages) if (p->group_id != cand) { any_new = true; break; }
            if (!any_new) cand = 0;
        }
        if (cand != g_dragadd_target) {
            g_dragadd_target = cand;
            g_dragadd_since  = ImGui::GetTime();
            g_dragadd_ready  = false;
        } else if (cand != 0 && !g_dragadd_ready &&
                   ImGui::GetTime() - g_dragadd_since >= GROUP_DRAGADD_DWELL) {
            g_dragadd_ready = true;
        }
    } else {
        // Drag ended this frame — commit if a target was armed and satisfied.
        if (g_was_dragging_for_add && g_dragadd_ready && g_dragadd_target != 0 && !g_dragadd_pages.empty())
            add_pages_to_group(g_dragadd_target, g_dragadd_pages);
        g_dragadd_pages.clear();
        g_dragadd_target = 0;
        g_dragadd_ready  = false;
    }
    g_was_dragging_for_add = dragging;
}

// Remove a single page from its group (nondestructive, undoable) — the deliberate,
// menu-driven counterpart to drag-to-add, so a stray drag can't change membership. The
// group's boundary recomputes to omit the page automatically; a brief fading outline in
// the page's document color flags the change.
void remove_page_from_group(Page* p) {
    if (!p || p->group_id == 0) return;
    UndoRecord r; r.type = UndoRecord::Type::GroupJoin;   // GroupJoin = revert membership only
    r.group_members.push_back({p->id, p->group_id});

    float fr = 0.6f, fg = 0.6f, fb = 0.6f;                // flash color = owning document hue
    for (const auto& doc : g_documents) {
        bool has = false;
        for (const auto& pg : doc.pages) if (&pg == p) { has = true; break; }
        if (has) { fr = doc.hue_r; fg = doc.hue_g; fb = doc.hue_b; break; }
    }
    g_group_remove_flashes.push_back({ p->world_pos.x, p->world_pos.y,
                                       p->world_pos.x + p->world_w, p->world_pos.y + p->world_h,
                                       fr, fg, fb, ImGui::GetTime() });
    p->group_id = 0;
    push_undo(r);
}

// Draw + age the "left the group" page flashes. Fades a rounded outline (and a faint fill)
// out over GROUP_REMOVE_FLASH_DUR. Runs on the canvas layer (below the UI).
void draw_group_remove_flashes() {
    if (g_group_remove_flashes.empty()) return;
    double now = ImGui::GetTime();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const auto& f : g_group_remove_flashes) {
        float t = (float)((now - f.t0) / GROUP_REMOVE_FLASH_DUR);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) continue;
        float ease = 1.0f - t;                 // fade out
        Vec2 tl = g_canvas.world_to_screen({f.x0, f.y0});
        Vec2 br = g_canvas.world_to_screen({f.x1, f.y1});
        ImU32 line = IM_COL32((int)(f.r * 255), (int)(f.g * 255), (int)(f.b * 255),
                              (int)(235.0f * ease));
        ImU32 fill = IM_COL32((int)(f.r * 255), (int)(f.g * 255), (int)(f.b * 255),
                              (int)(45.0f * ease));
        dl->AddRectFilled({tl.x, tl.y}, {br.x, br.y}, fill, GROUP_RING_ROUND);
        dl->AddRect({tl.x, tl.y}, {br.x, br.y}, line, GROUP_RING_ROUND,
                    ImDrawFlags_RoundCornersAll, 2.0f);
    }
    g_group_remove_flashes.erase(
        std::remove_if(g_group_remove_flashes.begin(), g_group_remove_flashes.end(),
            [&](const GroupRemoveFlash& f){ return (now - f.t0) >= GROUP_REMOVE_FLASH_DUR; }),
        g_group_remove_flashes.end());
}

// --- Module lifecycle / animation hooks (exposed via groups.h) --------------

// Clear transient group UI state on new/loaded project. The group *table* (g_groups)
// and g_next_group_id are owned by app-state reset in main.cpp; this drops only the
// in-flight editing/drag/flash state that must never survive a project switch.
void groups_reset() {
    g_editing_group = 0;
    g_group_name_buf[0] = '\0';
    g_dragadd_pages.clear();
    g_dragadd_target = 0;
    g_dragadd_ready  = false;
    g_was_dragging_for_add = false;
    g_group_remove_flashes.clear();
}

// True while a "left the group" flash is still fading — drives the redraw loop.
bool groups_animating() {
    return !g_group_remove_flashes.empty();
}
