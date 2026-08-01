#pragma once
// Cross-cutting user actions + platform file pickers, invoked from more than one UI module
// (toolbar today; dialogs / sidebar as they are extracted). Defined in main.cpp and the
// platform layer. Kept in one small header so each extracted module declares nothing platform-
// or action-specific of its own.
#include <string>

// Platform file pickers (defined in the platform layer; extern "C").
extern "C" const char* scholion_open_file(const char* title, const char** ext_patterns,
                                          int n_ext, int allow_multi);
extern "C" const char* scholion_select_folder(const char* title);
extern "C" const char* scholion_save_file(const char* title, const char* default_name,
                                          const char** ext_patterns, int n_ext);

// Canvas navigation + document actions (defined in main.cpp).
struct Page;
void zoom_to_rect(float x0, float y0, float x1, float y1);   // fit a world-space rect to the viewport
void zoom_to_fit();                                          // fit all pages to the viewport (Cmd+0)
Page* next_page_in_order(Page* from, int dir);               // page-order nav helper (arrow keys)
std::string extract_page_text(const std::string& path, int page_idx);  // one-off PdfLoader text pull
bool relink_document(int doc_idx, const std::string& new_path);
void reveal_in_file_manager(const std::string& path);

// PDF import actions (defined in main.cpp).
void load_pdf(const std::string& path);
void load_pdfs_from_selection(const char* selection);
void load_pdfs_from_folder(const std::string& folder);

// "Add from URL" modal — call once per frame; no-op unless its popup was opened (main.cpp).
void draw_url_modal();
