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
