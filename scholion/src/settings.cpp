#include "settings.h"
#include "app_state.h"     // g_settings, g_settings_open, g_logo_tex
#include "project_io.h"    // save_prefs, apply_appearance
#include "version.h"       // SCHOLION_VERSION
#include "imgui.h"
#include <algorithm>       // std::max
#include <cstdint>

// --- Settings popup --------------------------------------------------------

void draw_settings_popup() {
    if (!g_settings_open) return;

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos({vp.x * 0.5f, vp.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({360.0f, 100.0f}, {520.0f, vp.y * 0.9f});

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("Settings", &g_settings_open, flags)) {
        ImGui::End();
        return;
    }

    // Dismiss on ESC or ENTER
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        g_settings_open = false;
        ImGui::End();
        return;
    }

    // --- Appearance ---
    ImGui::SeparatorText("Appearance");
    bool was_dark = g_settings.dark_mode;
    ImGui::RadioButton("Dark",  (int*)&g_settings.dark_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Light", (int*)&g_settings.dark_mode, 0);
    if (g_settings.dark_mode != was_dark) {
        apply_appearance();
        save_prefs();
    }
    bool prev_vignette = g_settings.vignette_on;
    ImGui::Checkbox("Canvas Vignette", &g_settings.vignette_on);
    if (g_settings.vignette_on != prev_vignette)
        save_prefs();

    bool prev_large = g_settings.large_ui;
    ImGui::Checkbox("Larger UI", &g_settings.large_ui);
    ImGui::SetItemTooltip("Scale all text, buttons, and panels up for smaller or high-resolution screens");
    if (g_settings.large_ui != prev_large) {
        apply_appearance();
        save_prefs();
    }

    // --- Canvas Grid ---
    ImGui::SeparatorText("Canvas Grid");
    GridMode prev_grid = g_settings.grid_mode;
    ImGui::RadioButton("Off",        (int*)&g_settings.grid_mode, (int)GridMode::Off);
    ImGui::RadioButton("Line Grid",  (int*)&g_settings.grid_mode, (int)GridMode::Lines);
    ImGui::RadioButton("Dot Matrix", (int*)&g_settings.grid_mode, (int)GridMode::Dots);
    if (g_settings.grid_mode != prev_grid) {
        save_prefs();
    }

    // --- Compatibility Mode ---
    ImGui::SeparatorText("Performance");
    bool prev_compat = g_settings.compat_mode;
    ImGui::Checkbox("Compatibility Mode", &g_settings.compat_mode);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 340.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Caps PDF rendering at Low quality when zoomed out, "
                       "freezes re-rendering at extreme zoom levels, and limits "
                       "the frame rate to 30 fps. Saves globally across all projects.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (g_settings.compat_mode != prev_compat) {
        save_prefs();
    }


    // --- Keyboard Shortcuts ---
    ImGui::SeparatorText("Keyboard Shortcuts");
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_BordersInnerV
                                     | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Key",    ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();

#ifdef __APPLE__
#define MODK "Cmd"
#else
#define MODK "Ctrl"
#endif
        auto row = [](const char* action, const char* key) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", key);
        };

        row("Pan",                              "Middle-drag / Space+drag");
        row("Zoom",                             "Scroll wheel");
        row("Zoom to Fit",                      MODK "+0 / Middle double-click");
        row("Toggle Status Overlay",            "F3");
        row("Full-Text Search",                 MODK "+F");
        row("Select Page",                      "Click");
        row("Open Sidebar Viewer",              "Double-click");
        row("Toggle Sidebar Viewer",            "Space (tap)");
        row("Move Page",                        "Drag");
        row("Toggle Whole-Document Selection",  "Shift+click");
        row("Toggle Item in Selection",         MODK "+click");
        row("Select All",                       MODK "+A");
        row("Rubber-Band Select",               "Drag empty canvas");
        row("Clear Selection / Close Sidebar Viewer", "Escape");
        row("Text Tool",                        "T");
        row("Pen Tool",                         "P");
        row("Highlight Tool",                   "H");
        row("Create Text Box",                  "Drag (T active)");
        row("Edit Text Box",                    "Double-click box");
        row("Duplicate Text Box",               MODK "+C, " MODK "+V");
        row("Delete Text Box",                  "Delete / Backspace");
        row("Undo",                             MODK "+Z");
        row("Save",                             MODK "+S");
        ImGui::EndTable();
#undef MODK
    }

    // Footer with attribution — three center-aligned lines, wrapping to available width
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

    auto center_line = [](const char* s) {
        float avail = ImGui::GetContentRegionAvail().x;
        float tw    = ImGui::CalcTextSize(s).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - tw) * 0.5f));
        ImGui::TextUnformatted(s);
    };

    // Small centered logo at the top of the footer.
    if (g_logo_tex) {
        const float logo = 44.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - logo) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_logo_tex)), {logo, logo});
        ImGui::Spacing();
    }

    center_line("Scholion is a canvas-style PDF review utility.");
    center_line("designed and built by @armmrdn (2026)");
    ImGui::Spacing();
    center_line("a scholion is any note, detail, or definition");
    center_line("handwritten into the margin of a manuscript");
    center_line("by its previous scholars and readers");

    ImGui::Spacing();
    {
        const char* ver = "v" SCHOLION_VERSION;
        float ver_w = ImGui::CalcTextSize(ver).x;
        float avail  = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, avail - ver_w));
        ImGui::TextDisabled("%s", ver);
    }

    ImGui::PopStyleColor();

    ImGui::End();
}
