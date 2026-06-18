#include "canvas_annot.h"
#include "app_state.h"
#include "undo.h"

#include "imgui.h"

#include <algorithm>
#include <climits>
#include <string>
#include <vector>

// --- Annotation state globals (extern in canvas_annot.h) --------------------

AnnotTool   g_annot_tool    = AnnotTool::None;
bool        g_ann_drawing   = false;
float       g_pen_r = 0.82f, g_pen_g = 0.06f, g_pen_b = 0.06f;
int         g_ann_doc_idx   = -1;
int         g_ann_page_idx  = -1;
AnnotStroke g_ann_cur_stroke;
Vec2        g_ann_hl_start  = {};
Vec2        g_ann_cur_norm  = {};
int         g_next_note_idx = 0;
std::vector<RefNote> g_ref_notes;

// --- Helpers -----------------------------------------------------------------

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
    return {
        std::clamp((w.x - page.world_pos.x) / page.world_w, 0.0f, 1.0f),
        std::clamp((w.y - page.world_pos.y) / page.world_h, 0.0f, 1.0f)
    };
}

// --- Annotation finalization -------------------------------------------------

void finalize_annotation() {
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
        AnnotHighlight hl;
        hl.x0 = std::min(g_ann_hl_start.x, g_ann_cur_norm.x);
        hl.y0 = std::min(g_ann_hl_start.y, g_ann_cur_norm.y);
        hl.x1 = std::max(g_ann_hl_start.x, g_ann_cur_norm.x);
        hl.y1 = std::max(g_ann_hl_start.y, g_ann_cur_norm.y);
        if (hl.x1 > hl.x0 && hl.y1 > hl.y0) {
            auto* loader = (g_ann_doc_idx >= 0 && g_ann_doc_idx < (int)g_loaders.size())
                           ? g_loaders[g_ann_doc_idx].get() : nullptr;
            if (loader && loader->valid()) {
                const auto& quads = loader->get_char_quads(g_ann_page_idx);
                std::vector<const CharQuad*> sel;
                sel.reserve(quads.size());
                for (const auto& q : quads) {
                    float cx = (q.x0 + q.x1) * 0.5f;
                    float cy = (q.y0 + q.y1) * 0.5f;
                    if (cx >= hl.x0 && cx <= hl.x1 && cy >= hl.y0 && cy <= hl.y1)
                        sel.push_back(&q);
                }
                if (!sel.empty()) {
                    std::sort(sel.begin(), sel.end(),
                              [](const CharQuad* a, const CharQuad* b){
                                  return a->order < b->order; });
                    float sx0 = sel[0]->x0, sy0 = sel[0]->y0;
                    float sx1 = sel[0]->x1, sy1 = sel[0]->y1;
                    std::string text;
                    for (const auto* q : sel) {
                        sx0 = std::min(sx0, q->x0); sy0 = std::min(sy0, q->y0);
                        sx1 = std::max(sx1, q->x1); sy1 = std::max(sy1, q->y1);
                        text += q->utf8;
                        if (q->line_end) text += ' ';
                    }
                    while (!text.empty() && text.back() == ' ') text.pop_back();
                    hl.x0 = sx0; hl.y0 = sy0; hl.x1 = sx1; hl.y1 = sy1;
                    hl.text = std::move(text);
                }
            }
            fpage.annots.highlights.push_back(hl);
            UndoRecord r;
            r.type     = UndoRecord::Type::Highlight;
            r.doc_idx  = g_ann_doc_idx;
            r.page_idx = g_ann_page_idx;
            push_undo(r);
        }
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
        float x0 = std::min(g_ann_hl_start.x, g_ann_cur_norm.x);
        float y0 = std::min(g_ann_hl_start.y, g_ann_cur_norm.y);
        float x1 = std::max(g_ann_hl_start.x, g_ann_cur_norm.x);
        float y1 = std::max(g_ann_hl_start.y, g_ann_cur_norm.y);
        Vec2 tlw = {page->world_pos.x + x0 * page->world_w, page->world_pos.y + y0 * page->world_h};
        Vec2 brw = {page->world_pos.x + x1 * page->world_w, page->world_pos.y + y1 * page->world_h};
        Vec2 tls = g_canvas.world_to_screen(tlw);
        Vec2 brs = g_canvas.world_to_screen(brw);
        dl->AddRectFilled({tls.x, tls.y}, {brs.x, brs.y}, IM_COL32(255, 224, 0, 80));
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
