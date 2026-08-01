#include "side_panel.h"
#include "app_state.h"
#include "canvas_annot.h"     // g_ann_*, stroke_add_point, finalize_annotation, note_label, MARKER_*
#include "rast_pipeline.h"    // enqueue_rast
#include "references_panel.h" // draw_references_tab
#include "search.h"           // SearchResult/SearchHighlight + g_search_*
#include "undo.h"
#include "project_io.h"       // save_prefs
#include "imgui.h"
#include <algorithm>
#include <string>
#include <filesystem>

// ===== Panel state (g_* pair is extern in side_panel.h; main touches them) =====
int  g_panel_nav_page    = 0;      // current page index in open panel (0-based)
bool g_panel_ann_active  = false;  // true while annotating from the sidebar panel
static int  s_last_panel_doc     = 0;      // last doc shown — used by edge tabs to reopen
static bool s_panel_open_to_refs = false;  // next panel open lands on References tab

// Annotation input (Note / Pen / Highlight / Eraser) on one panel page. Mirrors the canvas
// annotation handling: writes the shared g_ann_* state + page.annots and pushes undo records.
// finalize_annotation() is called by the main draw loop when the stroke ends.
static void panel_page_annotation_input(Page& page, int doc_idx, int pi,
                                        ImVec2 img_pos, float img_w, float img_h, ImDrawList* dl) {
    ImVec2 mouse  = ImGui::GetMousePos();
    float  nx     = std::clamp((mouse.x - img_pos.x) / img_w, 0.0f, 1.0f);
    float  ny     = std::clamp((mouse.y - img_pos.y) / img_h, 0.0f, 1.0f);
    Vec2   pnorm  = {nx, ny};
    bool   hov    = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if (g_annot_tool == AnnotTool::Note && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int snap = g_next_note_idx;
        page.annots.notes.push_back({note_label(g_next_note_idx++)});
        UndoRecord r; r.type = UndoRecord::Type::Note;
        r.doc_idx = doc_idx; r.page_idx = pi; r.note_idx_before = snap;
        push_undo(r);
    }

    if ((g_annot_tool == AnnotTool::Highlight || g_annot_tool == AnnotTool::Pen)
            && !g_ann_drawing && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_ann_drawing      = true;
        g_ann_doc_idx      = doc_idx;
        g_ann_page_idx     = pi;
        g_panel_ann_active = true;
        if (g_annot_tool == AnnotTool::Pen ||
            (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode)) {
            g_ann_cur_stroke = {};
            if (g_annot_tool == AnnotTool::Pen) {
                g_ann_cur_stroke.r = g_pen_r; g_ann_cur_stroke.g = g_pen_g; g_ann_cur_stroke.b = g_pen_b;
            } else {
                g_ann_cur_stroke.r = g_hl_r; g_ann_cur_stroke.g = g_hl_g; g_ann_cur_stroke.b = g_hl_b;
                g_ann_cur_stroke.width = MARKER_HALF_W; g_ann_cur_stroke.alpha = MARKER_ALPHA;
            }
            g_ann_cur_stroke.pts.push_back(pnorm);
        }
        g_ann_hl_start = pnorm;
        g_ann_cur_norm = pnorm;
    }

    if (g_ann_drawing && g_panel_ann_active &&
            g_ann_doc_idx == doc_idx && g_ann_page_idx == pi) {
        if ((g_annot_tool == AnnotTool::Pen ||
             (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode))
                && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            stroke_add_point(page, pnorm, ImGui::GetIO().KeyShift);  // Shift = ortho-lock
            const auto& pts = g_ann_cur_stroke.pts;
            bool  hlt   = (g_annot_tool == AnnotTool::Highlight);
            ImU32 col   = hlt
                ? IM_COL32((int)(g_hl_r*255),(int)(g_hl_g*255),(int)(g_hl_b*255),(int)(MARKER_ALPHA*255))
                : IM_COL32((int)(g_pen_r*255),(int)(g_pen_g*255),(int)(g_pen_b*255),220);
            float thick = hlt ? MARKER_HALF_W * 2.0f : 2.0f;
            for (int si = 1; si < (int)pts.size(); ++si) {
                ImVec2 a = {img_pos.x + pts[si-1].x * img_w, img_pos.y + pts[si-1].y * img_h};
                ImVec2 b = {img_pos.x + pts[si  ].x * img_w, img_pos.y + pts[si  ].y * img_h};
                dl->AddLine(a, b, col, thick);
            }
        } else if (g_annot_tool == AnnotTool::Highlight && g_hl_box_mode) {
            // Box mode — track the rectangle and preview it in yellow.
            g_ann_cur_norm = pnorm;
            float x0 = std::min(g_ann_hl_start.x, pnorm.x), y0 = std::min(g_ann_hl_start.y, pnorm.y);
            float x1 = std::max(g_ann_hl_start.x, pnorm.x), y1 = std::max(g_ann_hl_start.y, pnorm.y);
            dl->AddRectFilled(
                {img_pos.x + x0 * img_w, img_pos.y + y0 * img_h},
                {img_pos.x + x1 * img_w, img_pos.y + y1 * img_h},
                IM_COL32(255, 224, 0, 80));
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            g_panel_ann_active = false;
            // finalize_annotation() is called by the main draw loop
    }

    if (g_annot_tool == AnnotTool::Eraser && hov && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        constexpr float ER = 0.025f;
        for (int hi = (int)page.annots.highlights.size() - 1; hi >= 0; --hi) {
            const auto& hl = page.annots.highlights[hi];
            float cx = std::clamp(nx, hl.x0, hl.x1), cy = std::clamp(ny, hl.y0, hl.y1);
            if ((nx-cx)*(nx-cx) + (ny-cy)*(ny-cy) <= ER*ER) {
                UndoRecord r; r.type = UndoRecord::Type::ErasedHighlight;
                r.doc_idx = doc_idx; r.page_idx = pi; r.erased_highlight = hl;
                push_undo(r);
                page.annots.highlights.erase(page.annots.highlights.begin() + hi);
            }
        }
        for (int si = (int)page.annots.strokes.size() - 1; si >= 0; --si) {
            bool hit = false;
            for (const auto& pt : page.annots.strokes[si].pts) {
                float dx = nx - pt.x, dy = ny - pt.y;
                if (dx*dx + dy*dy <= ER*ER) { hit = true; break; }
            }
            if (hit) {
                UndoRecord r; r.type = UndoRecord::Type::ErasedStroke;
                r.doc_idx = doc_idx; r.page_idx = pi;
                r.erased_stroke = page.annots.strokes[si];
                push_undo(r);
                page.annots.strokes.erase(page.annots.strokes.begin() + si);
            }
        }
    }
}

// Render one page in the panel Viewer: image/placeholder (+ Low-tier LOD request), search-hit
// highlights, annotation input, saved annotations, and note badges.
static void draw_panel_page(int doc_idx, int pi, float avail_w, int scroll_to) {
    Document& doc = g_documents[doc_idx];
    Page& page = doc.pages[pi];

    // Panel uses Low tier (150 DPI); sufficient for the panel's ~360px display width.
    // High-tier tiles are managed by stream_lod for canvas use only.
    if (page.needs_lod(LodTier::Low) && g_loaders[doc_idx])
        enqueue_rast(doc.path, g_loaders[doc_idx], page.page_index, LodTier::Low);
    uint32_t tex = page.tex_for_lod(LodTier::Low);

    float img_w = avail_w;
    float img_h = (page.world_w > 0.0f)
        ? page.world_h * (avail_w / page.world_w)
        : avail_w * 1.41f;

    ImVec2 img_pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Check if this page is the highlighted search result
    bool is_highlighted = false;
    if (g_highlighted_search_result >= 0 &&
        g_highlighted_search_result < (int)g_search_results.size()) {
        const auto& hit = g_search_results[g_highlighted_search_result];
        is_highlighted = (hit.doc_idx == doc_idx && hit.page_idx == pi);
    }

    // Render image or placeholder
    if (tex) {
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                     {img_w, img_h});
    } else {
        dl->AddRectFilled(img_pos, {img_pos.x + img_w, img_pos.y + img_h},
                          IM_COL32(55, 55, 58, 255));
        ImGui::Dummy({img_w, img_h});
    }

    // Draw blue highlight border if this is the highlighted search result
    if (is_highlighted) {
        dl->AddRect(img_pos, {img_pos.x + img_w, img_pos.y + img_h},
                   IM_COL32(100, 150, 255, 200), 0.0f, 0, 4.0f);
    }

    // Draw orange search hit rects (text-level, from g_search_highlight)
    if (g_search_highlight.active() &&
        g_search_highlight.doc_idx == doc_idx &&
        g_search_highlight.page_idx == pi) {
        for (const auto& r : g_search_highlight.rects) {
            ImVec2 tl = {img_pos.x + r[0] * img_w, img_pos.y + r[1] * img_h};
            ImVec2 br = {img_pos.x + r[2] * img_w, img_pos.y + r[3] * img_h};
            dl->AddRectFilled(tl, br, IM_COL32(255, 128, 0, 130));
        }
    }

    // Annotation tool interaction on this panel page.
    panel_page_annotation_input(page, doc_idx, pi, img_pos, img_w, img_h, dl);

    // Draw saved annotations on top of page
    for (const auto& hl : page.annots.highlights) {
        ImVec2 tl = {img_pos.x + hl.x0 * img_w, img_pos.y + hl.y0 * img_h};
        ImVec2 br = {img_pos.x + hl.x1 * img_w, img_pos.y + hl.y1 * img_h};
        dl->AddRectFilled(tl, br, IM_COL32(255, 224, 0, 80));
    }
    for (const auto& stroke : page.annots.strokes) {
        int sa = (int)(stroke.alpha * 255.0f);
        for (int si = 1; si < (int)stroke.pts.size(); ++si) {
            ImVec2 a = {img_pos.x + stroke.pts[si-1].x * img_w,
                        img_pos.y + stroke.pts[si-1].y * img_h};
            ImVec2 b = {img_pos.x + stroke.pts[si  ].x * img_w,
                        img_pos.y + stroke.pts[si  ].y * img_h};
            dl->AddLine(a, b,
                IM_COL32((int)(stroke.r*255), (int)(stroke.g*255), (int)(stroke.b*255), sa),
                stroke.width * 2.0f);
        }
    }

    // Note badges — stacked from top-right corner, left to right.
    // When the Note tool is active, clicking a badge removes that flag.
    {
        constexpr float NBW = 16.0f, NBH = 16.0f, NGAP = 2.0f;
        float nbx = img_pos.x + img_w;
        int remove_idx = -1;
        for (int ni = 0; ni < (int)page.annots.notes.size(); ++ni) {
            nbx -= NBW + NGAP;
            ImVec2 ntl = {nbx,        img_pos.y};
            ImVec2 nbr = {nbx + NBW,  img_pos.y + NBH};
            dl->AddRectFilled(ntl, nbr, IM_COL32(50, 110, 210, 230), 2.0f);
            const auto& note = page.annots.notes[ni];
            ImVec2 tsz = ImGui::CalcTextSize(note.label.c_str());
            dl->AddText({ntl.x + (NBW - tsz.x) * 0.5f, ntl.y + (NBH - tsz.y) * 0.5f},
                        IM_COL32(255, 255, 255, 255), note.label.c_str());
        }
    }

    if (scroll_to == page.page_index) {
        ImGui::SetScrollHereY(0.0f);
        g_input.clear_panel_scroll();
    }

    ImGui::Spacing();
}

