#include "canvas_annot.h"
#include "app_state.h"
#include "undo.h"

#include "imgui.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

// --- Annotation state globals (extern in canvas_annot.h) --------------------

AnnotTool   g_annot_tool    = AnnotTool::None;
bool        g_ann_drawing   = false;
float       g_pen_r = 0.82f, g_pen_g = 0.06f, g_pen_b = 0.06f;
float       g_hl_r = 1.0f, g_hl_g = 0.85f, g_hl_b = 0.0f;   // highlighter yellow
bool        g_hl_box_mode = true;   // true = rectangle drag (reliable glyph capture); false = swipe
int         g_ann_doc_idx   = -1;
int         g_ann_page_idx  = -1;
AnnotStroke g_ann_cur_stroke;
Vec2        g_ann_hl_start  = {};
Vec2        g_ann_cur_norm  = {};
int         g_next_note_idx = 0;
std::vector<AnnotStroke> g_canvas_strokes;
bool        g_ann_canvas = false;

// --- Helpers -----------------------------------------------------------------

void stroke_add_point(const Page& page, Vec2 norm, bool ortho) {
    if (ortho && !g_ann_cur_stroke.pts.empty()) {
        // Shift ortho-lock: collapse the stroke to a straight segment from its start to the
        // cursor, snapped to the nearest 45°. Snap in world-aspect space (delta scaled by the
        // page's world size) so diagonals are visually 45° on non-square pages, then convert
        // the snapped endpoint back to page-normalized coords.
        Vec2  s  = g_ann_cur_stroke.pts.front();
        float ww = (page.world_w > 1e-3f) ? page.world_w : 1.0f;
        float wh = (page.world_h > 1e-3f) ? page.world_h : 1.0f;
        float dwx = (norm.x - s.x) * ww, dwy = (norm.y - s.y) * wh;
        float len = std::sqrt(dwx * dwx + dwy * dwy);
        constexpr float STEP = 0.78539816339f;  // 45° in radians
        float ang = std::round(std::atan2(dwy, dwx) / STEP) * STEP;
        Vec2 end = { s.x + std::cos(ang) * len / ww,
                     s.y + std::sin(ang) * len / wh };
        g_ann_cur_stroke.pts.resize(1);          // keep only the start point
        g_ann_cur_stroke.pts.push_back(end);
    } else {
        g_ann_cur_stroke.pts.push_back(norm);
    }
}

void stroke_add_point_world(Vec2 world, bool ortho) {
    // Canvas strokes live in world space (uniform aspect), so ortho snaps directly.
    if (ortho && !g_ann_cur_stroke.pts.empty()) {
        Vec2 s = g_ann_cur_stroke.pts.front();
        float dx = world.x - s.x, dy = world.y - s.y;
        float len = std::sqrt(dx * dx + dy * dy);
        constexpr float STEP = 0.78539816339f;  // 45°
        float ang = std::round(std::atan2(dy, dx) / STEP) * STEP;
        g_ann_cur_stroke.pts.resize(1);
        g_ann_cur_stroke.pts.push_back({ s.x + std::cos(ang) * len, s.y + std::sin(ang) * len });
    } else {
        g_ann_cur_stroke.pts.push_back(world);
    }
}

std::string note_label(int idx) {
    int  prefix = idx / 26;
    char letter  = 'A' + (idx % 26);
    if (prefix == 0) return std::string(1, letter);
    return std::to_string(prefix + 1) + letter;
}

Page* hit_test_page(float sx, float sy, int* out_doc_idx) {
    Vec2 world = g_canvas.screen_to_world({sx, sy});
    int   best_z  = INT_MIN;
    Page* best    = nullptr;
    int   best_di = -1;
    for (int di = 0; di < (int)g_documents.size(); ++di) {
        const Document& doc = g_documents[di];
        if (doc.missing || doc.z_layer <= best_z) continue;
        for (auto& page : const_cast<Document&>(doc).pages) {
            if (world.x >= page.world_pos.x &&
                world.x <= page.world_pos.x + page.world_w &&
                world.y >= page.world_pos.y &&
                world.y <= page.world_pos.y + page.world_h) {
                best_z = doc.z_layer; best = &page; best_di = di;
                break;
            }
        }
    }
    if (out_doc_idx) *out_doc_idx = best_di;
    return best;
}

