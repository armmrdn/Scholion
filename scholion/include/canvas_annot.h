#pragma once
#include "canvas.h"
#include "document.h"
#include <string>
#include <vector>

enum class AnnotTool { None, Pen, Highlight, Note, Eraser };

// Freehand-highlighter "marker" stroke appearance: thick and translucent so the page/text
// shows through underneath. Used when a highlighter swipe covers no text glyphs.
constexpr float MARKER_HALF_W = 7.0f;   // screen-pixel half-width
constexpr float MARKER_ALPHA  = 0.30f;

// Legacy positional reference note — only used to migrate pre-v1.4 project files (where
// notes were keyed by list index). Notes now live on AnnotHighlight::note. See project_io.
struct RefNote { int after_idx; std::string text; };

// Annotation input state — defined in canvas_annot.cpp
extern AnnotTool   g_annot_tool;
extern bool        g_text_tool;   // text-box tool active (mutually exclusive with g_annot_tool)
extern bool        g_ann_drawing;
extern float       g_pen_r, g_pen_g, g_pen_b;
extern float       g_hl_r, g_hl_g, g_hl_b;   // highlighter (marker) color
extern bool        g_hl_box_mode;            // true = rectangle drag; false = freehand swipe
extern int         g_ann_doc_idx;
extern int         g_ann_page_idx;
extern AnnotStroke g_ann_cur_stroke;
extern Vec2        g_ann_hl_start;
extern Vec2        g_ann_cur_norm;
extern int         g_next_note_idx;

// Canvas (world-space) pen strokes — pen marks started on the empty canvas, drawn in front
// of everything and locked to world coordinates (they pan/zoom with the canvas).
extern std::vector<AnnotStroke> g_canvas_strokes;
extern bool                     g_ann_canvas;   // true while drawing a canvas stroke

// Append a point to the in-progress stroke; when `ortho`, constrain it to a straight
// segment from the stroke's start, snapped to the nearest 45° (Shift ortho-lock).
void        stroke_add_point(const Page& page, Vec2 norm, bool ortho);
// Same, for canvas strokes whose points are already world-space (uniform aspect).
void        stroke_add_point_world(Vec2 world, bool ortho);

std::string note_label(int idx);
Page*       hit_test_page(float sx, float sy, int* out_doc_idx = nullptr);
Vec2        screen_to_page_norm(const Page& page, float sx, float sy);
void        finalize_annotation();
void        draw_canvas_annotation_preview();
void        update_canvas_annotations();
