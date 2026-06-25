#include "pdf_loader.h"

#ifdef SCHOLION_HAVE_MUPDF

#include <mupdf/fitz.h>
#include <cstdio>
#include <cstring>


void PdfLoader::set_content_scale(float s) {
    // Cap at 1.5× — enough to match physical pixels on 2× Retina at normal reading
    // zoom without the 4× VRAM cost of a full 2× DPI multiplier.
    m_content_scale = (s > 1.5f) ? 1.5f : (s < 1.0f ? 1.0f : s);
}

PdfLoader::PdfLoader() {
    // 96 MB cap per loader (one per document). MuPDF's LRU evicts glyph/image
    // cache under pressure rather than growing without bound, preventing the
    // runaway RAM growth that causes unexpected quits on long sessions.
    m_ctx = fz_new_context(nullptr, nullptr, 96ULL * 1024 * 1024);
    if (m_ctx)
        fz_register_document_handlers(m_ctx);
    else
        fprintf(stderr, "PdfLoader: failed to create MuPDF context\n");
}

PdfLoader::~PdfLoader() {
    if (m_doc) fz_drop_document(m_ctx, m_doc);
    if (m_ctx) fz_drop_context(m_ctx);
}

bool PdfLoader::load(const std::string& path, Document& doc, Vec2 world_origin) {
    if (!m_ctx) return false;

    if (m_doc) {
        fz_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }

    fz_try(m_ctx) {
        m_doc = fz_open_document(m_ctx, path.c_str());
    }
    fz_catch(m_ctx) {
        fprintf(stderr, "PdfLoader: cannot open '%s': %s\n",
                path.c_str(), fz_caught_message(m_ctx));
        return false;
    }

    doc.path         = path;
    doc.stack_origin = world_origin;
    doc.pages.clear();

    int count = 0;
    fz_try(m_ctx)  { count = fz_count_pages(m_ctx, m_doc); }
    fz_catch(m_ctx){ return false; }

    for (int i = 0; i < count; ++i) {
        fz_page* fz_pg = nullptr;
        fz_try(m_ctx)  { fz_pg = fz_load_page(m_ctx, m_doc, i); }
        fz_catch(m_ctx){ continue; }

        fz_rect bounds = fz_bound_page(m_ctx, fz_pg);
        fz_drop_page(m_ctx, fz_pg);

        float w = bounds.x1 - bounds.x0;   // in PDF points = world units
        float h = bounds.y1 - bounds.y0;

        // Fan: page i offset diagonally so all pages overlap with a peek strip.
        // Page n-1 sits at the front (highest fan index, drawn last = on top).
        float fan = static_cast<float>(i) * PAGE_FAN_OFFSET;
        Page page;
        page.page_index = i;
        page.world_pos  = {world_origin.x + fan, world_origin.y + fan};
        page.world_w    = w;
        page.world_h    = h;
        doc.pages.push_back(page);
    }

    return !doc.pages.empty();
}

// Three LOD tiers and their approximate VRAM cost per page (RGBA, no alpha plane):
//   Thumb (72 DPI):   ~1.1 MB/page — loaded at startup; never evicted
//   Low  (150 DPI):   ~4.3 MB/page — normal reading zoom (0.5–2.5)
//   High (300 DPI):  ~17   MB/page — close reading zoom (≥ 2.5)
// DPI values chosen so the lowest visible tier always renders crisply at its
// intended zoom range on a typical 96–110 DPI display.
// rasterize_to_buffer() is safe to call from the background worker thread;
// rasterize_and_upload() must be called from the GL (main) thread.
bool PdfLoader::rasterize_and_upload(Page& page, LodTier tier, TextureCache& cache) {
    if (!m_ctx || !m_doc) return false;
    if (!page.needs_lod(tier)) return true;   // already on GPU

    float dpi   = dpi_for_lod(tier) * m_content_scale;
    float scale = dpi / 72.0f;

    fz_page* fz_pg = nullptr;
    fz_try(m_ctx)  { fz_pg = fz_load_page(m_ctx, m_doc, page.page_index); }
    fz_catch(m_ctx){ return false; }

    fz_matrix matrix = fz_scale(scale, scale);
    fz_rect   bounds = fz_bound_page(m_ctx, fz_pg);
    fz_irect  bbox   = fz_round_rect(fz_transform_rect(bounds, matrix));

    fz_pixmap* pix = nullptr;
    bool ok = false;

    fz_try(m_ctx) {
        // RGBA pixmap with white background
        pix = fz_new_pixmap_with_bbox(m_ctx, fz_device_rgb(m_ctx), bbox, nullptr, 1);
        fz_clear_pixmap_with_value(m_ctx, pix, 0xff);

        fz_device* dev = fz_new_draw_device(m_ctx, matrix, pix);
        fz_run_page(m_ctx, fz_pg, dev, fz_identity, nullptr);
        fz_close_device(m_ctx, dev);
        fz_drop_device(m_ctx, dev);

        cache.upload(page, tier,
                     fz_pixmap_samples(m_ctx, pix),
                     fz_pixmap_width(m_ctx,   pix),
                     fz_pixmap_height(m_ctx,  pix),
                     fz_pixmap_stride(m_ctx,  pix));
        ok = true;
    }
    fz_catch(m_ctx) {
        fprintf(stderr, "PdfLoader: rasterize failed for page %d: %s\n",
                page.page_index, fz_caught_message(m_ctx));
    }

    if (pix)   fz_drop_pixmap(m_ctx, pix);
    fz_drop_page(m_ctx, fz_pg);
    return ok;
}

