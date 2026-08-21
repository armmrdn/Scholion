#include "prefs.h"
#include "app_state.h"     // AppSettings g_settings
#include "imgui.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

std::vector<std::string> g_recents;

static constexpr int RECENTS_MAX = 10;

// Both per-user files live in the same place and differ only by leaf name, so the platform
// lookup is written once here rather than duplicated per file.
static std::string config_file_path(const char* win_leaf, const char* posix_leaf) {
    (void)win_leaf; (void)posix_leaf;
#ifdef _WIN32
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wpath))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
        std::string appdata(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &appdata[0], len, nullptr, nullptr);
        CoTaskMemFree(wpath);
        std::string dir = appdata + "\\Scholion";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir + "\\" + win_leaf;
    }
    return "";
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/" + posix_leaf : "";
#endif
}

static std::string recents_file_path() { return config_file_path("recents", ".scholion_recents"); }
static std::string prefs_file_path()   { return config_file_path("prefs",   ".scholion_prefs");   }

// --- Recent projects ---------------------------------------------------------

void save_recents() {
    std::string p = recents_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    for (const auto& r : g_recents) fprintf(f, "%s\n", r.c_str());
    fclose(f);
}

void load_recents() {
    std::string p = recents_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) g_recents.push_back(s);
    }
    fclose(f);
}

void add_to_recents(const std::string& path) {
    g_recents.erase(std::remove(g_recents.begin(), g_recents.end(), path), g_recents.end());
    g_recents.insert(g_recents.begin(), path);
    if ((int)g_recents.size() > RECENTS_MAX) g_recents.resize(RECENTS_MAX);
    save_recents();
}

// --- Preferences -------------------------------------------------------------

void save_prefs() {
    std::string p = prefs_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "dark_mode=%d\n",   g_settings.dark_mode   ? 1 : 0);
    fprintf(f, "grid_mode=%d\n",   (int)g_settings.grid_mode);
    fprintf(f, "compat_mode=%d\n", g_settings.compat_mode ? 1 : 0);
    fprintf(f, "vignette_on=%d\n", g_settings.vignette_on ? 1 : 0);
    fprintf(f, "panel_w=%.1f\n",   g_settings.panel_w);
    fprintf(f, "large_ui=%d\n",    g_settings.large_ui   ? 1 : 0);
    fclose(f);
}

void load_prefs() {
    std::string p = prefs_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char key[64];
    float fval;
    while (fscanf(f, " %63[^=]=%f", key, &fval) == 2) {
        int ival = (int)fval;
        if      (!strcmp(key, "dark_mode"))   g_settings.dark_mode   = ival;
        else if (!strcmp(key, "grid_mode"))   g_settings.grid_mode   = (GridMode)ival;
        else if (!strcmp(key, "compat_mode")) g_settings.compat_mode = ival;
        else if (!strcmp(key, "vignette_on")) g_settings.vignette_on = ival;
        else if (!strcmp(key, "panel_w"))     g_settings.panel_w     = fval;
        else if (!strcmp(key, "large_ui"))    g_settings.large_ui    = ival;
    }
    fclose(f);
}

// --- Appearance --------------------------------------------------------------

// Larger-UI scale factor. Applied to fonts (io.FontGlobalScale) and widget metrics
// (ImGuiStyle::ScaleAllSizes). 1.0 = normal.
static constexpr float SCHOLION_UI_SCALE_LARGE = 1.4f;

void apply_theme(bool dark) {
    if (dark) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
        ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.93f, 0.91f, 0.96f);
    }
    ImGui::GetStyle().WindowRounding   = 6.0f;
    ImGui::GetStyle().PopupRounding    = 5.0f;
    ImGui::GetStyle().FrameRounding    = 4.0f;
    ImGui::GetStyle().WindowBorderSize = 0.0f;
}

// Apply the theme AND the UI scale together. Use this everywhere theme or scale changes.
//
// ScaleAllSizes() *multiplies* the current style metrics, so it compounds if called on an
// already-scaled style. StyleColorsDark/Light only reset colors — NOT the size fields — so they
// do not undo a prior scale. Toggling "Larger UI" on/off therefore used to cascade the UI larger
// each cycle. Fix: reset the ENTIRE style to ImGui defaults first, so every call scales from a
// clean 1.0 baseline exactly once. apply_theme then re-establishes our colors + rounding.
void apply_appearance() {
    ImGui::GetStyle() = ImGuiStyle();   // clean default metrics + colors (no accumulated scale)
    apply_theme(g_settings.dark_mode);
    float s = g_settings.large_ui ? SCHOLION_UI_SCALE_LARGE : 1.0f;
    ImGui::GetIO().FontGlobalScale = s;
    if (s != 1.0f) ImGui::GetStyle().ScaleAllSizes(s);
}
