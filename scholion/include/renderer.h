#pragma once

#include "canvas.h"
#include "document.h"
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

    // Textured quad helper — switches to m_tex_prog + m_tex_vao internally.
    void draw_pdf_page_quad(const Canvas& canvas, const Page& page, unsigned int tex);
    void draw_threads(const Canvas& canvas, const Document& doc);
    void draw_page_annotations(const Canvas& canvas, const Page& page);

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
