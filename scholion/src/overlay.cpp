#include "overlay.h"
#include "canvas.h"

#include "imgui.h"

void PerformanceOverlay::update(float delta_time, bool active) {
    m_idle = !active;
    if (!active) return;  // idle-frame deltas are not representative; keep last FPS

    float inst = (delta_time > 1e-6f) ? (1.0f / delta_time) : 0.0f;

    m_fps_history[m_fps_head] = inst;
    m_fps_head = (m_fps_head + 1) % FPS_HISTORY;
    if (m_fps_count < FPS_HISTORY) ++m_fps_count;

    // Rolling average over the populated portion of the buffer.
    float sum = 0.0f;
    for (int i = 0; i < m_fps_count; ++i) sum += m_fps_history[i];
    m_fps = (m_fps_count > 0) ? sum / m_fps_count : 0.0f;
}

void PerformanceOverlay::draw(const Canvas& canvas) {
    if (!m_visible) return;

    const float pad = 5.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = { vp->WorkPos.x + pad,
                   vp->WorkPos.y + vp->WorkSize.y - pad };
    // Pivot at the window's bottom-left so it grows upward from the corner.
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad + 3.0f, pad + 3.0f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav         | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs;  // click-through so it never eats canvas clicks

    if (ImGui::Begin("##perf_overlay", nullptr, flags)) {
        // The status overlay is a fixed HUD element — keep it at its default size regardless of the
        // "Larger UI" setting. Neutralize the global font scale (SetWindowFontScale cancels
        // io.FontGlobalScale for this window) and the scaled item spacing, so the readout doesn't
        // grow with the rest of the chrome. No-op when Larger UI is off (inv == 1).
        ImGuiIO& io = ImGui::GetIO();
        float inv = (io.FontGlobalScale > 0.0f) ? 1.0f / io.FontGlobalScale : 1.0f;
        ImGui::SetWindowFontScale(inv);
        ImVec2 sp = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp.x * inv, sp.y * inv));

        // --- Performance health dot ---
        // When idle (event-driven, no work), show a neutral grey dot and "idle"
        // rather than a red dot / low FPS: the app is intentionally not redrawing,
        // which is healthy, not a performance problem.
        ImU32 dot_col;
        if      (m_idle)         dot_col = IM_COL32(120, 120, 130, 255);  // grey
        else if (m_fps >= 50.0f) dot_col = IM_COL32( 80, 220, 100, 255);  // green
        else if (m_fps >= 30.0f) dot_col = IM_COL32(240, 190,  70, 255);  // amber
        else                     dot_col = IM_COL32(230,  80,  80, 255);  // red

        const float    radius = 5.0f;  // ~10px diameter
        const float    line_h = ImGui::GetTextLineHeight();
        ImDrawList*    dl     = ImGui::GetWindowDrawList();
        ImVec2         p      = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + radius, p.y + line_h * 0.5f), radius, dot_col);
        ImGui::Dummy(ImVec2(radius * 2.0f, line_h));
        ImGui::SameLine(0.0f, 6.0f);
        if (m_idle) ImGui::TextUnformatted("idle");
        else        ImGui::Text("%.0f FPS", m_fps);

        ImGui::Text("%d/%d pages", m_pages_shown, m_pages_total);
        ImGui::Text("Zoom: %.0f%%", canvas.get_zoom_percentage());
        ImGui::PopStyleVar();   // ItemSpacing (pushed inside this window)
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
