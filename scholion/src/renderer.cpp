#include "renderer.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
// Windows/Linux: GLAD provides OpenGL 3.3 core function pointers.
// glad.h must appear before any other GL header; the loader is called in main().
#include <glad/glad.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

// --- Embedded shaders -------------------------------------------------------

static const char* COLOR_VERT_SRC = R"glsl(
#version 330 core
layout(location = 0) in vec2 a_pos;
uniform mat4 u_proj;
void main() {
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
)glsl";

static const char* COLOR_FRAG_SRC = R"glsl(
#version 330 core
uniform vec4 u_color;
out vec4 frag_color;
void main() {
    frag_color = u_color;
}
)glsl";

// Textured quad shader — used by milestone 2 PDF page rendering.
// Separate from the color VAO; see draw_pdf_page() in milestone 2.
static const char* TEX_VERT_SRC = R"glsl(
#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
uniform mat4 u_proj;
void main() {
    v_uv = a_uv;
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
)glsl";

static const char* TEX_FRAG_SRC = R"glsl(
#version 330 core
in vec2 v_uv;
out vec4 frag_color;
uniform sampler2D u_tex;
uniform float u_alpha;
void main() {
    frag_color = texture(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, u_alpha);
}
)glsl";

// --- GL helpers -------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char* vert_src, const char* frag_src) {
    GLuint vert = compile_shader(GL_VERTEX_SHADER,   vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vert || !frag) { glDeleteShader(vert); glDeleteShader(frag); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---------------------------------------------------------------------------

bool Renderer::init() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_color_prog = link_program(COLOR_VERT_SRC, COLOR_FRAG_SRC);
    m_tex_prog   = link_program(TEX_VERT_SRC,   TEX_FRAG_SRC);
    if (!m_color_prog || !m_tex_prog) return false;

    m_color_proj_loc  = glGetUniformLocation(m_color_prog, "u_proj");
    m_color_color_loc = glGetUniformLocation(m_color_prog, "u_color");
    m_tex_proj_loc    = glGetUniformLocation(m_tex_prog,   "u_proj");
    m_tex_tex_loc     = glGetUniformLocation(m_tex_prog,   "u_tex");
    m_tex_alpha_loc   = glGetUniformLocation(m_tex_prog,   "u_alpha");

    // Color-only VAO: location 0 = vec2 pos, stride = 8 bytes
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Textured-quad VAO: location 0 = vec2 pos, location 1 = vec2 uv, stride = 16 bytes
    glGenVertexArrays(1, &m_tex_vao);
    glGenBuffers(1, &m_tex_vbo);
    glBindVertexArray(m_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_tex_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    return true;
}

void Renderer::shutdown() {
    if (m_tex_vbo)    { glDeleteBuffers(1, &m_tex_vbo);        m_tex_vbo = 0; }
    if (m_tex_vao)    { glDeleteVertexArrays(1, &m_tex_vao);   m_tex_vao = 0; }
    if (m_vbo)        { glDeleteBuffers(1, &m_vbo);            m_vbo = 0; }
    if (m_vao)        { glDeleteVertexArrays(1, &m_vao);       m_vao = 0; }
    if (m_color_prog) { glDeleteProgram(m_color_prog);         m_color_prog = 0; }
    if (m_tex_prog)   { glDeleteProgram(m_tex_prog);           m_tex_prog = 0; }
    // PDF textures are owned by TextureCache, cleaned up by the caller.
}

void Renderer::set_projection(float vw, float vh) {
    // Column-major orthographic: screen-space (top-left origin) → NDC.
    // Maps x:[0,vw]→[-1,1], y:[0,vh]→[1,-1].
    float proj[16] = {
        2.0f/vw,  0.0f,     0.0f,  0.0f,   // column 0
        0.0f,    -2.0f/vh,  0.0f,  0.0f,   // column 1
        0.0f,     0.0f,    -1.0f,  0.0f,   // column 2
       -1.0f,     1.0f,     0.0f,  1.0f,   // column 3 (translation)
    };
    // Apply to both programs so callers don't need to track which is active.
    glUseProgram(m_color_prog);
    glUniformMatrix4fv(m_color_proj_loc, 1, GL_FALSE, proj);
    glUseProgram(m_tex_prog);
    glUniformMatrix4fv(m_tex_proj_loc,   1, GL_FALSE, proj);
}

void Renderer::draw(const Canvas& canvas, const std::vector<Document>& docs,
                    const DrawHints& hints) {
    if (hints.dark_mode) {
        glClearColor(0.12f, 0.12f, 0.13f, 1.0f);
    } else {
        glClearColor(0.92f, 0.91f, 0.89f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    set_projection(canvas.get_viewport_width(), canvas.get_viewport_height());

    glUseProgram(m_color_prog);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    draw_grid(canvas, hints);

    if (docs.empty()) {
        draw_placeholder_pages(canvas);
    } else {
        draw_pdf_pages(canvas, docs, hints);
        if (hints.selected_doc) draw_threads(canvas, *hints.selected_doc);

        // Rubber-band box selection overlay
        if (hints.box_selecting) {
            float x0 = std::min(hints.box_start_world.x, hints.box_cur_world.x);
            float y0 = std::min(hints.box_start_world.y, hints.box_cur_world.y);
            float w  = std::abs(hints.box_cur_world.x - hints.box_start_world.x);
            float h  = std::abs(hints.box_cur_world.y - hints.box_start_world.y);
            draw_rect(canvas, x0, y0, w, h, 0.25f, 0.55f, 1.0f, 0.07f);
            draw_rect_outline(canvas, x0, y0, w, h, 0.30f, 0.62f, 1.0f, 0.65f);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer::draw_grid(const Canvas& canvas, const DrawHints& hints) {
    if (hints.grid_mode == GridMode::Off) return;

    float left, right, bottom, top;
    canvas.get_world_bounds(left, right, bottom, top);
    if (bottom > top) { float t = bottom; bottom = top; top = t; }

    float zoom = canvas.get_zoom();
    float spacing = 60.0f;
    while (spacing * zoom < 20.0f)  spacing *= 5.0f;
    while (spacing * zoom > 120.0f) spacing /= 5.0f;

    std::vector<float> verts;
    verts.reserve(512);

    float start_x = std::floor(left   / spacing) * spacing;
    float start_y = std::floor(bottom / spacing) * spacing;

    if (hints.grid_mode == GridMode::Lines) {
        // Draw grid lines
        for (float x = start_x; x <= right; x += spacing) {
            Vec2 s0 = canvas.world_to_screen({x, bottom});
            Vec2 s1 = canvas.world_to_screen({x, top});
            verts.insert(verts.end(), {s0.x, s0.y, s1.x, s1.y});
        }
        for (float y = start_y; y <= top; y += spacing) {
            Vec2 s0 = canvas.world_to_screen({left,  y});
            Vec2 s1 = canvas.world_to_screen({right, y});
            verts.insert(verts.end(), {s0.x, s0.y, s1.x, s1.y});
        }

        if (!verts.empty()) {
            if (hints.dark_mode) {
                glUniform4f(m_color_color_loc, 1.0f, 1.0f, 1.0f, 0.06f);
            } else {
                glUniform4f(m_color_color_loc, 0.0f, 0.0f, 0.0f, 0.07f);
            }
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size() / 2));
        }

        // Origin crosshair — brighter than the grid
        Vec2 ox0 = canvas.world_to_screen({left,  0.0f});
        Vec2 ox1 = canvas.world_to_screen({right, 0.0f});
        Vec2 oy0 = canvas.world_to_screen({0.0f,  bottom});
        Vec2 oy1 = canvas.world_to_screen({0.0f,  top});
        float cross[] = {
            ox0.x, ox0.y,  ox1.x, ox1.y,
            oy0.x, oy0.y,  oy1.x, oy1.y,
        };
        if (hints.dark_mode) {
            glUniform4f(m_color_color_loc, 1.0f, 1.0f, 1.0f, 0.15f);
        } else {
            glUniform4f(m_color_color_loc, 0.0f, 0.0f, 0.0f, 0.14f);
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(cross), cross, GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, 4);

    } else if (hints.grid_mode == GridMode::Dots) {
        // Draw dot matrix at grid intersections
        for (float x = start_x; x <= right; x += spacing) {
            for (float y = start_y; y <= top; y += spacing) {
                Vec2 s = canvas.world_to_screen({x, y});
                verts.insert(verts.end(), {s.x, s.y});
            }
        }

        if (!verts.empty()) {
            glPointSize(2.0f);
            if (hints.dark_mode) {
                glUniform4f(m_color_color_loc, 1.0f, 1.0f, 1.0f, 0.25f);
            } else {
                glUniform4f(m_color_color_loc, 0.0f, 0.0f, 0.0f, 0.30f);
            }
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(verts.size() / 2));
            glPointSize(1.0f);
        }

        // Origin crosshair for dot matrix mode (as a small + at origin)
        Vec2 ox0 = canvas.world_to_screen({left,  0.0f});
        Vec2 ox1 = canvas.world_to_screen({right, 0.0f});
        Vec2 oy0 = canvas.world_to_screen({0.0f,  bottom});
        Vec2 oy1 = canvas.world_to_screen({0.0f,  top});
        float cross[] = {
            ox0.x, ox0.y,  ox1.x, ox1.y,
            oy0.x, oy0.y,  oy1.x, oy1.y,
        };
        if (hints.dark_mode) {
            glUniform4f(m_color_color_loc, 1.0f, 1.0f, 1.0f, 0.15f);
        } else {
            glUniform4f(m_color_color_loc, 0.0f, 0.0f, 0.0f, 0.14f);
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(cross), cross, GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, 4);
    }
}

bool Renderer::is_rect_visible(const Canvas& canvas,
                                float x, float y, float w, float h) const {
    Vec2 tl = canvas.world_to_screen({x,     y    });
    Vec2 br = canvas.world_to_screen({x + w, y + h});
    float vw = canvas.get_viewport_width();
    float vh = canvas.get_viewport_height();
    return !(br.x < 0.0f || tl.x > vw || br.y < 0.0f || tl.y > vh);
}

void Renderer::draw_rect(const Canvas& canvas,
                          float x, float y, float w, float h,
                          float r, float g, float b, float a) {
    Vec2 tl = canvas.world_to_screen({x,     y    });
    Vec2 br = canvas.world_to_screen({x + w, y + h});

    float verts[] = {
        tl.x, tl.y,   br.x, tl.y,   br.x, br.y,   // triangle 1
        tl.x, tl.y,   br.x, br.y,   tl.x, br.y,   // triangle 2
    };
    glUniform4f(m_color_color_loc, r, g, b, a);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Renderer::draw_rect_outline(const Canvas& canvas,
                                  float x, float y, float w, float h,
                                  float r, float g, float b, float a) {
    Vec2 tl = canvas.world_to_screen({x,     y    });
    Vec2 br = canvas.world_to_screen({x + w, y + h});

    float verts[] = {
        tl.x, tl.y,   br.x, tl.y,   br.x, br.y,   tl.x, br.y,
    };
    glUniform4f(m_color_color_loc, r, g, b, a);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    glDrawArrays(GL_LINE_LOOP, 0, 4);
}

void Renderer::draw_rect_dashed(const Canvas& canvas,
                                 float x, float y, float w, float h,
                                 float r, float g, float b, float a,
                                 float margin_px, float dash_px, float gap_px) {
    Vec2 tl = canvas.world_to_screen({x,     y    });
    Vec2 br = canvas.world_to_screen({x + w, y + h});
    // Expand outward by a fixed screen margin so the dashes clear the item edge
    // regardless of zoom.
    float x0 = tl.x - margin_px, y0 = tl.y - margin_px;
    float x1 = br.x + margin_px, y1 = br.y + margin_px;

    Vec2 corner[4] = { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} };  // clockwise
    std::vector<float> verts;
    verts.reserve(512);
    const float period = dash_px + gap_px;
    for (int e = 0; e < 4; ++e) {
        Vec2 p0 = corner[e], p1 = corner[(e + 1) & 3];
        float ex = p1.x - p0.x, ey = p1.y - p0.y;
        float len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-3f) continue;
        float ux = ex / len, uy = ey / len;
        for (float s = 0.0f; s < len; s += period) {
            float d1 = std::min(s + dash_px, len);
            verts.push_back(p0.x + ux * s);  verts.push_back(p0.y + uy * s);
            verts.push_back(p0.x + ux * d1); verts.push_back(p0.y + uy * d1);
        }
    }
    if (verts.empty()) return;
    glUniform4f(m_color_color_loc, r, g, b, a);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, (GLsizei)(verts.size() / 2));
}

void Renderer::draw_pdf_page_quad(const Canvas& canvas, const Page& page,
                                   unsigned int tex) {
    Vec2 tl = canvas.world_to_screen(page.world_pos);
    Vec2 br = canvas.world_to_screen({page.world_pos.x + page.world_w,
                                      page.world_pos.y + page.world_h});

    // MuPDF row 0 = top of page. glTexImage2D stores data[0] at v=0 (GL bottom).
    // So v=0 in GL texture == page top. Map screen-top to v=0, screen-bottom to v=1.
    float verts[] = {
        tl.x, tl.y,  0.0f, 0.0f,   // TL
        br.x, tl.y,  1.0f, 0.0f,   // TR
        br.x, br.y,  1.0f, 1.0f,   // BR — triangle 1
        tl.x, tl.y,  0.0f, 0.0f,   // TL
        br.x, br.y,  1.0f, 1.0f,   // BR
        tl.x, br.y,  0.0f, 1.0f,   // BL — triangle 2
    };

    glUseProgram(m_tex_prog);
    glBindVertexArray(m_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_tex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(m_tex_tex_loc, 0);
    glUniform1f(m_tex_alpha_loc, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore color program state for subsequent draw_rect calls
    glUseProgram(m_color_prog);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
}

void Renderer::draw_pdf_tile_quad(const Canvas& canvas,
                                   float wx, float wy, float ww, float wh,
                                   uint32_t tex) {
    Vec2 tl = canvas.world_to_screen({wx,       wy});
    Vec2 br = canvas.world_to_screen({wx + ww,  wy + wh});

    float verts[] = {
        tl.x, tl.y,  0.0f, 0.0f,
        br.x, tl.y,  1.0f, 0.0f,
        br.x, br.y,  1.0f, 1.0f,
        tl.x, tl.y,  0.0f, 0.0f,
        br.x, br.y,  1.0f, 1.0f,
        tl.x, br.y,  0.0f, 1.0f,
    };

    glUseProgram(m_tex_prog);
    glBindVertexArray(m_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_tex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(m_tex_tex_loc, 0);
    glUniform1f(m_tex_alpha_loc, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glUseProgram(m_color_prog);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
}

// Coordinate systems used throughout this file:
//   World space  — 1 unit = 1 PDF point (1/72 inch). Page positions, world_w/h,
//                  and annotation coords are in this space. The canvas pan/zoom
//                  transforms between world and screen.
//   Screen space — GLFW framebuffer pixels, origin top-left. All GL vertex data
//                  is in screen space (set_projection maps it to NDC).
//   canvas.world_to_screen(p)  = (p - offset) * zoom + viewport_center
//   canvas.screen_to_world(p)  = (p - viewport_center) / zoom + offset
//
// draw_rect_dashed uses a fixed screen-pixel margin so the dashed border stays
// a constant visual distance outside its item regardless of zoom level.
void Renderer::draw_pdf_pages(const Canvas& canvas, const std::vector<Document>& docs,
                               const DrawHints& hints) {
    LodTier lod = lod_for_zoom(canvas.get_zoom());

    // Build a sorted index vector for z-order (higher z_layer = drawn last = on top)
    std::vector<int> order(docs.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return docs[a].z_layer < docs[b].z_layer;
    });

    for (int di : order) {
        const auto& doc = docs[di];
        // Draw back-to-front: page N-1 first, page 0 last (on top).
        // Page positions: page 0 at stack_origin, page i at origin + i*FAN_OFFSET.
        // Drawing page 0 last makes the first page the visible front of the fan.
        for (int pi = static_cast<int>(doc.pages.size()) - 1; pi >= 0; --pi) {
            const auto& page = doc.pages[pi];
            if (!is_rect_visible(canvas, page.world_pos.x, page.world_pos.y,
                                  page.world_w, page.world_h))
                continue;

            // Shadow (color program is active here)
            draw_rect(canvas,
                      page.world_pos.x + 4.0f, page.world_pos.y + 4.0f,
                      page.world_w, page.world_h,
                      0.0f, 0.0f, 0.0f, 0.35f);

            static constexpr float STRIPE_H = 10.0f;

            if (lod == LodTier::High && hints.tile_cache && hints.content_scale > 0.0f) {
                // Tile-based High rendering: Low/Thumb fallback underneath, tiles on top.
                // This lets the page appear immediately at moderate quality while high-res
                // tiles stream in one-by-one from the background rasterizer.
                uint32_t fallback = page.tex_low ? page.tex_low : page.tex_thumb;
                if (fallback) {
                    draw_pdf_page_quad(canvas, page, fallback);
                } else {
                    float pulse = 0.03f * std::sin(hints.draw_time * 2.5f);
                    draw_rect(canvas, page.world_pos.x, page.world_pos.y,
                              page.world_w, page.world_h,
                              0.95f + pulse, 0.94f + pulse, 0.92f + pulse, 1.0f);
                }
                float px_per_wu = dpi_for_lod(LodTier::High) * hints.content_scale / 72.0f;
                float tile_wu   = static_cast<float>(TILE_PX) / px_per_wu;
                int n_cols = static_cast<int>(std::ceil(page.world_w / tile_wu));
                int n_rows = static_cast<int>(std::ceil(page.world_h / tile_wu));
                for (int row = 0; row < n_rows; ++row) {
                    for (int col = 0; col < n_cols; ++col) {
                        auto it = hints.tile_cache->find(
                            tile_cache_key(doc.path, page.page_index, col, row));
                        if (it == hints.tile_cache->end()) continue;
                        float tx = page.world_pos.x + col * tile_wu;
                        float ty = page.world_pos.y + row * tile_wu;
                        float tw = std::min(tile_wu, page.world_pos.x + page.world_w  - tx);
                        float th = std::min(tile_wu, page.world_pos.y + page.world_h - ty);
                        if (tw > 0.0f && th > 0.0f)
                            draw_pdf_tile_quad(canvas, tx, ty, tw, th, it->second);
                    }
                }
                draw_rect(canvas, page.world_pos.x, page.world_pos.y,
                          page.world_w, STRIPE_H, doc.hue_r, doc.hue_g, doc.hue_b, 0.55f);
            } else {
                uint32_t tex = page.tex_for_lod(lod);
                if (tex) {
                    draw_pdf_page_quad(canvas, page, tex);
                    draw_rect(canvas,
                              page.world_pos.x, page.world_pos.y,
                              page.world_w, STRIPE_H,
                              doc.hue_r, doc.hue_g, doc.hue_b, 0.55f);
                } else {
                    // Three placeholder states — each with a distinct color:
                    //   missing PDF  → dark grey (document unresolvable)
                    //   rast failed  → warm rose (page unreadable; no retry)
                    //   loading      → warm white with a slow breathing shimmer
                    float fr, fg, fb;
                    if (doc.missing) {
                        fr = 0.28f; fg = 0.28f; fb = 0.31f;
                    } else if (page.rast_failed) {
                        fr = 0.96f; fg = 0.89f; fb = 0.88f;
                    } else {
                        float pulse = 0.03f * std::sin(hints.draw_time * 2.5f);
                        fr = 0.95f + pulse;
                        fg = 0.94f + pulse;
                        fb = 0.92f + pulse;
                    }
                    draw_rect(canvas,
                              page.world_pos.x, page.world_pos.y,
                              page.world_w, page.world_h,
                              fr, fg, fb, 1.0f);
                    draw_rect(canvas,
                              page.world_pos.x, page.world_pos.y,
                              page.world_w, STRIPE_H,
                              doc.hue_r, doc.hue_g, doc.hue_b, 0.8f);
                }
            }

            // Annotations (highlights + pen strokes) drawn over the page texture
            if (!page.annots.empty())
                draw_page_annotations(canvas, page);

            // Selection outline — thin dotted grey border, offset outside the page,
            // marking items in the selection (also covers a whole document).
            if (hints.selection && hints.selection->count(const_cast<Page*>(&page))) {
                draw_rect_dashed(canvas,
                                 page.world_pos.x, page.world_pos.y,
                                 page.world_w,      page.world_h,
                                 0.85f, 0.85f, 0.90f, 0.95f,
                                 8.0f, 6.0f, 4.0f);   // 8px offset, 6px dash / 4px gap
            }

            // Hover / drag highlight (white)
            bool is_hover = (&page == hints.hovered_page);
            bool is_drag  = (&page == hints.dragged_page);
            if (is_hover || is_drag) {
                float a = is_drag ? 0.90f : 0.60f;
                draw_rect_outline(canvas,
                                  page.world_pos.x - 1.0f, page.world_pos.y - 1.0f,
                                  page.world_w + 2.0f,      page.world_h + 2.0f,
                                  1.0f, 1.0f, 1.0f, a);
                draw_rect_outline(canvas,
                                  page.world_pos.x - 2.0f, page.world_pos.y - 2.0f,
                                  page.world_w + 4.0f,      page.world_h + 4.0f,
                                  1.0f, 1.0f, 1.0f, a * 0.35f);
            }
        }
    }
}

static Vec2 cubic_bezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    float u = 1.0f - t;
    return {
        u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
        u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y
    };
}

static Vec2 cubic_bezier_deriv(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    float u = 1.0f - t;
    return {
        3*(u*u*(p1.x-p0.x) + 2*u*t*(p2.x-p1.x) + t*t*(p3.x-p2.x)),
        3*(u*u*(p1.y-p0.y) + 2*u*t*(p2.y-p1.y) + t*t*(p3.y-p2.y))
    };
}

static Vec2 vec2_norm(Vec2 v) {
    float l = std::sqrt(v.x*v.x + v.y*v.y);
    return l > 1e-6f ? Vec2{v.x/l, v.y/l} : Vec2{1.0f, 0.0f};
}

// Returns the midpoint of whichever edge of 'page' faces closest toward 'toward',
// plus the outward unit normal of that edge — used as the thread anchor and
// departure/arrival tangent direction.
struct EdgeAnchor { Vec2 pos; Vec2 tang; };

static EdgeAnchor nearest_edge_anchor(const Page& page, Vec2 toward) {
    Vec2 c = {page.world_pos.x + page.world_w * 0.5f,
              page.world_pos.y + page.world_h * 0.5f};
    float dx = toward.x - c.x;
    float dy = toward.y - c.y;
    // Normalize displacement by half-extents so aspect ratio doesn't bias selection
    float rx = page.world_w > 0.0f ? std::abs(dx) / (page.world_w  * 0.5f) : 0.0f;
    float ry = page.world_h > 0.0f ? std::abs(dy) / (page.world_h * 0.5f) : 0.0f;
    if (rx >= ry) {
        if (dx >= 0.0f) return {{page.world_pos.x + page.world_w, c.y}, { 1.0f,  0.0f}};
        else            return {{page.world_pos.x,                 c.y}, {-1.0f,  0.0f}};
    } else {
        if (dy >= 0.0f) return {{c.x, page.world_pos.y + page.world_h}, { 0.0f,  1.0f}};
        else            return {{c.x, page.world_pos.y              },   { 0.0f, -1.0f}};
    }
}

void Renderer::draw_threads(const Canvas& canvas, const Document& doc) {
    if (doc.pages.size() < 2) return;

    std::vector<const Page*> ordered;
    ordered.reserve(doc.pages.size());
    for (const auto& p : doc.pages) ordered.push_back(&p);
    std::sort(ordered.begin(), ordered.end(),
              [](const Page* a, const Page* b){ return a->page_index < b->page_index; });

    const float HALF_W = 0.75f;
    const int   STEPS  = 32;
    const GLsizei VERT_N = static_cast<GLsizei>((STEPS + 1) * 2);

    std::vector<float> strip, glow;
    strip.reserve((STEPS + 1) * 4);
    glow.reserve((STEPS + 1) * 4);

    int n = static_cast<int>(ordered.size());
    for (int i = 0; i < n - 1; ++i) {
        const Page* a = ordered[i];
        const Page* b = ordered[i + 1];

        Vec2 a_ctr = {a->world_pos.x + a->world_w * 0.5f,
                      a->world_pos.y + a->world_h * 0.5f};
        Vec2 b_ctr = {b->world_pos.x + b->world_w * 0.5f,
                      b->world_pos.y + b->world_h * 0.5f};

        // Find each page's nearest edge to the other
        EdgeAnchor ea = nearest_edge_anchor(*a, b_ctr);
        EdgeAnchor eb = nearest_edge_anchor(*b, a_ctr);

        Vec2 s0 = canvas.world_to_screen(ea.pos);
        Vec2 s3 = canvas.world_to_screen(eb.pos);

        // Handle length proportional to screen-space separation (node-editor style).
        // world_to_screen is uniform scale+translate, so direction vectors are preserved.
        float dx = s3.x - s0.x, dy = s3.y - s0.y;
        float dist   = std::sqrt(dx*dx + dy*dy);
        float handle = std::max(dist * 0.45f, 60.0f);

        // Control points depart/arrive perpendicular to their respective edges
        Vec2 s1 = {s0.x + ea.tang.x * handle, s0.y + ea.tang.y * handle};
        Vec2 s2 = {s3.x + eb.tang.x * handle, s3.y + eb.tang.y * handle};

        strip.clear();
        glow.clear();
        for (int s = 0; s <= STEPS; ++s) {
            float t  = static_cast<float>(s) / STEPS;
            Vec2 pos = cubic_bezier(s0, s1, s2, s3, t);
            Vec2 tan = cubic_bezier_deriv(s0, s1, s2, s3, t);
            Vec2 nrm = vec2_norm({-tan.y, tan.x});

            strip.push_back(pos.x + nrm.x * HALF_W);
            strip.push_back(pos.y + nrm.y * HALF_W);
            strip.push_back(pos.x - nrm.x * HALF_W);
            strip.push_back(pos.y - nrm.y * HALF_W);

            const float GW = HALF_W * 4.0f;
            glow.push_back(pos.x + nrm.x * GW);
            glow.push_back(pos.y + nrm.y * GW);
            glow.push_back(pos.x - nrm.x * GW);
            glow.push_back(pos.y - nrm.y * GW);
        }

        // Glow: faint maroon halo (midpoint between original and reduced opacity)
        glUniform4f(m_color_color_loc, 0.42f, 0.03f, 0.06f, 0.055f);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(glow.size() * sizeof(float)),
                     glow.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, VERT_N);

        // Core wire: dark maroon (midpoint between original and reduced opacity)
        glUniform4f(m_color_color_loc, 0.52f, 0.05f, 0.08f, 0.34f);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(strip.size() * sizeof(float)),
                     strip.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, VERT_N);
    }
}

void Renderer::draw_page_annotations(const Canvas& canvas, const Page& page) {
    // color program + m_vao + m_vbo are already active (called from draw_pdf_pages).

    // Highlights: semi-transparent yellow rects
    for (const auto& hl : page.annots.highlights) {
        draw_rect(canvas,
            page.world_pos.x + hl.x0 * page.world_w,
            page.world_pos.y + hl.y0 * page.world_h,
            (hl.x1 - hl.x0) * page.world_w,
            (hl.y1 - hl.y0) * page.world_h,
            1.0f, 0.88f, 0.0f, 0.32f);
    }

    // Pen strokes: thin red triangle strip per stroke
    static constexpr float HALF_W = 1.2f;  // screen-pixel half-width

    for (const auto& stroke : page.annots.strokes) {
        int n = static_cast<int>(stroke.pts.size());
        if (n < 2) continue;

        std::vector<float> strip;
        strip.reserve(n * 4);

        for (int i = 0; i < n; ++i) {
            // Smooth tangent from adjacent points
            int prev = std::max(0, i - 1);
            int next = std::min(n - 1, i + 1);

            Vec2 sp = canvas.world_to_screen({
                page.world_pos.x + stroke.pts[prev].x * page.world_w,
                page.world_pos.y + stroke.pts[prev].y * page.world_h});
            Vec2 sn = canvas.world_to_screen({
                page.world_pos.x + stroke.pts[next].x * page.world_w,
                page.world_pos.y + stroke.pts[next].y * page.world_h});
            Vec2 sc = canvas.world_to_screen({
                page.world_pos.x + stroke.pts[i].x * page.world_w,
                page.world_pos.y + stroke.pts[i].y * page.world_h});

            float dx = sn.x - sp.x, dy = sn.y - sp.y;
            float l = std::sqrt(dx*dx + dy*dy);
            Vec2 nrm = (l > 0.01f) ? Vec2{-dy / l, dx / l} : Vec2{1.0f, 0.0f};

            strip.push_back(sc.x + nrm.x * HALF_W);
            strip.push_back(sc.y + nrm.y * HALF_W);
            strip.push_back(sc.x - nrm.x * HALF_W);
            strip.push_back(sc.y - nrm.y * HALF_W);
        }

        glUniform4f(m_color_color_loc, stroke.r, stroke.g, stroke.b, 0.88f);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(strip.size() * sizeof(float)),
                     strip.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(strip.size() / 2));
    }
}

