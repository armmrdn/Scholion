#include "texture_cache.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <glad/glad.h>
#endif

uint32_t TextureCache::upload(Page& page, LodTier tier,
                               const uint8_t* pixels, int width, int height) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Trilinear filtering with mipmaps — pages are almost always downscaled
    // when many are on screen, so mipmaps eliminate moiré patterns.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // MuPDF RGBA pixmap rows are top-to-bottom; OpenGL expects bottom-to-top.
    // Flipping is handled in the renderer UV coordinates (v: 1→0 top-to-bottom).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    size_t sz = static_cast<size_t>(width) * height * 4;
    m_vram_bytes += sz;
    m_tex_sizes[tex] = sz;

    switch (tier) {
        case LodTier::Thumb: page.tex_thumb = tex; break;
        case LodTier::Low:   page.tex_low   = tex; break;
        case LodTier::High:  page.tex_high  = tex; break;
    }
    return tex;
}

void TextureCache::evict(Page& page) {
    auto drop = [this](uint32_t& h) {
        if (!h) return;
        auto it = m_tex_sizes.find(h);
        if (it != m_tex_sizes.end()) { m_vram_bytes -= it->second; m_tex_sizes.erase(it); }
        GLuint t = h;
        glDeleteTextures(1, &t);
        h = 0;
    };
    drop(page.tex_thumb);
    drop(page.tex_low);
    drop(page.tex_high);
}

// Eviction policy (decisions made in stream_lod(), executed here):
//   High  — evicted unconditionally when off-screen or zoom drops below 2.5.
//           ~17 MB/page means a dozen off-screen pages would exhaust VRAM.
//   Low   — evicted only when VRAM exceeds VRAM_BUDGET (350 MB); kept alive
//           during zoom changes so there's no re-rasterize stall.
//   Thumb — never evicted; always-available fallback at ~1.1 MB/page.
// m_vram_bytes / m_tex_sizes track usage so stream_lod can budget without
// querying the GL driver (glGetInteger64v would stall the pipeline).
void TextureCache::evict_tier(Page& page, LodTier tier) {
    auto drop = [this](uint32_t& h) {
        if (!h) return;
        auto it = m_tex_sizes.find(h);
        if (it != m_tex_sizes.end()) { m_vram_bytes -= it->second; m_tex_sizes.erase(it); }
        GLuint t = h;
        glDeleteTextures(1, &t);
        h = 0;
    };
    switch (tier) {
        case LodTier::Thumb: drop(page.tex_thumb); break;
        case LodTier::Low:   drop(page.tex_low);   break;
        case LodTier::High:  drop(page.tex_high);  break;
    }
}

void TextureCache::clear() {
    m_vram_bytes = 0;
    m_tex_sizes.clear();
}
