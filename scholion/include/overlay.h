#pragma once

class Canvas;

/// Small always-visible debug overlay pinned to the bottom-left corner.
/// Shows a colored FPS health dot, page count, and the current zoom %.
/// Toggled with F3.
class PerformanceOverlay {
public:
    // Feed per-frame timing; maintains a rolling FPS average.
    void update(float delta_time);

    // Emit the ImGui window. Must be called inside an ImGui frame, before
    // ImGui::Render(). Reads zoom % from the canvas.
    void draw(const Canvas& canvas);

    void toggle()        { m_visible = !m_visible; }
    bool visible() const { return m_visible; }

    // Page count tracking; defaults to 0/0.
    void set_page_count(int shown, int total) { m_pages_shown = shown; m_pages_total = total; }

    float fps() const { return m_fps; }

private:
    static constexpr int FPS_HISTORY = 90;
    float m_fps_history[FPS_HISTORY] = {};
    int   m_fps_head  = 0;
    int   m_fps_count = 0;
    float m_fps       = 0.0f;

    int   m_pages_shown = 0;
    int   m_pages_total = 0;

    bool  m_visible = true;  // visible by default for now; F3 toggles
};