Vec2 screen_to_page_norm(const Page& page, float sx, float sy) {
    Vec2 w = g_canvas.screen_to_world({sx, sy});
    float nx = std::clamp((w.x - page.world_pos.x) / page.world_w, 0.0f, 1.0f);
    float ny = std::clamp((w.y - page.world_pos.y) / page.world_h, 0.0f, 1.0f);
    // Invert the rotation applied by renderer's to_world() so stored annotation
    // coords are always in PDF-native normalized space, not screen-fraction space.
    switch (page.rotation) {
        case 90:  return {ny,        1.0f - nx};
        case 180: return {1.0f - nx, 1.0f - ny};
        case 270: return {1.0f - ny, nx       };
        default:  return {nx,        ny       };
    }
}

// --- Annotation finalization -------------------------------------------------

void finalize_annotation() {
    // Canvas (world-space) pen stroke: commit to the global list, no page involved.
    if (g_ann_canvas) {
        if (g_ann_cur_stroke.pts.size() >= 2) {
            g_canvas_strokes.push_back(std::move(g_ann_cur_stroke));
            UndoRecord r; r.type = UndoRecord::Type::CanvasStroke;
            push_undo(r);
        }
        g_ann_cur_stroke = {};
        g_ann_canvas = false;
        g_ann_drawing = false;
        return;
    }
    if (g_ann_doc_idx < 0 || g_ann_doc_idx >= (int)g_documents.size()) {
        g_ann_drawing = false; return;
    }
    Document& fdoc = g_documents[g_ann_doc_idx];
    Page* fpage_ptr = nullptr;
    for (auto& pg : fdoc.pages)
        if (pg.page_index == g_ann_page_idx) { fpage_ptr = &pg; break; }
    if (!fpage_ptr) { g_ann_drawing = false; return; }
    Page& fpage = *fpage_ptr;
    if (g_annot_tool == AnnotTool::Pen) {
        if (g_ann_cur_stroke.pts.size() >= 2) {
            fpage.annots.strokes.push_back(std::move(g_ann_cur_stroke));
            UndoRecord r;
            r.type     = UndoRecord::Type::PenStroke;
            r.doc_idx  = g_ann_doc_idx;
            r.page_idx = g_ann_page_idx;
            push_undo(r);
        }
        g_ann_cur_stroke = {};
    } else if (g_annot_tool == AnnotTool::Highlight) {
        // Two modes (g_hl_box_mode): Box drags a rectangle; Freehand swipes a marker line.
        // BOTH capture glyphs by center-in-AABB (reliable) → a glyph-locked highlight fed to
        // References. When no glyphs are covered, Box leaves a plain highlight rectangle and
        // Freehand leaves a persistent translucent marker stroke.
        const auto& pts = g_ann_cur_stroke.pts;
        float ax0, ay0, ax1, ay1;
        bool  have_region = false;
        if (g_hl_box_mode) {
            ax0 = std::min(g_ann_hl_start.x, g_ann_cur_norm.x);
            ay0 = std::min(g_ann_hl_start.y, g_ann_cur_norm.y);
            ax1 = std::max(g_ann_hl_start.x, g_ann_cur_norm.x);
            ay1 = std::max(g_ann_hl_start.y, g_ann_cur_norm.y);
            have_region = (ax1 > ax0 && ay1 > ay0);
        } else if (pts.size() >= 2) {
            ax0 = ax1 = pts[0].x; ay0 = ay1 = pts[0].y;
            for (const auto& p : pts) {
                ax0 = std::min(ax0, p.x); ay0 = std::min(ay0, p.y);
                ax1 = std::max(ax1, p.x); ay1 = std::max(ay1, p.y);
            }
            have_region = true;
        }

        if (have_region) {
            // Glyph capture (both modes): center-in-AABB — forgiving, reliable for text.
            std::vector<const CharQuad*> sel;
            auto* loader = (g_ann_doc_idx >= 0 && g_ann_doc_idx < (int)g_loaders.size())
                           ? g_loaders[g_ann_doc_idx].get() : nullptr;
            if (loader && loader->valid()) {
                const auto& quads = loader->get_char_quads(g_ann_page_idx);
                sel.reserve(quads.size());
                for (const auto& q : quads) {
                    float cx = (q.x0 + q.x1) * 0.5f, cy = (q.y0 + q.y1) * 0.5f;
                    if (cx >= ax0 && cx <= ax1 && cy >= ay0 && cy <= ay1)
                        sel.push_back(&q);
                }
            }
            if (!sel.empty()) {
                // Text: clean glyph-locked highlight + extracted text (→ References).
                std::sort(sel.begin(), sel.end(),
                          [](const CharQuad* a, const CharQuad* b){ return a->order < b->order; });
                AnnotHighlight hl;
                hl.x0 = sel[0]->x0; hl.y0 = sel[0]->y0; hl.x1 = sel[0]->x1; hl.y1 = sel[0]->y1;
                std::string text;
                for (const auto* q : sel) {
                    hl.x0 = std::min(hl.x0, q->x0); hl.y0 = std::min(hl.y0, q->y0);
                    hl.x1 = std::max(hl.x1, q->x1); hl.y1 = std::max(hl.y1, q->y1);
                    text += q->utf8;
                    if (q->line_end) text += '\n';   // preserve the PDF's line structure
                }
                while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
                hl.text = std::move(text);
                fpage.annots.highlights.push_back(hl);
                UndoRecord r;
                r.type = UndoRecord::Type::Highlight;
                r.doc_idx = g_ann_doc_idx; r.page_idx = g_ann_page_idx;
                push_undo(r);
            } else if (g_hl_box_mode) {
                // Box over non-text → plain visual highlight rectangle (no References entry).
                AnnotHighlight hl;
                hl.x0 = ax0; hl.y0 = ay0; hl.x1 = ax1; hl.y1 = ay1;   // text stays empty
                fpage.annots.highlights.push_back(hl);
                UndoRecord r;
                r.type = UndoRecord::Type::Highlight;
                r.doc_idx = g_ann_doc_idx; r.page_idx = g_ann_page_idx;
                push_undo(r);
            } else {
                // Freehand over non-text → persistent translucent marker (rides the stroke path).
                g_ann_cur_stroke.width = MARKER_HALF_W;
                g_ann_cur_stroke.alpha = MARKER_ALPHA;
                fpage.annots.strokes.push_back(std::move(g_ann_cur_stroke));
                UndoRecord r;
                r.type = UndoRecord::Type::PenStroke;
                r.doc_idx = g_ann_doc_idx; r.page_idx = g_ann_page_idx;
                push_undo(r);
            }
        }
        g_ann_cur_stroke = {};
    }
    g_ann_drawing = false;
}

