#pragma once
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

// Globals owned by project_io.cpp
extern std::string              g_project_path;
extern std::chrono::steady_clock::time_point g_last_save_time;

// Save-durability / status state (batch #3)
extern bool        g_load_ok;         // false → last load was suspect: warn + suppress autosave
extern bool        g_dirty;           // true  → unsaved edits since the last save
extern std::time_t g_last_save_wall;  // wall-clock of the last successful save (0 = never)

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

// Headless save/load round-trip self-test (src/selftest.cpp). Returns 0 = pass, 1 = fail.
int run_selftest();

// Recent files
