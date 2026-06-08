#pragma once

#include "document.h"
#include "texture_cache.h"
#include <string>
#include <unordered_map>
#include <vector>

// fz_context / fz_document forward-declared to keep MuPDF out of this header.
struct fz_context;
struct fz_document;

/// One character's bounding box in page-normalized [0,1] coordinates, plus
/// its UTF-8 text and position-order key for sorting.
struct CharQuad {
    float x0 = 0.0f, y0 = 0.0f;  // tight bbox, normalized [0,1]
    float x1 = 0.0f, y1 = 0.0f;
    std::string utf8;              // UTF-8 encoded glyph
    int  order    = 0;             // block*100000 + line*1000 + char — for sorting
    bool line_end = false;         // true for the last char on a text line
};

/// Opens a PDF with MuPDF, maps its pages into world-space geometry, and
/// rasterizes them to GPU textures via TextureCache.
///
/// World-space convention: 1 world unit = 1 PDF point (1/72 inch).
/// Pages are arranged as a fanned stack: each page i is offset by
/// (i * FAN_OFFSET, i * FAN_OFFSET) from world_origin so they overlap with a
/// diagonal peek, page n-1 rendered on top.
class PdfLoader {
public:
    PdfLoader();
    ~PdfLoader();

    PdfLoader(const PdfLoader&)            = delete;
    PdfLoader& operator=(const PdfLoader&) = delete;

    /// Open path and populate doc with page geometry (no rasterization yet).
    /// world_origin is the top-left corner of the first page in world space.
    bool load(const std::string& path, Document& doc,
              Vec2 world_origin = {0.0f, 0.0f});

    /// Rasterize one page at the given LOD and upload it to the GPU.
    /// No-ops if the texture for that tier is already uploaded.
    bool rasterize_and_upload(Page& page, LodTier tier, TextureCache& cache);

    /// Convenience: rasterize every page in doc at the given tier.
    void rasterize_all(Document& doc, LodTier tier, TextureCache& cache);

    bool valid() const { return m_ctx != nullptr; }

    /// Rasterize one page to a CPU pixel buffer — no GL calls.
    /// Safe to call from a background thread. Returns ok=false on failure.
    struct RasterBuffer {
        std::vector<uint8_t> pixels;
        int width  = 0;
        int height = 0;
        bool ok    = false;
    };
    RasterBuffer rasterize_to_buffer(int page_index, LodTier tier);

    /// Extract all text from one page as a UTF-8 string (newlines at line ends).
    /// Not thread-safe against concurrent rasterization on the same loader.
    std::string extract_text(int page_index);

    /// Case-insensitive search across all pages. Returns up to max_hits results.
    struct SearchHit {
        int         page_index;
        std::string excerpt;    // ~100-char context around the first match on the page
    };
    std::vector<SearchHit> search_text(const std::string& query, int max_hits = 200);

    /// Return cached character quads for a page (populated lazily on first call).
    /// Quads are in normalized [0,1] page coordinates with y=0 at the top.
    /// Returns an empty vector for pages with no selectable text.
    const std::vector<CharQuad>& get_char_quads(int page_index);

private:
    fz_context*  m_ctx = nullptr;
    fz_document* m_doc = nullptr;

    std::unordered_map<int, std::vector<CharQuad>> m_char_cache;
};
