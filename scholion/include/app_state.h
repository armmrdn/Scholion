#pragma once
// Shared mutable application state — defined in main.cpp, extern everywhere else.
// Each extracted subsystem includes this header to access the globals it needs.

#include "canvas.h"
#include "document.h"
#include "texture_cache.h"
#include "pdf_loader.h"
#include "app_settings.h"
#include "canvas_text_box.h"
#include "canvas_annot.h"
#include "save_feedback.h"
#include "input.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

extern Canvas                                    g_canvas;
extern InputHandler                              g_input;
extern std::vector<Document>                     g_documents;
extern std::vector<std::shared_ptr<PdfLoader>>   g_loaders;
extern TextureCache                              g_cache;
extern std::unordered_map<std::string, uint32_t> g_tile_cache;
extern float                                     g_content_scale;
extern AppSettings                               g_settings;
extern std::vector<CanvasTextBox>                g_text_boxes;
extern int                                       g_next_box_id;
extern std::vector<PageGroup>                    g_groups;
extern int                                       g_next_group_id;
extern uint64_t                                  g_next_page_id;   // stable page id counter
extern bool                                      g_debug;
extern GLFWwindow*                               g_window;
extern float                                     g_panel_w;

// Text-box editing/selection state (reset on project load/new)
extern int   g_selected_box;
extern int   g_editing_box;
extern int   g_prev_selected_box;
extern int   g_prev_editing_box;
extern bool  g_just_created;
extern bool  g_edit_was_new;

// App-state actions defined in main.cpp (exposed for the self-test + future extraction).
void remove_document(int doc_idx);
struct Page;
Page* page_by_id(uint64_t id);   // resolve a stable page id to a live Page* (nullptr if gone)

// Overlay/search flags + selection helper (defined in main.cpp; consumed by
// extracted modules such as groups.cpp).
extern bool g_settings_open;
extern bool g_search_open;
extern unsigned int g_logo_tex;        // GLuint app-logo texture (settings/startup UI)
std::vector<Page*> selected_pages();   // live Page* for every id in the current selection