void PdfLoader::rasterize_all(Document& doc, LodTier tier, TextureCache& cache) {
    for (auto& page : doc.pages)
        rasterize_and_upload(page, tier, cache);
}

PdfLoader::RasterBuffer PdfLoader::rasterize_to_buffer(int page_index, LodTier tier,
                                                         int tile_col, int tile_row,
                                                         int tile_size) {
    RasterBuffer result;
    if (!m_ctx || !m_doc) return result;

    float dpi   = dpi_for_lod(tier) * m_content_scale;
    float scale = dpi / 72.0f;

    fz_page* fz_pg = nullptr;
    fz_try(m_ctx)  { fz_pg = fz_load_page(m_ctx, m_doc, page_index); }
    fz_catch(m_ctx){ return result; }

    fz_matrix matrix = fz_scale(scale, scale);
    fz_rect   bounds = fz_bound_page(m_ctx, fz_pg);
    fz_irect  bbox   = fz_round_rect(fz_transform_rect(bounds, matrix));

    // Clip to a single tile when rendering High-tier tiles.
    // tile_col/row index into page-raster pixel space (origin = page top-left).
    if (tile_col >= 0 && tile_row >= 0) {
        fz_irect tile_box;
        tile_box.x0 = bbox.x0 + tile_col * tile_size;
        tile_box.y0 = bbox.y0 + tile_row * tile_size;
        tile_box.x1 = (tile_box.x0 + tile_size < bbox.x1) ? tile_box.x0 + tile_size : bbox.x1;
        tile_box.y1 = (tile_box.y0 + tile_size < bbox.y1) ? tile_box.y0 + tile_size : bbox.y1;
        if (tile_box.x1 > tile_box.x0 && tile_box.y1 > tile_box.y0)
            bbox = tile_box;
        else { fz_drop_page(m_ctx, fz_pg); return result; }
    }

    fz_pixmap* pix = nullptr;
    fz_try(m_ctx) {
        pix = fz_new_pixmap_with_bbox(m_ctx, fz_device_rgb(m_ctx), bbox, nullptr, 1);
        fz_clear_pixmap_with_value(m_ctx, pix, 0xff);
        fz_device* dev = fz_new_draw_device(m_ctx, matrix, pix);
        fz_run_page(m_ctx, fz_pg, dev, fz_identity, nullptr);
        fz_close_device(m_ctx, dev);
        fz_drop_device(m_ctx, dev);
        int w      = fz_pixmap_width(m_ctx, pix);
        int h      = fz_pixmap_height(m_ctx, pix);
        int stride = fz_pixmap_stride(m_ctx, pix);
        const uint8_t* s = fz_pixmap_samples(m_ctx, pix);
        // Copy row-by-row to produce a packed (width*4) buffer — pixmap rows
        // may have alignment padding (stride > w*4) on some platforms/builds.
        size_t row_bytes = (size_t)w * 4;
        result.pixels.resize(row_bytes * h);
        for (int row = 0; row < h; ++row)
            std::memcpy(result.pixels.data() + row * row_bytes,
                        s + (size_t)row * stride, row_bytes);
        result.width  = w;
        result.height = h;
        result.ok     = true;
    }
    fz_catch(m_ctx) {
        fprintf(stderr, "PdfLoader: rasterize_to_buffer failed for page %d: %s\n",
                page_index, fz_caught_message(m_ctx));
    }
    if (pix)   fz_drop_pixmap(m_ctx, pix);
    fz_drop_page(m_ctx, fz_pg);
    return result;
}

