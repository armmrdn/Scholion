#include "input.h"
#include <GLFW/glfw3.h>
#include <algorithm>

InputHandler::InputHandler(Canvas& canvas) : m_canvas(canvas) {}

void InputHandler::on_mouse_button(GLFWwindow* window, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        m_panning = (action == GLFW_PRESS);
        if (m_panning) m_last_mouse = m_current_mouse;
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT && m_space_held) {
        m_panning = (action == GLFW_PRESS);
        if (m_panning) {
            m_last_mouse    = m_current_mouse;
            m_space_dragged = true;  // pan drag began during this space-hold
        }
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT && !m_space_held) {
        if (action == GLFW_PRESS) {
            bool shift = (mods & GLFW_MOD_SHIFT) != 0;
            bool cmd   = (mods & GLFW_MOD_SUPER) || (mods & GLFW_MOD_CONTROL);
            auto hit = hit_test(m_current_mouse);

            if (hit.page) {
                if (shift) {
                    // Toggle entire document in/out of selection
                    bool all_in = true;
                    for (auto& page : hit.doc->pages) {
                        if (!m_selection.count(&page)) { all_in = false; break; }
                    }
                    for (auto& page : hit.doc->pages) {
                        if (all_in) m_selection.erase(&page);
                        else        m_selection.insert(&page);
                    }
                    m_selected_doc = hit.doc;
                    if (!m_selection.empty())
                        start_multi_drag(m_current_mouse);
                } else if (cmd) {
                    // Toggle individual page in/out of selection
                    if (m_selection.count(hit.page))
                        m_selection.erase(hit.page);
                    else {
                        m_selection.insert(hit.page);
                        m_selected_doc = hit.doc;
                    }
                    if (!m_selection.empty())
                        start_multi_drag(m_current_mouse);
                } else if (m_selection.count(hit.page)) {
                    // Clicked a page already in the selection — drag the whole group
                    m_selected_doc = hit.doc;
                    start_multi_drag(m_current_mouse);
                } else {
                    // Fresh single-page deferred drag
                    m_selection.clear();
                    m_multi_drag_active = false;
                    m_drag_origins.clear();
                    m_drag_pending_page = hit.page;
                    m_drag_pending_doc  = hit.doc;
                    m_drag_start_screen = m_current_mouse;
                    m_drag_active       = false;
                    Vec2 world = m_canvas.screen_to_world(m_current_mouse);
                    m_drag_world_offset = {world.x - hit.page->world_pos.x,
                                           world.y - hit.page->world_pos.y};
                    m_selected_doc = hit.doc;
                }
            } else {
                // Clicked empty canvas
                if (!shift && !cmd) {
                    clear_selection();
                    m_selected_doc = nullptr;
                    if (m_panel_open) close_panel();
                }
                // Begin rubber-band box selection
                m_box_selecting   = true;
                m_box_start_world = m_canvas.screen_to_world(m_current_mouse);
                m_box_cur_world   = m_box_start_world;
            }

        } else if (action == GLFW_RELEASE) {
            if (m_box_selecting) {
                finalize_box_selection();
                m_box_selecting = false;
            } else if (m_multi_drag_active) {
                m_multi_drag_active = false;
                m_drag_origins.clear();
            } else if (m_drag_pending_page && !m_drag_active) {
                // Pure click (no drag) — add the clicked page to the selection so threads
                // and the selection border show. Panel stays closed; double-click opens it.
                m_selection.insert(m_drag_pending_page);
            }
            m_dragged_page      = nullptr;
            m_drag_active       = false;
            m_drag_pending_page = nullptr;
            m_drag_pending_doc  = nullptr;
        }
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        auto hit = hit_test(m_current_mouse);
        if (hit.page) {
            m_ctx_pending = true;
            m_ctx_doc     = hit.doc;
            m_ctx_page    = hit.page;
            m_ctx_pos     = m_current_mouse;
        } else {
            m_canvas_ctx_pending = true;
            m_canvas_ctx_pos     = m_current_mouse;
        }
    }
}

