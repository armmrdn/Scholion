#pragma once

#include "canvas.h"
#include <cstdint>
#include <string>
#include <vector>

// --- Tile rendering ---------------------------------------------------------

/// Native pixel size of one High-tier tile.
/// 256 px keeps each tile under 256 KB (256×256×4 = 256 KB RGBA), giving 140
/// tiles per A4/Letter page at 300 DPI. Smaller tiles mean each visible "patch"
/// that streams in is less visually jarring than a 512 px coarse tile.
inline constexpr int TILE_PX = 256;

/// Key for g_tile_cache (main.cpp) and the rasterizer inflight set.
/// Null-byte separators prevent path characters from colliding with field delimiters.
inline std::string tile_cache_key(const std::string& path, int page_idx, int col, int row) {
    return path + '\0' + std::to_string(page_idx)
                + '\0' + std::to_string(col)
                + '\0' + std::to_string(row);
}

// --- LOD tiers --------------------------------------------------------------

/// Rasterization quality tier, chosen by zoom level to avoid wasting VRAM
/// on detail that can't be resolved at the current scale.
///
/// Thresholds (zoom = canvas pixels per world unit):
///   < 0.3  → Thumb  (72 DPI)  — stack overview, many pages on screen
///   < 1.5  → Low   (150 DPI)  — normal reading distance
///   >= 1.5 → High  (300 DPI)  — close reading, individual words legible
enum class LodTier { Thumb, Low, High };

// 1 world unit = 1 PDF point (1/72 inch).
// At zoom 1.0 the canvas renders at 72 DPI. Switch tiers before upscaling
// exceeds ~1.5x so pages stay crisp without wasting VRAM on unused detail.
inline LodTier lod_for_zoom(float zoom) {
    if (zoom < 0.5f) return LodTier::Thumb;  // < 36 DPI — stack overview
    if (zoom < 2.5f) return LodTier::Low;    // 36–180 DPI — normal reading
    return LodTier::High;                     // > 180 DPI — close reading
}

inline float dpi_for_lod(LodTier tier) {
    switch (tier) {
        case LodTier::Thumb: return  72.0f;
        case LodTier::Low:   return 150.0f;
        case LodTier::High:  return 300.0f;
    }
    return 150.0f;
}

// --- Annotations ------------------------------------------------------------

/// A freehand pen stroke stored in page-normalized coordinates [0,1]x[0,1].
/// Coordinates are relative to the page's top-left so they follow the page
/// as it moves on the canvas.
struct AnnotStroke {
    std::vector<Vec2> pts;
    float r = 0.82f, g = 0.06f, b = 0.06f;
    // Screen-pixel half-width and opacity. Defaults match the original pen look; the
    // unified highlighter creates thick, low-alpha "marker" strokes (wide width, ~0.3
    // alpha) so the page/text shows through underneath.
    float width = 1.2f;
    float alpha = 0.88f;
};

/// An axis-aligned highlight rectangle in page-normalized coordinates.
/// x0 <= x1, y0 <= y1 (enforced at creation time).
/// text is empty for legacy rect-only highlights; populated when the highlight
/// was created with text-snap enabled and covers actual PDF character glyphs.
struct AnnotHighlight {
    float x0 = 0.0f, y0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;
    std::string text;
    // Research note attached to this reference (empty = none). Stored on the highlight
    // itself so it can never mis-key or orphan — it moves and deletes with the highlight.
    std::string note;
};

/// A note flag stamped on a page. Label follows A–Z, then 2A–2Z, 3A–3Z, …
struct AnnotNote {
    std::string label;  // "A", "B", ... "Z", "2A", ...
};

struct PageAnnotations {
    std::vector<AnnotStroke>    strokes;
    std::vector<AnnotHighlight> highlights;
    std::vector<AnnotNote>      notes;
    bool empty() const { return strokes.empty() && highlights.empty() && notes.empty(); }
};

// --- Page -------------------------------------------------------------------

/// A single PDF page with its world-space geometry and per-tier GL textures.
/// Textures are populated lazily by TextureCache; 0 means not yet uploaded.
struct Page {
    uint64_t id      = 0;           // stable identity, survives vector reallocation (0 = unassigned)
    int   page_index = 0;           // 0-based within parent document
    Vec2  world_pos  = {0.0f, 0.0f};
    float world_w    = 0.0f;        // derived from PDF media box (points → world units)
    float world_h    = 0.0f;
    int   rotation   = 0;           // clockwise degrees: 0, 90, 180, 270
    int   group_id   = 0;           // ad-hoc page group; 0 = ungrouped (see PageGroup)

