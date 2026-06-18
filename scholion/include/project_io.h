#pragma once
#include <chrono>
#include <string>
#include <vector>

// Globals owned by project_io.cpp
extern std::string              g_project_path;
extern std::vector<std::string> g_recents;
extern std::chrono::steady_clock::time_point g_last_save_time;

// Platform file-dialog focus helpers (used by any code that calls tinyfiledialogs)
void before_file_dialog();
void after_file_dialog();

// Window title
void update_window_title();

// Save
bool save_to_path(const std::string& path);
void save_project();
void save_project_current();
void start_autosave(const std::string& path);
bool autosave_running();

// Load
void load_project();
void load_project_from_path(const std::string& path);

// Preferences
void save_prefs();
void load_prefs();
void apply_theme(bool dark);

// Recent files
void add_to_recents(const std::string& path);
void save_recents();
void load_recents();