void InputHandler::on_cursor_move(GLFWwindow* /*window*/, double xpos, double ypos) {
    m_current_mouse = {(float)xpos, (float)ypos};

    if (m_panning) {
        Vec2 delta = m_current_mouse - m_last_mouse;
        m_canvas.pan(delta);
        m_last_mouse = m_current_mouse;
        return;
    }

    if (m_box_selecting) {
        m_box_cur_world = m_canvas.screen_to_world(m_current_mouse);
        return;
    }

    if (m_multi_drag_active) {
        Vec2 grab_world = m_canvas.screen_to_world(m_current_mouse);
        for (auto& origin : m_drag_origins)
            origin.page->world_pos = grab_world + origin.offset;
        return;
    }

    if (m_drag_pending_page && !m_drag_active) {
        float dx = m_current_mouse.x - m_drag_start_screen.x;
        float dy = m_current_mouse.y - m_drag_start_screen.y;
        if (dx*dx + dy*dy > DRAG_THRESHOLD_SQ) {
            m_drag_active  = true;
            m_dragged_page = m_drag_pending_page;
        }
    }

    if (m_drag_active && m_dragged_page) {
        Vec2 world = m_canvas.screen_to_world(m_current_mouse);
        m_dragged_page->world_pos = {world.x - m_drag_world_offset.x,
                                     world.y - m_drag_world_offset.y};
    } else {
        auto hit = hit_test(m_current_mouse);
        m_hovered_doc  = hit.doc;
        m_hovered_page = hit.page;
    }
}

void InputHandler::on_scroll(GLFWwindow* /*window*/, double xoffset, double yoffset) {
    float zoom_delta = (float)yoffset * ZOOM_SENSITIVITY;
    m_canvas.zoom_at(m_current_mouse, zoom_delta);
}

void InputHandler::on_key(GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int mods) {
    if (key == GLFW_KEY_SPACE) {
        m_space_held = (action == GLFW_PRESS || action == GLFW_REPEAT);
        if (action == GLFW_RELEASE) {
            // Fire a tap signal only when space was released without any pan drag.
            if (!m_space_dragged) m_space_tap_pending = true;
            m_space_held    = false;
            m_panning       = false;
            m_space_dragged = false;
        }
    }

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        if (!m_selection.empty())
            clear_selection();
        else if (m_panel_open)
            close_panel();
        // Never quit — closing via OS window controls only
    }

    // F3 — toggle the performance overlay (consumed by the main loop)
    if (key == GLFW_KEY_F3 && action == GLFW_PRESS)
        m_overlay_toggle_pending = true;

    // Cmd+A — select all pages
    if (key == GLFW_KEY_A && action == GLFW_PRESS && (mods & GLFW_MOD_SUPER) && m_documents) {
        m_selection.clear();
        for (auto& doc : *m_documents)
            for (auto& page : doc.pages)
                m_selection.insert(&page);
    }
}

void InputHandler::update(GLFWwindow* /*window*/) {}

InputHandler::HitResult InputHandler::hit_test(Vec2 screen_pos) const {
    if (!m_documents) return {nullptr, nullptr};
    Vec2 world = m_canvas.screen_to_world(screen_pos);
    for (int di = (int)m_documents->size() - 1; di >= 0; --di) {
        auto& doc = (*m_documents)[di];
        for (int pi = 0; pi < (int)doc.pages.size(); ++pi) {
            auto& page = doc.pages[pi];
            if (world.x >= page.world_pos.x && world.x <= page.world_pos.x + page.world_w &&
                world.y >= page.world_pos.y && world.y <= page.world_pos.y + page.world_h)
                return {&doc, &page};
        }
    }
    return {nullptr, nullptr};
}

void InputHandler::start_multi_drag(Vec2 grab_screen) {
    Vec2 grab_world = m_canvas.screen_to_world(grab_screen);
    m_multi_drag_grab_world = grab_world;
    m_drag_origins.clear();
    for (Page* page : m_selection)
        m_drag_origins.push_back({page, page->world_pos - grab_world});
    m_multi_drag_active = true;
    // Pressing on an item supersedes any rubber-band that began in the same click.
    m_box_selecting     = false;
    m_drag_pending_page = nullptr;
    m_drag_pending_doc  = nullptr;
    m_drag_active       = false;
    m_dragged_page      = nullptr;
}

void InputHandler::finalize_box_selection() {
    if (!m_documents) return;
    float x0 = std::min(m_box_start_world.x, m_box_cur_world.x);
    float y0 = std::min(m_box_start_world.y, m_box_cur_world.y);
    float x1 = std::max(m_box_start_world.x, m_box_cur_world.x);
    float y1 = std::max(m_box_start_world.y, m_box_cur_world.y);
    for (auto& doc : *m_documents) {
        for (auto& page : doc.pages) {
            if (page.world_pos.x >= x0 && page.world_pos.y >= y0 &&
                page.world_pos.x + page.world_w <= x1 &&
                page.world_pos.y + page.world_h <= y1)
                m_selection.insert(&page);
        }
    }
}
