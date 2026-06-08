#pragma once

#include "canvas.h"
#include <cstdint>
#include <string>
#include <vector>

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
};

/// An axis-aligned highlight rectangle in page-normalized coordinates.
/// x0 <= x1, y0 <= y1 (enforced at creation time).
/// text is empty for legacy rect-only highlights; populated when the highlight
/// was created with text-snap enabled and covers actual PDF character glyphs.
struct AnnotHighlight {
    float x0 = 0.0f, y0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;
    std::string text;
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
    int   page_index = 0;           // 0-based within parent document
    Vec2  world_pos  = {0.0f, 0.0f};
    float world_w    = 0.0f;        // derived from PDF media box (points → world units)
    float world_h    = 0.0f;

    PageAnnotations annots;          // persistent highlights and pen strokes

    uint32_t tex_thumb = 0;         // GL texture handle for Thumb tier
    uint32_t tex_low   = 0;         // GL texture handle for Low tier
    uint32_t tex_high  = 0;         // GL texture handle for High tier

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
};

/// Visual stack of pages on the canvas.  In milestone 3 this grows to support
/// partial fans, reordering, and detached loose pages.
struct DocumentStack {
    Document* doc      = nullptr;
    Vec2      world_pos = {0.0f, 0.0f};

    static constexpr float FAN_OFFSET = 20.0f;  // world-unit diagonal offset per page in a fan
};
