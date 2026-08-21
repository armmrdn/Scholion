#pragma once
// App-level persisted state — everything stored per USER rather than per project: the preferences
// file (theme, canvas grid, compatibility mode, panel width, Larger-UI) and the recent-projects
// list. Both live side by side in the platform's per-user config location (%APPDATA%\Scholion on
// Windows, ~/.scholion_* elsewhere). Also owns pushing the theme + UI scale into ImGui.
//
// Project *content* serialization (.scholion save/load) is project_io.h — a separate concern.
// Implementation in src/prefs.cpp.
#include <string>
#include <vector>

// --- Preferences (values live in AppSettings g_settings, declared in app_state.h) ---
void save_prefs();
void load_prefs();
void apply_theme(bool dark);
void apply_appearance();   // theme + UI scale together — call whenever either one changes

// --- Recent projects ---
extern std::vector<std::string> g_recents;
void add_to_recents(const std::string& path);
void save_recents();
void load_recents();