// ---------------------------------------------------------------------------
// Append a single Unicode code point as UTF-8 to s.
static void append_utf8(std::string& s, int c) {
    if      (c < 0x80)    { s += (char)c; }
    else if (c < 0x800)   { s += (char)(0xC0|(c>>6));  s += (char)(0x80|(c&0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0|(c>>12)); s += (char)(0x80|((c>>6)&0x3F)); s += (char)(0x80|(c&0x3F)); }
    else                  { s += (char)(0xF0|(c>>18)); s += (char)(0x80|((c>>12)&0x3F)); s += (char)(0x80|((c>>6)&0x3F)); s += (char)(0x80|(c&0x3F)); }
}

std::string PdfLoader::extract_text(int page_index) {
    if (!m_ctx || !m_doc) return {};

    fz_page* pg = nullptr;
    fz_try(m_ctx)  { pg = fz_load_page(m_ctx, m_doc, page_index); }
    fz_catch(m_ctx){ return {}; }

    std::string result;
    fz_stext_page*   stext = nullptr;
    fz_stext_options opts  = {};

    fz_try(m_ctx) {
        stext = fz_new_stext_page_from_page(m_ctx, pg, &opts);
        for (fz_stext_block* blk = stext->first_block; blk; blk = blk->next) {
            if (blk->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = blk->u.t.first_line; ln; ln = ln->next) {
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    append_utf8(result, ch->c);
                result += '\n';
            }
        }
    }
    fz_catch(m_ctx) {
        fprintf(stderr, "PdfLoader::extract_text failed for page %d: %s\n",
                page_index, fz_caught_message(m_ctx));
    }

    if (stext) fz_drop_stext_page(m_ctx, stext);
    fz_drop_page(m_ctx, pg);
    return result;
}

const std::vector<CharQuad>& PdfLoader::get_char_quads(int page_index) {
    auto it = m_char_cache.find(page_index);
    if (it != m_char_cache.end()) return it->second;

    auto& result = m_char_cache[page_index];
    if (!m_ctx || !m_doc) return result;

    fz_page* pg = nullptr;
    fz_try(m_ctx)  { pg = fz_load_page(m_ctx, m_doc, page_index); }
    fz_catch(m_ctx){ return result; }

    fz_rect bounds = fz_bound_page(m_ctx, pg);
    float ox = bounds.x0, oy = bounds.y0;
    float pw = bounds.x1 - bounds.x0;
    float ph = bounds.y1 - bounds.y0;

    if (pw <= 0.0f || ph <= 0.0f) {
        fz_drop_page(m_ctx, pg);
        return result;
    }

    fz_stext_options opts = {};
    fz_stext_page*   stext = nullptr;

    fz_try(m_ctx) {
        stext = fz_new_stext_page_from_page(m_ctx, pg, &opts);
        int block_n = 0;
        for (fz_stext_block* blk = stext->first_block; blk; blk = blk->next) {
            if (blk->type != FZ_STEXT_BLOCK_TEXT) { ++block_n; continue; }
            int line_n = 0;
            for (fz_stext_line* ln = blk->u.t.first_line; ln; ln = ln->next, ++line_n) {
                int char_n = 0;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next, ++char_n) {
                    fz_rect r = fz_rect_from_quad(ch->quad);
                    CharQuad q;
                    q.x0 = (r.x0 - ox) / pw;
                    q.y0 = (r.y0 - oy) / ph;
                    q.x1 = (r.x1 - ox) / pw;
                    q.y1 = (r.y1 - oy) / ph;
                    q.order    = block_n * 100000 + line_n * 1000 + char_n;
                    q.line_end = (ch->next == nullptr);
                    append_utf8(q.utf8, ch->c);
                    result.push_back(std::move(q));
                }
            }
            ++block_n;
        }
    }
    fz_catch(m_ctx) {
        fprintf(stderr, "PdfLoader::get_char_quads failed for page %d: %s\n",
                page_index, fz_caught_message(m_ctx));
    }

    if (stext) fz_drop_stext_page(m_ctx, stext);
    fz_drop_page(m_ctx, pg);
    return result;
}

