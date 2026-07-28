#pragma once
#include "renderer.h"  // GridMode

struct AppSettings {
    bool     dark_mode   = true;
    GridMode grid_mode   = GridMode::Lines;
    bool     compat_mode = false;
    float    panel_w     = 360.0f;
    bool     vignette_on = true;
    bool     large_ui    = false;   // scale all UI text/buttons up for small screens
};
