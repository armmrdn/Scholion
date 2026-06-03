#pragma once

#include <algorithm>
#include <cmath>

/// Represents a 2D point in either screen or world coordinates.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
};

/// The infinite canvas viewport. Tracks pan offset and zoom level,
/// and converts between screen pixels and world coordinates.
class Canvas {
public:
    Canvas() = default;

    // --- Coordinate conversion ---
    Vec2 screen_to_world(Vec2 screen_pos) const;
    Vec2 world_to_screen(Vec2 world_pos) const;

    // --- Mutation ---
    void pan(Vec2 delta_screen);
    void zoom_at(Vec2 screen_focus, float zoom_delta);
    void set_viewport_size(float width, float height);

    // --- Accessors ---
    float get_zoom()   const { return m_zoom; }
    Vec2  get_offset() const { return m_offset; }
    float get_viewport_width()  const { return m_viewport_w; }
    float get_viewport_height() const { return m_viewport_h; }

    // Zoom as a percentage where 100% = print size (zoom 1.0). The canvas works
    // in logical points, so zoom 1.0 already maps 1 PDF point to 1 logical point
    // (~1/72 inch) — print size — independent of display pixel density.
    float get_zoom_percentage() const { return m_zoom * 100.0f; }

    // --- Direct setters (used when restoring saved state) ---
    void set_offset(Vec2 o)  { m_offset = o; }
    void set_zoom(float z)   { m_zoom = std::clamp(z, MIN_ZOOM, MAX_ZOOM); }

    // Returns the orthographic projection bounds in world space
    void get_world_bounds(float& left, float& right, float& bottom, float& top) const;

private:
    Vec2  m_offset = {0.0f, 0.0f};  // world-space offset of viewport center
    float m_zoom   = 0.60f;

    float m_viewport_w = 1280.0f;
    float m_viewport_h = 800.0f;

    static constexpr float MIN_ZOOM = 0.02f;
    static constexpr float MAX_ZOOM = 10.0f;
};