void draw_canvas_annotation_preview() {
    if (!g_ann_drawing) return;
    if (g_ann_doc_idx < 0 || g_ann_doc_idx >= (int)g_documents.size()) return;
    const Document& doc = g_documents[g_ann_doc_idx];
    const Page* page = nullptr;
    for (const auto& pg : doc.pages)
        if (pg.page_index == g_ann_page_idx) { page = &pg; break; }
    if (!page) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (g_annot_tool == AnnotTool::Pen) {
        const auto& pts = g_ann_cur_stroke.pts;
        for (int i = 1; i < (int)pts.size(); ++i) {
            Vec2 aw = {page->world_pos.x + pts[i-1].x * page->world_w,
                       page->world_pos.y + pts[i-1].y * page->world_h};
            Vec2 bw = {page->world_pos.x + pts[i].x   * page->world_w,
                       page->world_pos.y + pts[i].y   * page->world_h};
            Vec2 as = g_canvas.world_to_screen(aw);
            Vec2 bs = g_canvas.world_to_screen(bw);
            dl->AddLine({as.x, as.y}, {bs.x, bs.y},
                IM_COL32((int)(g_pen_r*255),(int)(g_pen_g*255),(int)(g_pen_b*255),220), 2.0f);
        }
    } else if (g_annot_tool == AnnotTool::Highlight) {
        ImU32 col = IM_COL32((int)(g_hl_r*255), (int)(g_hl_g*255), (int)(g_hl_b*255),
                             (int)(MARKER_ALPHA*255));
        if (g_hl_box_mode) {
            // Box preview — yellow rectangle from the drag corners.
            float x0 = std::min(g_ann_hl_start.x, g_ann_cur_norm.x);
            float y0 = std::min(g_ann_hl_start.y, g_ann_cur_norm.y);
            float x1 = std::max(g_ann_hl_start.x, g_ann_cur_norm.x);
            float y1 = std::max(g_ann_hl_start.y, g_ann_cur_norm.y);
            Vec2 tlw = {page->world_pos.x + x0 * page->world_w, page->world_pos.y + y0 * page->world_h};
            Vec2 brw = {page->world_pos.x + x1 * page->world_w, page->world_pos.y + y1 * page->world_h};
            Vec2 tls = g_canvas.world_to_screen(tlw);
            Vec2 brs = g_canvas.world_to_screen(brw);
            dl->AddRectFilled({tls.x, tls.y}, {brs.x, brs.y}, IM_COL32(255, 224, 0, 80));
        } else {
            // Freehand swipe preview — thick translucent line in the highlighter colour.
            const auto& pts = g_ann_cur_stroke.pts;
            for (int i = 1; i < (int)pts.size(); ++i) {
                Vec2 aw = {page->world_pos.x + pts[i-1].x * page->world_w,
                           page->world_pos.y + pts[i-1].y * page->world_h};
                Vec2 bw = {page->world_pos.x + pts[i].x   * page->world_w,
                           page->world_pos.y + pts[i].y   * page->world_h};
                Vec2 as = g_canvas.world_to_screen(aw);
                Vec2 bs = g_canvas.world_to_screen(bw);
                dl->AddLine({as.x, as.y}, {bs.x, bs.y}, col, MARKER_HALF_W * 2.0f);
            }
        }
    }
}

