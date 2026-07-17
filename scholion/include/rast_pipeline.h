#pragma once
#include "document.h"
#include "pdf_loader.h"
#include <memory>
#include <string>

// Lifecycle
void rast_init();
void rast_shutdown();

// Per-frame
bool drain_rast_results();
void stream_lod(bool vram_changed);

// True while the background rasterizer still has queued, in-flight, or
// ready-but-not-yet-uploaded work. The main loop uses this to keep the render
// loop awake (loading shimmer, prompt tile uploads) instead of idling mid-load.
bool rast_pending();

// Called from load_pdf and panel interactions
void enqueue_rast(const std::string& doc_path,
                  std::shared_ptr<PdfLoader> loader,
                  int page_index, LodTier tier);

// Called when a document is removed or a page is replaced
void evict_page_tiles(const std::string& doc_path, int page_index);
void cancel_page_tile_tasks(const std::string& doc_path, int page_index);
bool page_has_tiles(const std::string& doc_path, int page_index);

// Cancel all pending tasks for one document (called by remove_document)
void rast_cancel_doc(const std::string& doc_path);
// Cancel and drain all pending tasks (called by clear_documents)
void rast_cancel_all();

// Visibility tests used by the main loop and renderer
bool is_page_visible(const Page& page);
bool is_world_rect_visible(float wx, float wy, float ww, float wh);
