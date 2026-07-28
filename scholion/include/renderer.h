#pragma once
#include <array>

#include "canvas.h"
#include "document.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class GridMode { Off, Lines, Dots };

/// Hints passed each frame so the renderer can draw hover/drag/selection feedback.
struct DrawHints {
    const Document* selected_doc = nullptr;
    const Page*     hovered_page = nullptr;
    const Page*     dragged_page = nullptr;

    // Multi-page selection
    const std::unordered_set<Page*>* selection = nullptr;

    // Rubber-band box
    bool box_selecting   = false;
    Vec2 box_start_world = {};
    Vec2 box_cur_world   = {};

    // Canvas settings
    GridMode grid_mode = GridMode::Lines;
    bool     dark_mode = true;

    // Time in seconds since app start — drives the loading shimmer animation.
    float draw_time = 0.0f;

    // High-tier tile cache: maps tile_cache_key() → GL texture handle.
    // nullptr disables tiled rendering (uses full-page tex_for_lod fallback).
    const std::unordered_map<std::string, uint32_t>* tile_cache = nullptr;

    // Display content scale from glfwGetWindowContentScale, used to convert
    // TILE_PX to world-space tile width for tile quad positioning.
    float content_scale = 1.0f;

    // Transient search hit highlights — set when user clicks a search result.
    // Cleared when the search panel closes or the query changes.
    int                                          search_hit_doc   = -1;
    int                                          search_hit_page  = -1;
    const std::vector<std::array<float,4>>*      search_hit_rects = nullptr;
};

/// Handles all OpenGL drawing using a GL 3.3 core profile pipeline.
///
/// Two shader programs:
///   m_color_prog — solid-color geometry (grid, shadows, placeholder rects, outlines)
///   m_tex_prog   — textured quads for PDF pages
class Renderer {
public:
    Renderer() = default;

    bool init();
    void shutdown();

    /// Draw a complete frame.
    void draw(const Canvas& canvas, const std::vector<Document>& docs = {},
              const DrawHints& hints = {});

private:
    void draw_grid(const Canvas& canvas, const DrawHints& hints);
    void draw_placeholder_pages(const Canvas& canvas);
    void draw_pdf_pages(const Canvas& canvas, const std::vector<Document>& docs,
                        const DrawHints& hints);

    // Solid-color helpers — require m_color_prog + m_vao + m_vbo active.
    void draw_rect(const Canvas& canvas, float x, float y, float w, float h,
                   float r, float g, float b, float a = 1.0f);
    void draw_rect_outline(const Canvas& canvas, float x, float y, float w, float h,
                            float r, float g, float b, float a = 1.0f);
    // Dashed outline; the rect is expanded outward by margin_px (screen pixels) so
    // it sits clearly outside the item. dash/gap are in screen pixels.
    void draw_rect_dashed(const Canvas& canvas, float x, float y, float w, float h,
                          float r, float g, float b, float a,
                          float margin_px, float dash_px, float gap_px);

    // Textured quad helpers — switch to m_tex_prog + m_tex_vao internally.
    void draw_pdf_page_quad(const Canvas& canvas, const Page& page, unsigned int tex);
    // Tile variant: draws a textured quad over an arbitrary world-space rect.
    void draw_pdf_tile_quad(const Canvas& canvas, float wx, float wy, float ww, float wh,
                            uint32_t tex);
    // hued: false → default maroon wires (selected/dragged doc); true → the document's
    // assigned hue color (threads toggled on from the Open Documents sidebar list).
    void draw_threads(const Canvas& canvas, const Document& doc, bool hued);
    void draw_page_annotations(const Canvas& canvas, const Page& page,
                               const DrawHints& hints, int doc_idx);

    void set_projection(float vw, float vh);

    bool is_rect_visible(const Canvas& canvas, float x, float y, float w, float h) const;

    // --- GL resources (GLuint = unsigned int) ---

    // Solid-color pipeline
    unsigned int m_color_prog  = 0;
    unsigned int m_vao         = 0;
    unsigned int m_vbo         = 0;
    int          m_color_proj_loc  = -1;
    int          m_color_color_loc = -1;

    // Textured-quad pipeline
    unsigned int m_tex_prog    = 0;
    unsigned int m_tex_vao     = 0;
    unsigned int m_tex_vbo     = 0;
    int          m_tex_proj_loc  = -1;
    int          m_tex_tex_loc   = -1;
    int          m_tex_alpha_loc = -1;
};
