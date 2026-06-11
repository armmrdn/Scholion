#pragma once

#include "document.h"
#include <cstddef>
#include <cstdint>
#include <unordered_map>

/// Manages GPU texture uploads for PDF pages with LOD-tier awareness.
///
/// Workflow (milestone 2):
///   1. MuPDF rasterizes a page to an RGBA pixel buffer (background thread).
///   2. Main thread calls upload() to create the GL texture and store the handle.
///   3. Renderer calls Page::tex_for_lod() each frame to get the best handle.
///   4. When zoom changes tier, the new tier is requested and uploaded lazily.
///   5. Lower tiers are retained as fallback while the higher tier loads.
///
/// VRAM estimate at 150 DPI (Low tier), US Letter, 100 pages:
///   ~870 x 1230 x 4 bytes ≈ 4.3 MB/page → ~430 MB total.
///   Thumb tier reduces that to ~1.1 MB/page → ~110 MB total.
class TextureCache {
public:
    TextureCache()  = default;
    ~TextureCache() { clear(); }

    /// Upload a rasterized pixel buffer as a GL RGBA8 texture.
    /// Sets the appropriate tex_* handle on the page.
    /// pixels: RGBA row-major. stride: bytes per row (0 = packed, i.e. width*4).
    uint32_t upload(Page& page, LodTier tier,
                    const uint8_t* pixels, int width, int height, int stride = 0);

    /// Free all GL textures for a page (e.g., document closed).
    void evict(Page& page);

    /// Free the GL texture for a single LOD tier of a page.
    void evict_tier(Page& page, LodTier tier);

    /// Free all GPU textures managed by this cache.
    void clear();

    /// Upload raw RGBA8 pixels not associated with a Page (e.g. tile textures).
    /// Tracked in m_vram_bytes / m_tex_sizes; release with free_raw().
    uint32_t upload_raw(const uint8_t* pixels, int width, int height);

    /// Release a texture handle created by upload_raw.
    void free_raw(uint32_t tex);

    /// Approximate VRAM usage in bytes (sum of width*height*4 for all textures).
    size_t vram_bytes() const { return m_vram_bytes; }

private:
    size_t m_vram_bytes = 0;
    std::unordered_map<uint32_t, size_t> m_tex_sizes;  // GL handle → byte count
};