void update_canvas_annotations() {
    if (g_annot_tool == AnnotTool::None) return;
    if (ImGui::GetIO().WantCaptureMouse) return;

    float panel_left = g_input.panel_open()
        ? (ImGui::GetMainViewport()->Size.x - g_panel_w)
        : ImGui::GetMainViewport()->Size.x;
    ImVec2 mouse = ImGui::GetMousePos();
    if (mouse.x >= panel_left) return;

    if (!g_ann_drawing || g_ann_doc_idx < 0 || g_ann_doc_idx >= (int)g_documents.size()) {
        draw_canvas_annotation_preview();
        return;
    }
    Document& doc = g_documents[g_ann_doc_idx];
    Page* page = nullptr;
    for (auto& pg : doc.pages)
        if (pg.page_index == g_ann_page_idx) { page = &pg; break; }
    if (!page) { draw_canvas_annotation_preview(); return; }

    bool active = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!active) { draw_canvas_annotation_preview(); return; }

    Vec2 norm = screen_to_page_norm(*page, mouse.x, mouse.y);

    if (g_annot_tool == AnnotTool::Highlight)
        g_ann_cur_norm = norm;

    if (g_annot_tool == AnnotTool::Eraser) {
        constexpr float ER = 0.025f;
        float nx = norm.x, ny = norm.y;
        for (int hi = (int)page->annots.highlights.size() - 1; hi >= 0; --hi) {
            const auto& hl = page->annots.highlights[hi];
            float cx = std::clamp(nx, hl.x0, hl.x1), cy = std::clamp(ny, hl.y0, hl.y1);
            if ((nx-cx)*(nx-cx) + (ny-cy)*(ny-cy) <= ER*ER) {
                UndoRecord r; r.type = UndoRecord::Type::ErasedHighlight;
                r.doc_idx = g_ann_doc_idx; r.page_idx = page->page_index;
                r.erased_highlight = hl; push_undo(r);
                page->annots.highlights.erase(page->annots.highlights.begin() + hi);
            }
        }
        for (int si = (int)page->annots.strokes.size() - 1; si >= 0; --si) {
            bool hit = false;
            for (const auto& pt : page->annots.strokes[si].pts) {
                float dx = nx - pt.x, dy = ny - pt.y;
                if (dx*dx + dy*dy <= ER*ER) { hit = true; break; }
            }
            if (hit) {
                UndoRecord r; r.type = UndoRecord::Type::ErasedStroke;
                r.doc_idx = g_ann_doc_idx; r.page_idx = page->page_index;
                r.erased_stroke = page->annots.strokes[si]; push_undo(r);
                page->annots.strokes.erase(page->annots.strokes.begin() + si);
            }
        }
    }

    draw_canvas_annotation_preview();
}