std::vector<PdfLoader::SearchHit> PdfLoader::search_text(const std::string& query, int max_hits) {
    std::vector<SearchHit> hits;
    if (!m_ctx || !m_doc || query.empty()) return hits;

    std::string ql = query;
    for (char& c : ql) c = (char)std::tolower((unsigned char)c);

    int n = 0;
    fz_try(m_ctx)  { n = fz_count_pages(m_ctx, m_doc); }
    fz_catch(m_ctx){ return hits; }

    for (int pi = 0; pi < n && (int)hits.size() < max_hits; ++pi) {
        std::string text = extract_text(pi);
        if (text.empty()) continue;

        std::string tl = text;
        for (char& c : tl) c = (char)std::tolower((unsigned char)c);

        if (tl.find(ql) == std::string::npos) continue;

        // Get MuPDF hit quads for every occurrence on this page and normalize to [0,1].
        std::vector<std::array<float,4>> page_rects;
        fz_page* pg = nullptr;
        fz_try(m_ctx) { pg = fz_load_page(m_ctx, m_doc, pi); } fz_catch(m_ctx) {}
        if (pg) {
            fz_rect bounds = fz_bound_page(m_ctx, pg);
            float pw = bounds.x1 - bounds.x0;
            float ph = bounds.y1 - bounds.y0;
            if (pw > 0.0f && ph > 0.0f) {
                static constexpr int MAX_QUADS = 256;
                fz_quad quads[MAX_QUADS];
                int     marks[MAX_QUADS];
                int nq = 0;
                fz_try(m_ctx) {
                    nq = fz_search_page(m_ctx, pg, query.c_str(), marks, quads, MAX_QUADS);
                } fz_catch(m_ctx) { nq = 0; }
                page_rects.reserve(nq);
                for (int h = 0; h < nq; ++h) {
                    fz_rect r = fz_rect_from_quad(quads[h]);
                    page_rects.push_back({
                        (r.x0 - bounds.x0) / pw,
                        (r.y0 - bounds.y0) / ph,
                        (r.x1 - bounds.x0) / pw,
                        (r.y1 - bounds.y0) / ph
                    });
                }
            }
            fz_drop_page(m_ctx, pg);
        }

        size_t pos = 0;
        while (pos < tl.size() && (int)hits.size() < max_hits) {
            size_t found = tl.find(ql, pos);
            if (found == std::string::npos) break;

            // ~50 chars of context on each side
            size_t beg = (found >= 50) ? found - 50 : 0;
            size_t end = std::min(found + ql.size() + 50, text.size());
            std::string excerpt = text.substr(beg, end - beg);
            for (char& c : excerpt) if (c == '\n' || c == '\r') c = ' ';
            while (!excerpt.empty() && excerpt.front() == ' ') excerpt.erase(0, 1);
            while (!excerpt.empty() && excerpt.back()  == ' ') excerpt.pop_back();
            if (beg > 0)           excerpt = "..." + excerpt;
            if (end < text.size()) excerpt += "...";

            hits.push_back({pi, std::move(excerpt), page_rects});
            pos = found + ql.size();
        }
    }
    return hits;
}

#else  // !SCHOLION_HAVE_MUPDF

// Stub so the TU compiles without MuPDF; all methods return false/no-op.
#include <cstdio>
PdfLoader::PdfLoader()  { fprintf(stderr, "PdfLoader: built without MuPDF support\n"); }
PdfLoader::~PdfLoader() {}
bool PdfLoader::load(const std::string&, Document&, Vec2)                     { return false; }
bool PdfLoader::rasterize_and_upload(Page&, LodTier, TextureCache&)           { return false; }
void PdfLoader::rasterize_all(Document&, LodTier, TextureCache&)              {}
PdfLoader::RasterBuffer PdfLoader::rasterize_to_buffer(int, LodTier, int, int, int) { return {}; }
std::string PdfLoader::extract_text(int)                                      { return {}; }
std::vector<PdfLoader::SearchHit> PdfLoader::search_text(const std::string&, int) { return {}; }
const std::vector<CharQuad>& PdfLoader::get_char_quads(int)                  { static std::vector<CharQuad> empty; return empty; }

#endif // SCHOLION_HAVE_MUPDF
