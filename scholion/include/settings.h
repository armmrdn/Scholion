#pragma once
// Settings popup — the modal Settings window (appearance, canvas grid, performance,
// keyboard-shortcut reference, and about/footer). Gated by g_settings_open; edits
// write through to g_settings and persist via save_prefs()/apply_appearance().
// Implementation in src/settings.cpp.

void draw_settings_popup();   // no-op unless g_settings_open; call once per frame
