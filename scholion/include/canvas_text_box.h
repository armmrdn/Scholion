#pragma once
#include "canvas.h"  // Vec2

struct CanvasTextBox {
    int   id        = 0;
    Vec2  world_pos = {};
    char  text[2048] = {};
    float r = 0.82f, g = 0.06f, b = 0.06f;
    float font_size = 16.0f;
    float w = 0.0f, h = 0.0f;
    bool  zoom_scaled = false;  // false: fixed on-screen size (sticky note). true: font/box
                                // scale with canvas zoom so text stays proportional to the page.
};

// Default style for newly created text boxes — the "template" the toolbar property strip edits
// and text-box creation reads. Defined in main.cpp; shared across the toolbar + text-box code.
extern float g_tbox_r, g_tbox_g, g_tbox_b;   // default color (red, like the pen)
extern float g_tbox_font_size;               // default font size (px)
extern bool  g_tbox_zoom_scaled;             // default: scale text with zoom