    PageAnnotations annots;          // persistent highlights and pen strokes

    uint32_t tex_thumb = 0;         // GL texture handle for Thumb tier
    uint32_t tex_low   = 0;         // GL texture handle for Low tier
    uint32_t tex_high  = 0;         // GL texture handle for High tier

    // Set by drain_rast_results() when MuPDF returns ok=false. Suppresses all
    // future rasterization attempts so a corrupt/unreadable page doesn't loop.
    bool rast_failed = false;

    /// Best available texture for the requested tier.
    /// Falls back to any loaded tier rather than returning 0.
    uint32_t tex_for_lod(LodTier tier) const {
        switch (tier) {
            case LodTier::Thumb:
                return tex_thumb ? tex_thumb : (tex_low ? tex_low : tex_high);
            case LodTier::Low:
                return tex_low   ? tex_low   : (tex_thumb ? tex_thumb : tex_high);
            case LodTier::High:
                return tex_high  ? tex_high  : (tex_low   ? tex_low   : tex_thumb);
        }
        return 0;
    }

    bool needs_lod(LodTier tier) const {
        if (rast_failed) return false;   // permanently unreadable — don't retry
        switch (tier) {
            case LodTier::Thumb: return tex_thumb == 0;
            case LodTier::Low:   return tex_low   == 0;
            case LodTier::High:  return tex_high  == 0;
        }
        return true;
    }
};

// --- Document / Stack -------------------------------------------------------

/// A loaded PDF document.  Pages are owned here; stacks hold indices into them.
struct Document {
    std::string       path;
    std::vector<Page> pages;
    Vec2              stack_origin = {};   // world pos set at load time; used by "Return to stack"

    float hue_r = 0.5f;   // color identity used for stack stripe
    float hue_g = 0.5f;
    float hue_b = 0.5f;

    int z_layer = 0;      // z-order for rendering; higher = on top

    // True when the referenced PDF could not be found at load time. The document
    // is kept as a placeholder (pages + annotations preserved, no textures) so its
    // reference and annotations survive a re-save and indices stay aligned.
    bool missing = false;

    // True when the PDF opened but is password-protected / encrypted and can't be
    // rendered without authentication. Shown as a distinct placeholder page. Re-derived
    // on every load (not persisted) — a single placeholder page stands in for the doc.
    bool locked = false;

    // Thread visibility: toggled from the Open Documents list in the References tab.
    bool show_threads = false;
};

// --- Page group (ad-hoc cross-document cluster) -----------------------------

/// A temporary, nondestructive grouping of pages — possibly drawn from several
/// different documents — so they can be framed and moved together as a unit.
/// Membership lives on each Page as `group_id`; this table only holds the
/// group's identity and appearance. Ungrouping just clears the members'
/// group_id (their document, position, and annotations are untouched), so the
/// grouping never alters the underlying documents.
struct PageGroup {
    int         id = 0;                            // matches Page::group_id; 0 is never a real group
    std::string name;                              // optional label shown on the frame
    float       col_r = 0.63f, col_g = 0.32f, col_b = 0.75f;  // frame + label color
};

inline constexpr float PAGE_FAN_OFFSET = 20.0f;  // world-unit diagonal offset per page in a fan

// Palette cycled per page group so successive groups are visually distinct.
inline constexpr float GROUP_PALETTE[5][3] = {
    {0.63f, 0.32f, 0.75f},   // violet
    {0.20f, 0.60f, 0.58f},   // teal
    {0.85f, 0.52f, 0.20f},   // amber
    {0.30f, 0.58f, 0.30f},   // green
    {0.78f, 0.32f, 0.44f},   // rose
};
inline constexpr int GROUP_PALETTE_SIZE = 5;

// Color palette cycled per document (hue stripe, selection tint, etc.)
inline constexpr float DOC_PALETTE[5][3] = {
    {0.30f, 0.55f, 0.85f},
    {0.85f, 0.40f, 0.35f},
    {0.35f, 0.75f, 0.45f},
    {0.80f, 0.70f, 0.30f},
    {0.65f, 0.35f, 0.75f},
};
inline constexpr int PALETTE_SIZE = 5;