void draw_panel_ui() {
    if (!g_input.panel_open()) return;

    int doc_idx = g_input.panel_doc_index();
    if (doc_idx < 0 || doc_idx >= static_cast<int>(g_documents.size())) {
        g_input.close_panel();
        return;
    }

    Document& doc = g_documents[doc_idx];
    s_last_panel_doc = doc_idx;  // remember for edge-tab re-open
    ImVec2 vp = ImGui::GetMainViewport()->Size;

    ImGui::SetNextWindowPos({vp.x - g_panel_w, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({g_panel_w, vp.y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar           |
        ImGuiWindowFlags_NoResize             |
        ImGuiWindowFlags_NoMove               |
        ImGuiWindowFlags_NoCollapse           |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f, 10.0f});
    ImGui::Begin("##panel", nullptr, kFlags);
    ImGui::PopStyleVar();

    // Track the "active" page for nav and copy (updated on scroll-to events)
    int scroll_to_peek = g_input.panel_scroll_page();  // read before clear
    if (ImGui::IsWindowAppearing()) g_panel_nav_page = std::max(0, scroll_to_peek);
    else if (scroll_to_peek >= 0)  g_panel_nav_page = scroll_to_peek;

    // Tab bar — "Viewer" / "References" — blue-themed, pinned at the top of the panel.
    // On first appearance, honour s_panel_open_to_refs to select the right tab.
    namespace fs = std::filesystem;
    bool just_appeared = ImGui::IsWindowAppearing();
    ImGuiTabItemFlags viewer_flags = (just_appeared && !s_panel_open_to_refs)
                                     ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags ref_flags    = (just_appeared &&  s_panel_open_to_refs)
                                     ? ImGuiTabItemFlags_SetSelected : 0;
    if (just_appeared) s_panel_open_to_refs = false;

    ImGui::PushStyleColor(ImGuiCol_Tab,                 ImVec4(0.14f, 0.33f, 0.65f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,          ImVec4(0.20f, 0.42f, 0.78f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,         ImVec4(0.27f, 0.51f, 0.88f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.50f, 0.75f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed,           ImVec4(0.10f, 0.24f, 0.50f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,   ImVec4(0.18f, 0.40f, 0.72f, 0.90f));
    ImGui::BeginTabBar("##panel_tabs");
    ImGui::PopStyleColor(6);

    if (ImGui::BeginTabItem("Viewer", nullptr, viewer_flags)) {
        // Document name below tab bar
        std::string fname = fs::path(doc.path).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, {doc.hue_r, doc.hue_g, doc.hue_b, 1.0f});
        ImGui::TextUnformatted(fname.c_str());
        ImGui::PopStyleColor();

        // Page navigation  < p.N/Total >
        {
            int total = (int)doc.pages.size();
            if (ImGui::SmallButton("<") && g_panel_nav_page > 0) {
                g_panel_nav_page--;
                g_input.open_panel(doc_idx, g_panel_nav_page);
            }
            ImGui::SetItemTooltip("Previous page");
            ImGui::SameLine();
            ImGui::Text("p.%d/%d", g_panel_nav_page + 1, total);
            ImGui::SameLine();
            if (ImGui::SmallButton(">") && g_panel_nav_page < total - 1) {
                g_panel_nav_page++;
                g_input.open_panel(doc_idx, g_panel_nav_page);
            }
            ImGui::SetItemTooltip("Next page");
        }

    // Child window fills the remaining panel height and is the only scrollable region.
    ImGui::BeginChild("##panel_scroll", {0.0f, 0.0f}, false, ImGuiWindowFlags_None);
    ImGui::Spacing();

    float avail_w  = ImGui::GetContentRegionAvail().x;
    int   scroll_to = g_input.panel_scroll_page();

    for (int pi = 0; pi < (int)doc.pages.size(); ++pi)
        draw_panel_page(doc_idx, pi, avail_w, scroll_to);

    ImGui::EndChild();
    ImGui::EndTabItem();
    } // Viewer tab

    if (ImGui::BeginTabItem("References", nullptr, ref_flags)) {
        ImGui::BeginChild("##panel_ref_scroll", {0.0f, 0.0f}, false, ImGuiWindowFlags_None);
        draw_references_tab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

void draw_panel_resize_handle() {
    if (!g_input.panel_open()) return;

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    constexpr float STRIP_W = 12.0f;

    ImGui::SetNextWindowPos({vp.x - g_panel_w - STRIP_W * 0.5f, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({STRIP_W, vp.y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0f, 1.0f});
    constexpr ImGuiWindowFlags kRFlags =
        ImGuiWindowFlags_NoTitleBar          |
        ImGuiWindowFlags_NoResize            |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoScrollbar         |
        ImGuiWindowFlags_NoSavedSettings     |
        ImGuiWindowFlags_NoBackground        |
        ImGuiWindowFlags_NoFocusOnAppearing  |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##panel_resize", nullptr, kRFlags);
    ImGui::PopStyleVar(2);

    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::InvisibleButton("##drag", {STRIP_W, vp.y});
    bool is_active  = ImGui::IsItemActive();
    bool is_hovered = ImGui::IsItemHovered();

    if (is_active) {
        g_panel_w -= ImGui::GetIO().MouseDelta.x;
        g_panel_w  = std::clamp(g_panel_w, 180.0f, vp.x - 30.0f);
    } else if (ImGui::IsItemDeactivated()) {
        g_settings.panel_w = g_panel_w;
        save_prefs();
    }
    if (is_hovered || is_active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // Blue divider line — brighter when hovered/dragged
    float cx = wp.x + STRIP_W * 0.5f;
    ImU32 line_col = is_active  ? IM_COL32(110, 165, 255, 230)
                   : is_hovered ? IM_COL32(80, 140, 230, 200)
                   :              IM_COL32(55, 108, 190, 150);
    ImGui::GetWindowDrawList()->AddLine({cx, wp.y}, {cx, wp.y + vp.y}, line_col, 2.5f);

    ImGui::End();
}

// --- Panel edge tabs (shown on right edge when sidebar is closed) -----------
// Two vertical tab handles sitting flush on the right edge — characters are
// rendered stacked top-to-bottom so the label reads downward without rotation.
// They disappear once the sidebar is open.

// Render text rotated 90° CCW, centered on 'center', using glyph quads so
// the result is crisp at any size.  Reading direction: bottom → top.
static void draw_text_ccw(ImDrawList* dl, ImVec2 center, const char* text, ImU32 col)
{
    ImFont*  font  = ImGui::GetFont();
    float    scale = ImGui::GetFontSize() / font->FontSize;
    ImVec2   tsz   = ImGui::CalcTextSize(text);
    float    cx    = tsz.x * 0.5f;   // half text-width  → vertical offset
    float    cy    = tsz.y * 0.5f;   // half text-height → horizontal offset

    float cur_x = 0.0f;
    for (const char* s = text; *s; ++s) {
        const ImFontGlyph* g = font->FindGlyph((ImWchar)(unsigned char)*s);
        if (!g) continue;
        if (g->Visible) {
            float x0 = cur_x + g->X0 * scale,  y0 = g->Y0 * scale;
            float x1 = cur_x + g->X1 * scale,  y1 = g->Y1 * scale;
            // 90° CCW in screen-space: (lx,ly) → (+ly - cy, cx - lx) + center
            ImVec2 p1 = { center.x + y0 - cy, center.y + cx - x0 };
            ImVec2 p2 = { center.x + y0 - cy, center.y + cx - x1 };
            ImVec2 p3 = { center.x + y1 - cy, center.y + cx - x1 };
            ImVec2 p4 = { center.x + y1 - cy, center.y + cx - x0 };
            dl->AddImageQuad(ImGui::GetIO().Fonts->TexID,
                p1, p2, p3, p4,
                { g->U0, g->V0 }, { g->U1, g->V0 },
                { g->U1, g->V1 }, { g->U0, g->V1 },
                col);
        }
        cur_x += g->AdvanceX * scale;
    }
}

void draw_panel_edge_tabs() {
    if (g_input.panel_open()) return;
    if (g_documents.empty()) return;

    ImVec2 vp      = ImGui::GetMainViewport()->Size;
    float  lh      = ImGui::GetTextLineHeight();
    // When rotated 90°, text width becomes the tab height and text height
    // becomes the tab width.  Add padding in both axes.
    const float PAD_H   = 10.0f;   // left/right padding → adds to STRIP_W
    const float PAD_V   = 16.0f;   // top/bottom padding → adds to tab height
    const float TAB_GAP =  6.0f;
    float STRIP_W  = lh + PAD_H * 2.0f;
    float viewer_h = ImGui::CalcTextSize("Viewer").x     + PAD_V * 2.0f;
    float refs_h   = ImGui::CalcTextSize("References").x + PAD_V * 2.0f;
    float total_h  = viewer_h + TAB_GAP + refs_h;
    float origin_y = g_toolbar_bottom + 4.0f;

    ImGui::SetNextWindowPos({vp.x - STRIP_W, origin_y}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({STRIP_W, total_h}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0f, 1.0f});
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar          |
        ImGuiWindowFlags_NoResize            |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoScrollbar         |
        ImGuiWindowFlags_NoSavedSettings     |
        ImGuiWindowFlags_NoBackground        |
        ImGuiWindowFlags_NoFocusOnAppearing  |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##edge_tabs", nullptr, kFlags);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();

    // --- "Viewer" tab ---
    ImGui::InvisibleButton("##etab_viewer", {STRIP_W, viewer_h});
    bool v_hov = ImGui::IsItemHovered();
    bool v_act = ImGui::IsItemActive();
    if (ImGui::IsItemClicked()) {
        int di = (s_last_panel_doc >= 0 && s_last_panel_doc < (int)g_documents.size())
                 ? s_last_panel_doc : 0;
        s_panel_open_to_refs = false;
        g_input.open_panel(di, -1);
    }
    ImU32 v_bg = v_act  ? IM_COL32(70, 130, 225, 240)
               : v_hov  ? IM_COL32(50, 108, 200, 220)
               :          IM_COL32(35,  85, 165, 190);
    dl->AddRectFilled({wp.x, wp.y}, {wp.x + STRIP_W, wp.y + viewer_h},
                      v_bg, 5.0f, ImDrawFlags_RoundCornersLeft);
    draw_text_ccw(dl, {wp.x + STRIP_W * 0.5f, wp.y + viewer_h * 0.5f},
                  "Viewer", IM_COL32(210, 225, 255, 245));

    // Gap between tabs
    ImGui::Dummy({STRIP_W, TAB_GAP});

    // --- "References" tab ---
    float refs_y = wp.y + viewer_h + TAB_GAP;
    ImGui::InvisibleButton("##etab_refs", {STRIP_W, refs_h});
    bool r_hov = ImGui::IsItemHovered();
    bool r_act = ImGui::IsItemActive();
    if (ImGui::IsItemClicked()) {
        int di = (s_last_panel_doc >= 0 && s_last_panel_doc < (int)g_documents.size())
                 ? s_last_panel_doc : 0;
        s_panel_open_to_refs = true;
        g_input.open_panel(di, -1);
    }
    ImU32 r_bg = r_act  ? IM_COL32(70, 130, 225, 240)
               : r_hov  ? IM_COL32(50, 108, 200, 220)
               :          IM_COL32(35,  85, 165, 190);
    dl->AddRectFilled({wp.x, refs_y}, {wp.x + STRIP_W, refs_y + refs_h},
                      r_bg, 5.0f, ImDrawFlags_RoundCornersLeft);
    draw_text_ccw(dl, {wp.x + STRIP_W * 0.5f, refs_y + refs_h * 0.5f},
                  "References", IM_COL32(210, 225, 255, 245));

    ImGui::End();
}