void Renderer::draw_placeholder_pages(const Canvas& canvas) {
    struct PlaceholderStack {
        float x, y;
        int   page_count;
        float hue_r, hue_g, hue_b;
    };

    static const PlaceholderStack stacks[] = {
        {-400.0f, -300.0f, 8,  0.30f, 0.55f, 0.85f},
        { 300.0f, -200.0f, 5,  0.85f, 0.40f, 0.35f},
        {  50.0f,  400.0f, 10, 0.35f, 0.75f, 0.45f},
        {-500.0f,  300.0f, 3,  0.80f, 0.70f, 0.30f},
    };

    constexpr float page_w = 153.0f;
    constexpr float page_h = 198.0f;
    constexpr float fan    = 4.0f;

    for (const auto& stack : stacks) {
        // Cull entire stack before iterating pages
        float stack_w = page_w + stack.page_count * fan;
        float stack_h = page_h + stack.page_count * fan;
        if (!is_rect_visible(canvas, stack.x, stack.y, stack_w, stack_h))
            continue;

        for (int i = 0; i < stack.page_count; i++) {
            float px = stack.x + i * fan;
            float py = stack.y + i * fan;

            if (!is_rect_visible(canvas, px, py, page_w, page_h)) continue;

            draw_rect(canvas, px + 3.0f, py + 3.0f, page_w, page_h,
                      0.0f, 0.0f, 0.0f, 0.3f);                       // shadow
            draw_rect(canvas, px, py, page_w, page_h,
                      0.95f, 0.94f, 0.92f, 1.0f);                    // page body
            draw_rect(canvas, px, py, page_w, 8.0f,
                      stack.hue_r, stack.hue_g, stack.hue_b, 0.8f);  // color stripe

            for (int line = 0; line < 12; line++) {
                float lw = page_w * (0.5f + 0.4f * ((line * 7 + i * 3) % 5) / 5.0f);
                draw_rect(canvas, px + 10.0f, py + 20.0f + line * 12.0f,
                          lw, 3.0f, 0.4f, 0.4f, 0.4f, 0.25f);        // fake text
            }
        }
    }
}
