#include "canvas.h"
#include <algorithm>

Vec2 Canvas::screen_to_world(Vec2 screen_pos) const {
    // Screen center is (viewport_w/2, viewport_h/2).
    // World pos = offset + (screen_pos - screen_center) / zoom
    float cx = m_viewport_w * 0.5f;
    float cy = m_viewport_h * 0.5f;
    return {
        m_offset.x + (screen_pos.x - cx) / m_zoom,
        m_offset.y + (screen_pos.y - cy) / m_zoom
    };
}

Vec2 Canvas::world_to_screen(Vec2 world_pos) const {
    float cx = m_viewport_w * 0.5f;
    float cy = m_viewport_h * 0.5f;
    return {
        (world_pos.x - m_offset.x) * m_zoom + cx,
        (world_pos.y - m_offset.y) * m_zoom + cy
    };
}

void Canvas::pan(Vec2 delta_screen) {
    // Convert screen-space drag delta to world-space movement
    m_offset.x -= delta_screen.x / m_zoom;
    m_offset.y -= delta_screen.y / m_zoom;
}

void Canvas::zoom_at(Vec2 screen_focus, float zoom_delta) {
    // Get world position under cursor before zoom
    Vec2 world_before = screen_to_world(screen_focus);

    // Apply zoom
    float new_zoom = m_zoom * (1.0f + zoom_delta);
    m_zoom = std::clamp(new_zoom, MIN_ZOOM, MAX_ZOOM);

    // Get world position under cursor after zoom
    Vec2 world_after = screen_to_world(screen_focus);

    // Adjust offset so the point under the cursor stays fixed
    m_offset.x -= (world_after.x - world_before.x);
    m_offset.y -= (world_after.y - world_before.y);
}

void Canvas::set_viewport_size(float width, float height) {
    m_viewport_w = width;
    m_viewport_h = height;
}

void Canvas::get_world_bounds(float& left, float& right, float& bottom, float& top) const {
    float half_w = (m_viewport_w * 0.5f) / m_zoom;
    float half_h = (m_viewport_h * 0.5f) / m_zoom;
    left   = m_offset.x - half_w;
    right  = m_offset.x + half_w;
    bottom = m_offset.y - half_h;
    top    = m_offset.y + half_h;
}
