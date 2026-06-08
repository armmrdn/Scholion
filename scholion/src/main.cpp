#include "canvas.h"
#include "input.h"
#include "renderer.h"
#include "overlay.h"
#include "pdf_loader.h"
#include "texture_cache.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
extern "C" void scholion_register_early(void);
extern "C" void scholion_register_file_handler(void);
extern "C" int  scholion_pop_pending_open(char* buf, int buf_len);
extern "C" void scholion_activate_app(void);
#include <unistd.h>   // fork / execl
#elif defined(_WIN32)
// Windows: GLAD must be included before GLFW so it wins the GL symbol race.
// GLFW_INCLUDE_NONE prevents GLFW from pulling in its own GL headers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW (reveal in Explorer)
#include <shlobj.h>     // SHGetKnownFolderPath, FOLDERID_*
#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#endif

#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "tinyfiledialogs.h"
#include <curl/curl.h>

// --- Document colors (cycled per document) ----------------------------------

static const float DOC_PALETTE[][3] = {
    {0.30f, 0.55f, 0.85f},
    {0.85f, 0.40f, 0.35f},
    {0.35f, 0.75f, 0.45f},
    {0.80f, 0.70f, 0.30f},
    {0.65f, 0.35f, 0.75f},
};
static constexpr int PALETTE_SIZE = 5;

// --- Globals for GLFW callbacks (GLFW C callbacks can't capture state) ------

static Canvas       g_canvas;
static InputHandler g_input(g_canvas);

static GLFWcursor* g_cursor_hand  = nullptr;
static GLFWcursor* g_cursor_arrow = nullptr;

static GLFWwindow*                              g_window  = nullptr;
static std::vector<Document>                    g_documents;
static std::vector<std::shared_ptr<PdfLoader>>  g_loaders;   // one per document, same index
static TextureCache                             g_cache;

// --- Background rasterization -----------------------------------------------
// Pipeline: enqueue_rast() → g_rast_tasks (work queue) → rast_worker() (one
// background thread) → g_rast_ready (result queue) → drain_rast_results() on
// the main thread, which calls TextureCache::upload() (GL call).
//
// MuPDF is not thread-safe; each document has its own PdfLoader. The shared_ptr
// in RastTask keeps the loader alive even if remove_document() runs concurrently.
// GL calls are forbidden on the worker — rasterize_to_buffer() returns raw RGBA;
// the main thread uploads after draining.
// glfwPostEmptyEvent() wakes the main loop (blocked on glfwWaitEventsTimeout) so
// newly-rasterized pages render in the next frame, not up to 16 ms later.
// g_rast_inflight deduplicates tasks so the same page:tier pair isn't queued twice.

struct RastTask {
    std::shared_ptr<PdfLoader> loader;
    std::string doc_path;
    int         page_index = 0;
    LodTier     tier       = LodTier::Thumb;
};
struct RastResult {
    std::string              doc_path;
    int                      page_index = 0;
    LodTier                  tier       = LodTier::Thumb;
    PdfLoader::RasterBuffer  buf;
};

static std::mutex              g_rast_mutex;
static std::condition_variable g_rast_cv;
static std::vector<RastTask>   g_rast_tasks;
static std::vector<RastResult> g_rast_ready;
static std::unordered_set<std::string> g_rast_inflight;   // "path:page:tier" keys
static std::atomic<bool>       g_rast_stop{false};
static std::thread             g_rast_thread;

static std::string rast_key(const std::string& path, int pg, LodTier tier) {
    return path + '\0' + std::to_string(pg) + '\0' + std::to_string((int)tier);
}

static void rast_worker() {
    while (true) {
        RastTask task;
        {
            std::unique_lock<std::mutex> lk(g_rast_mutex);
            g_rast_cv.wait(lk, []{ return g_rast_stop || !g_rast_tasks.empty(); });
            if (g_rast_stop && g_rast_tasks.empty()) break;
            task = std::move(g_rast_tasks.front());
            g_rast_tasks.erase(g_rast_tasks.begin());
            g_rast_inflight.erase(rast_key(task.doc_path, task.page_index, task.tier));
        }
        auto buf = task.loader->rasterize_to_buffer(task.page_index, task.tier);
        if (buf.ok) {
            {
                std::lock_guard<std::mutex> lk(g_rast_mutex);
                g_rast_ready.push_back({task.doc_path, task.page_index, task.tier, std::move(buf)});
            }
            glfwPostEmptyEvent();   // wake main loop so the frame renders promptly
        }
    }
}

// Enqueue a rasterization task, skipping duplicates.
static void enqueue_rast(const std::string& doc_path,
                         std::shared_ptr<PdfLoader> loader,
                         int page_index, LodTier tier) {
    std::string key = rast_key(doc_path, page_index, tier);
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    if (g_rast_inflight.count(key)) return;
    g_rast_inflight.insert(key);
    g_rast_tasks.push_back({loader, doc_path, page_index, tier});
    g_rast_cv.notify_one();
}

// Upload all completed pixel buffers to the GPU (main thread only).
static void drain_rast_results() {
    std::vector<RastResult> ready;
    {
        std::lock_guard<std::mutex> lk(g_rast_mutex);
        ready.swap(g_rast_ready);
    }
    for (auto& res : ready) {
        // Find the page — document may have been removed since task was queued
        Page* target = nullptr;
        for (auto& doc : g_documents) {
            if (doc.path != res.doc_path) continue;
            for (auto& page : doc.pages)
                if (page.page_index == res.page_index) { target = &page; break; }
            break;
        }
        if (!target || !target->needs_lod(res.tier)) continue;
        g_cache.upload(*target, res.tier, res.buf.pixels.data(), res.buf.width, res.buf.height);
    }
}

// --- Annotation tool state --------------------------------------------------
enum class AnnotTool { None, Pen, Highlight, Note, Eraser };
static AnnotTool   g_annot_tool   = AnnotTool::None;
static bool        g_ann_drawing  = false;
static float       g_pen_r = 0.82f, g_pen_g = 0.06f, g_pen_b = 0.06f;
static int         g_ann_doc_idx  = -1;
static int         g_ann_page_idx = -1;
static AnnotStroke g_ann_cur_stroke;
static Vec2        g_ann_hl_start = {};
static Vec2        g_ann_cur_norm = {};

// Global note index: increments each time a note is stamped.
// label sequence: A–Z (0–25), 2A–2Z (26–51), 3A–3Z (52–77), …
static int g_next_note_idx = 0;

static std::string note_label(int idx) {
    int  prefix = idx / 26;
    char letter  = 'A' + (idx % 26);
    if (prefix == 0) return std::string(1, letter);
    return std::to_string(prefix + 1) + letter;
}

// --- Save notification feedback -----------------------------------------------
enum class SaveFeedbackType { None, Manual, Auto };
static SaveFeedbackType g_save_feedback_type = SaveFeedbackType::None;
static std::chrono::steady_clock::time_point g_save_feedback_time;

// --- Application settings (persisted to ~/.scholion_prefs) -------------------
struct AppSettings {
    bool     dark_mode   = true;
    GridMode grid_mode   = GridMode::Lines;
    bool     compat_mode = false;
    float    panel_w     = 360.0f;          // persisted sidebar width
    bool     vignette_on = true;
};
static AppSettings g_settings;
static bool g_settings_open = false;

static GLuint g_vignette_tex = 0;  // elliptical gradient texture, created once after GL init

// --- Quit confirmation -------------------------------------------------------
static bool g_quit_requested = false;
enum class QuitState { None, Waiting, Confirmed };
static QuitState g_quit_state = QuitState::None;

// --- Panel width (shared between panel and resize handle) -------------------
static float s_panel_w = 360.0f;
static int s_panel_nav_page = 0;    // current page index in open panel (0-based)
static Page* g_nav_focus = nullptr; // focused page for arrow navigation (when panel closed)

// --- Canvas text boxes -------------------------------------------------------
// w/h are the box's on-screen size in pixels (0 = auto-size to text, used by
// bare-click and legacy boxes). Text wraps to w; h is a floor that grows to fit.
struct CanvasTextBox { int id; Vec2 world_pos; char text[2048]; float r=0.82f, g=0.06f, b=0.06f; float font_size=16.0f; float w=0.0f; float h=0.0f; };
static std::vector<CanvasTextBox> g_text_boxes;
static int  g_next_box_id    = 0;
static int  g_selected_box   = -1;  // id, -1 = none
static int  g_editing_box    = -1;  // id, -1 = not editing
static bool  g_text_tool      = false;
static float g_tbox_r = 0.82f, g_tbox_g = 0.06f, g_tbox_b = 0.06f;  // default red, like the pen
static float g_tbox_font_size = 16.0f;
static int  g_hovered_box    = -1;  // id under cursor (set per-frame); -1 = none
static bool g_box_dragging    = false;
struct TextBoxDragState { int id; Vec2 initial_pos; };
static std::vector<TextBoxDragState> g_box_drag_states = {};
struct PageDragState { Page* page; Vec2 initial_pos; };
static std::vector<PageDragState>    g_page_drag_states = {};
static Vec2 g_box_drag_start_world = {};  // world position where drag began (grab point)

// Press-drag-release creation of a new text box (text tool active)
static bool g_tbox_creating     = false;
static Vec2 g_tbox_create_start = {};   // screen pos where the drag began
static bool g_just_created      = false; // set on create; consumed by edit-session tracking

// Entity clipboard for Cmd+C / Cmd+V duplication of selected boxes
static CanvasTextBox g_clip_box   = {};
static bool          g_clip_valid = false;

// Undo session tracking — one record per editing session (text) and per
// selection session (style). Snapshots taken on begin, compared on end.
static int   g_prev_editing_box  = -1;
static char  g_edit_text0[2048]  = "";
static bool  g_edit_was_new      = false;
static int   g_prev_selected_box = -1;
static float g_style_r0 = 0, g_style_g0 = 0, g_style_b0 = 0, g_style_fs0 = 0;

// --- Undo stack -------------------------------------------------------------
// Each record is typed; the type determines which fields are meaningful.
//
// PageMove batches all pages moved in one drag into a single record so one
// Cmd+Z undoes the whole group.  TextBoxMove is one record per box (boxes are
// independently identified by ID, not by index).
//
// TextBoxEdit: snapshot taken when an editing session opens (double-click or
// just-created); compared and pushed when it closes (ESC or click-off). No
// record if the text didn't change.
//
// TextBoxStyle: snapshot taken on the first single-click that selects a box;
// pushed when the selection changes or the tool exits. No record if style
// didn't change.
//
// UNDO_LIMIT caps the stack at 60 records; oldest are dropped when exceeded.
struct UndoRecord {
    enum class Type {
        PenStroke, Highlight, Note,
        PageMove, PageResize,
        TextBoxCreate, TextBoxMove, TextBoxDelete,
        TextBoxEdit, TextBoxStyle,
        ErasedStroke, ErasedHighlight,
        ErasedNote,
        DocumentRemove
    };
    Type type = Type::PenStroke;

    // Annotation + eraser ops
    int doc_idx         = -1;
    int page_idx        = -1;
    int note_idx_before = -1;   // g_next_note_idx value before the note was stamped

    // Page move / resize (single or group)
    struct PagePos { Page* page; Vec2 old_pos; float old_w = 0.0f; float old_h = 0.0f; };
    std::vector<PagePos> page_moves;

    // Text box ops
    int           box_id       = -1;
    Vec2          old_box_pos  = {};
    CanvasTextBox deleted_box  = {};   // full copy for TextBoxDelete / TextBoxCreate rollback
    std::string   prev_text;           // TextBoxEdit — text before the edit session
    float old_r = 0, old_g = 0, old_b = 0, old_fs = 0;  // TextBoxStyle — style before change

    // Erased annotations (stored so they can be re-inserted)
    AnnotStroke    erased_stroke    = {};
    AnnotHighlight erased_highlight = {};
    AnnotNote      erased_note      = {};
    int            erased_note_at   = -1;  // index within page.annots.notes

    // Document removal (stores full document copy for undo)
    int                        removed_doc_idx = -1;
    Document                   removed_doc     = {};
    std::shared_ptr<PdfLoader> removed_loader;
};

static std::vector<UndoRecord> g_undo_stack;
static constexpr int           UNDO_LIMIT = 60;

static void push_undo(UndoRecord r) {
    g_undo_stack.push_back(std::move(r));
    if ((int)g_undo_stack.size() > UNDO_LIMIT)
        g_undo_stack.erase(g_undo_stack.begin());
}

static void undo_last() {
    if (g_undo_stack.empty()) return;
    UndoRecord r = std::move(g_undo_stack.back());
    g_undo_stack.pop_back();

    auto safe_page = [&]() -> Page* {
        if (r.doc_idx  < 0 || r.doc_idx  >= (int)g_documents.size()) return nullptr;
        auto& doc = g_documents[r.doc_idx];
        if (r.page_idx < 0 || r.page_idx >= (int)doc.pages.size())   return nullptr;
        return &doc.pages[r.page_idx];
    };

    switch (r.type) {
        case UndoRecord::Type::PenStroke:
            if (auto* p = safe_page(); p && !p->annots.strokes.empty())
                p->annots.strokes.pop_back();
            break;
        case UndoRecord::Type::Highlight:
            if (auto* p = safe_page(); p && !p->annots.highlights.empty())
                p->annots.highlights.pop_back();
            break;
        case UndoRecord::Type::Note:
            if (auto* p = safe_page(); p && !p->annots.notes.empty()) {
                p->annots.notes.pop_back();
                g_next_note_idx = r.note_idx_before;
            }
            break;
        case UndoRecord::Type::PageMove:
            for (auto& pm : r.page_moves)
                if (pm.page) pm.page->world_pos = pm.old_pos;
            break;
        case UndoRecord::Type::PageResize:
            for (auto& pm : r.page_moves)
                if (pm.page) { pm.page->world_w = pm.old_w; pm.page->world_h = pm.old_h; }
            break;
        case UndoRecord::Type::TextBoxCreate:
            g_text_boxes.erase(
                std::remove_if(g_text_boxes.begin(), g_text_boxes.end(),
                               [&](const CanvasTextBox& b){ return b.id == r.box_id; }),
                g_text_boxes.end());
            if (g_selected_box == r.box_id) g_selected_box = -1;
            if (g_editing_box  == r.box_id) g_editing_box  = -1;
            break;
        case UndoRecord::Type::TextBoxMove:
            for (auto& b : g_text_boxes)
                if (b.id == r.box_id) { b.world_pos = r.old_box_pos; break; }
            break;
        case UndoRecord::Type::TextBoxDelete:
            g_text_boxes.push_back(r.deleted_box);
            break;
        case UndoRecord::Type::TextBoxEdit:
            for (auto& b : g_text_boxes)
                if (b.id == r.box_id) {
                    strncpy(b.text, r.prev_text.c_str(), sizeof(b.text) - 1);
                    b.text[sizeof(b.text) - 1] = '\0';
                    break;
                }
            break;
        case UndoRecord::Type::TextBoxStyle:
            for (auto& b : g_text_boxes)
                if (b.id == r.box_id) {
                    b.r = r.old_r; b.g = r.old_g; b.b = r.old_b; b.font_size = r.old_fs;
                    break;
                }
            break;
        case UndoRecord::Type::ErasedStroke:
            if (auto* p = safe_page())
                p->annots.strokes.push_back(r.erased_stroke);
            break;
        case UndoRecord::Type::ErasedHighlight:
            if (auto* p = safe_page())
                p->annots.highlights.push_back(r.erased_highlight);
            break;
        case UndoRecord::Type::ErasedNote:
            if (auto* p = safe_page()) {
                int at = std::clamp(r.erased_note_at, 0, (int)p->annots.notes.size());
                p->annots.notes.insert(p->annots.notes.begin() + at, r.erased_note);
            }
            break;
        case UndoRecord::Type::DocumentRemove: {
            int idx = std::clamp(r.removed_doc_idx, 0, (int)g_documents.size());
            g_documents.insert(g_documents.begin() + idx, r.removed_doc);
            g_loaders.insert(g_loaders.begin() + idx, r.removed_loader);
            g_input.set_documents(g_documents.empty() ? nullptr : &g_documents);
            break;
        }
    }
}

// --- Drag state machine / undo tracking -------------------------------------
// Three overlapping systems cooperate to move pages and text boxes:
//
//  1. Single-page deferred drag (InputHandler): m_drag_pending_page set at
//     mousedown; m_drag_active fires after 4-px threshold. Positions updated
//     in on_cursor_move. g_drag_snap_page/pos capture start for undo.
//
//  2. Multi-page group drag (InputHandler): m_multi_drag_active + m_drag_origins.
//     Activated when a selected page is pressed or after modifier-click.
//     g_multi_drag_snaps snapshots all positions at drag start.
//
//  3. Text-box group drag (main.cpp): g_box_dragging + g_box_drag_states +
//     g_page_drag_states. Activated when a text box is pressed. g_page_drag_states
//     moves selected pages alongside boxes, bypassing m_multi_drag_active to avoid
//     the glfwPollEvents/ImGui frame-boundary race: GLFW RELEASE fires during
//     glfwPollEvents (synchronously); ImGui IsMouseClicked fires in the next frame.
//     Setting m_multi_drag_active from an ImGui handler can arrive after its
//     RELEASE and never clear — causing pages to stick to the cursor.
//
//  When a page-initiated drag (system 2) starts and text boxes are also selected,
//  the main loop detects the !g_was_multi_drag → cur_multi transition and sets
//  g_box_dragging = true so boxes follow the pages.
//
//  Raw Page* stored here are purged in remove_document() before the page vector
//  is erased to prevent dangling-pointer crashes on Cmd+Z after a removal.
static const Page* g_drag_snap_page    = nullptr;
static Vec2        g_drag_snap_pos     = {};
static bool        g_was_page_dragging = false;
static bool        g_was_multi_drag    = false;
struct PageSnap { Page* page; Vec2 old_pos; };
static std::vector<PageSnap> g_multi_drag_snaps;

// --- Signal flag (set by SIGINT/SIGTERM; checked in main loop) ---------------
static volatile sig_atomic_t g_signal_received = 0;

// --- URL download state -----------------------------------------------------
// 0 = idle, 1 = in-flight, 2 = done (success), 3 = failed
// The thread writes g_dl_path / g_dl_error before storing to g_dl_state
// (memory_order_release), so the main thread safely reads them after a
// memory_order_acquire load that observes the new state.

static char             g_url_buf[2048]  = {};
static std::atomic<int> g_dl_state{0};
static std::atomic<bool>g_dl_cancel{false};
static std::string      g_dl_path;
static std::string      g_dl_error;
static std::thread      g_dl_thread;

// --- Search state -----------------------------------------------------------
static bool g_search_open = false;
static bool g_debug       = false;  // verbose save/load logging; set from SCHOLION_DEBUG env
static char g_search_buf[256] = {};
struct SearchResult {
    int         doc_idx;
    int         page_idx;
    std::string excerpt;
    std::string doc_name;
};
static std::vector<SearchResult> g_search_results;
static int g_highlighted_search_result = -1;  // index of the currently highlighted search result
static std::string g_last_search_term;         // track if search term changed to clear highlight

// --- Helpers ----------------------------------------------------------------

static Vec2 next_doc_origin() {
    static constexpr int   PER_ROW   = 2;
    static constexpr float COL_STEP  = 700.0f;
    static constexpr float ROW_STEP  = 1100.0f;
    static constexpr float START_Y   = -396.0f;

    int   idx  = static_cast<int>(g_documents.size());
    int   col  = idx % PER_ROW;
    int   row  = idx / PER_ROW;
    float left = -(PER_ROW - 1) * COL_STEP * 0.5f;
    return {left + col * COL_STEP, START_Y + row * ROW_STEP};
}

static void load_pdf(const std::string& path) {
    // Prevent duplicate loads
    for (const auto& d : g_documents)
        if (d.path == path) { printf("Already loaded: %s\n", path.c_str()); return; }

    auto loader = std::make_shared<PdfLoader>();
    if (!loader->valid()) {
        fprintf(stderr, "PdfLoader init failed for: %s\n", path.c_str());
        return;
    }

    Document doc;
    if (!loader->load(path, doc, next_doc_origin())) {
        fprintf(stderr, "Failed to load PDF: %s\n", path.c_str());
        return;
    }

    int idx = static_cast<int>(g_documents.size());
    auto& c = DOC_PALETTE[idx % PALETTE_SIZE];
    doc.hue_r = c[0]; doc.hue_g = c[1]; doc.hue_b = c[2];

    // Register document immediately so it appears on canvas; pages show as
    // warm-white placeholders until background rasterization completes.
    printf("  Queuing %zu pages for background rasterization\xe2\x80\xa6\n", doc.pages.size());
    for (const auto& page : doc.pages)
        enqueue_rast(path, loader, page.page_index, LodTier::Thumb);

    g_documents.push_back(std::move(doc));
    g_loaders.push_back(loader);
    g_input.set_documents(&g_documents);
}

static void clear_documents() {
    {
        std::lock_guard<std::mutex> lk(g_rast_mutex);
        g_rast_tasks.clear();
        g_rast_inflight.clear();
        g_rast_ready.clear();
    }
    for (auto& doc : g_documents)
        for (auto& page : doc.pages)
            g_cache.evict(page);
    g_documents.clear();
    g_loaders.clear();
    g_cache.clear();
    g_input.set_documents(nullptr);
}

static void new_project();          // defined after g_project_path
static void save_project_current(); // Cmd+S handler; defined after save_project

static void zoom_to_rect(float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return;
    float vw   = g_canvas.get_viewport_width();
    float vh   = g_canvas.get_viewport_height();
    float zoom = std::min(vw / (x1 - x0), vh / (y1 - y0)) * 0.88f;
    g_canvas.set_zoom(zoom);
    g_canvas.set_offset({(x0 + x1) * 0.5f, (y0 + y1) * 0.5f});
}

static void zoom_to_fit() {
    if (g_documents.empty()) return;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const auto& doc : g_documents)
        for (const auto& page : doc.pages) {
            x0 = std::min(x0, page.world_pos.x);
            y0 = std::min(y0, page.world_pos.y);
            x1 = std::max(x1, page.world_pos.x + page.world_w);
            y1 = std::max(y1, page.world_pos.y + page.world_h);
        }
    zoom_to_rect(x0, y0, x1, y1);
}

// Arrow key selection navigation: find next/prev page in document order.
// Iterates all docs sorted by z_layer, flattens pages, finds current page, returns ±1 neighbor.
static Page* next_page_in_order(Page* from, int dir) {
    if (!from) return nullptr;
    std::vector<int> order(g_documents.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return g_documents[a].z_layer < g_documents[b].z_layer;
    });

    std::vector<Page*> all_pages;
    for (int di : order)
        for (auto& page : g_documents[di].pages)
            all_pages.push_back(&page);

    for (size_t i = 0; i < all_pages.size(); ++i) {
        if (all_pages[i] == from) {
            int next_idx = static_cast<int>(i) + dir;
            if (next_idx >= 0 && next_idx < (int)all_pages.size())
                return all_pages[next_idx];
            return nullptr;
        }
    }
    return nullptr;
}

static void remove_document(int doc_idx) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return;

    // Collect raw Page* addresses before the vector is erased so we can purge
    // every data structure that stores raw pointers into it.  Must happen first.
    std::unordered_set<const Page*> removing;
    for (const auto& page : g_documents[doc_idx].pages)
        removing.insert(&page);

    // Undo records of type PageMove/PageResize store raw Page*.  If any entry in
    // a record points into the document being removed the whole record is unsafe —
    // drop it rather than leaving a dangling pointer that would crash on Cmd+Z.
    g_undo_stack.erase(
        std::remove_if(g_undo_stack.begin(), g_undo_stack.end(),
            [&](const UndoRecord& r) {
                for (const auto& pm : r.page_moves)
                    if (removing.count(pm.page)) return true;
                return false;
            }),
        g_undo_stack.end());

    // Per-frame drag-snapshot globals also hold raw Page* — clear them.
    if (removing.count(g_drag_snap_page)) g_drag_snap_page = nullptr;
    g_multi_drag_snaps.erase(
        std::remove_if(g_multi_drag_snaps.begin(), g_multi_drag_snaps.end(),
            [&](const PageSnap& s){ return removing.count(s.page); }),
        g_multi_drag_snaps.end());

    const std::string& rpath = g_documents[doc_idx].path;
    {
        std::lock_guard<std::mutex> lk(g_rast_mutex);
        g_rast_tasks.erase(
            std::remove_if(g_rast_tasks.begin(), g_rast_tasks.end(),
                [&](const RastTask& t){ return t.doc_path == rpath; }),
            g_rast_tasks.end());
        for (auto it = g_rast_inflight.begin(); it != g_rast_inflight.end(); )
            it = (it->rfind(rpath + '\0', 0) == 0) ? g_rast_inflight.erase(it) : std::next(it);
    }
    for (auto& page : g_documents[doc_idx].pages)
        g_cache.evict(page);
    g_documents.erase(g_documents.begin() + doc_idx);
    g_loaders.erase(g_loaders.begin() + doc_idx);
    // Adjust panel state
    if (g_input.panel_open()) {
        int pi = g_input.panel_doc_index();
        if (pi == doc_idx)     g_input.close_panel();
        else if (pi > doc_idx) g_input.open_panel(pi - 1, g_input.panel_scroll_page());
    }
    g_input.clear_selection();
    g_input.set_documents(g_documents.empty() ? nullptr : &g_documents);
}

// Point a missing-PDF placeholder at the real file: load it into the same slot,
// carrying over the placeholder's page positions and annotations (matched by page
// index), then queue rasterization. Returns false if the new file won't load.
static bool relink_document(int doc_idx, const std::string& new_path) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return false;

    auto loader = std::make_shared<PdfLoader>();
    if (!loader->valid()) { fprintf(stderr, "relink: loader init failed\n"); return false; }

    Document& old = g_documents[doc_idx];
    Document  fresh;
    if (!loader->load(new_path, fresh, old.stack_origin)) {
        fprintf(stderr, "relink: failed to load %s\n", new_path.c_str());
        return false;
    }

    // Carry over saved positions + annotations onto the matching real pages.
    for (auto& fp : fresh.pages)
        for (auto& op : old.pages)
            if (op.page_index == fp.page_index) {
                fp.world_pos = op.world_pos;
                fp.annots    = std::move(op.annots);
                break;
            }

    fresh.stack_origin = old.stack_origin;
    fresh.hue_r = old.hue_r; fresh.hue_g = old.hue_g; fresh.hue_b = old.hue_b;
    fresh.missing = false;
    fresh.path    = new_path;   // remember the found location for future saves

    g_documents[doc_idx] = std::move(fresh);
    g_loaders[doc_idx]   = loader;

    for (const auto& page : g_documents[doc_idx].pages)
        enqueue_rast(g_documents[doc_idx].path, loader, page.page_index, LodTier::Thumb);

    g_input.clear_selection();                 // old Page* pointers are now invalid
    g_input.set_documents(&g_documents);
    printf("Relinked document %d → %s\n", doc_idx, new_path.c_str());
    return true;
}

// Returns true if the page's screen-space bounding box overlaps the viewport.
static bool is_page_visible(const Page& page) {
    Vec2 tl = g_canvas.world_to_screen({page.world_pos.x,               page.world_pos.y});
    Vec2 br = g_canvas.world_to_screen({page.world_pos.x + page.world_w, page.world_pos.y + page.world_h});
    float vw = g_canvas.get_viewport_width();
    float vh = g_canvas.get_viewport_height();
    return br.x >= 0.0f && tl.x <= vw && br.y >= 0.0f && tl.y <= vh;
}

// Called once per frame: enqueues rasterization for the zoom-appropriate LOD
// tier of each visible page and evicts tiers that are no longer needed.
//
// Eviction rules:
//   High tier (~17 MB/page) — evicted unconditionally when off-screen or zoom
//     drops below 2.5. Left alive, off-screen pages would exhaust VRAM quickly.
//   Low tier  (~4.3 MB/page) — evicted only when VRAM exceeds 350 MB so it
//     survives brief zoom-out/zoom-in cycles without a re-rasterize stall.
//   Thumb tier (~1.1 MB/page) — never evicted; always-available fallback.
//
// g_loaders[di] == nullptr for missing-PDF placeholder documents — those slots
// are skipped to avoid calling enqueue_rast for a document with no real file.
static constexpr size_t VRAM_BUDGET = 350ULL * 1024 * 1024;

static void stream_lod() {
    if (g_documents.empty()) return;

    LodTier target = lod_for_zoom(g_canvas.get_zoom());
    if (g_settings.compat_mode && target == LodTier::High)
        target = LodTier::Low;

    for (int di = 0; di < (int)g_documents.size(); ++di) {
        auto& doc    = g_documents[di];
        auto& loader = g_loaders[di];
        if (!loader) continue;   // placeholder (missing PDF) — nothing to rasterize

        // Don't evict High tier for pages in the currently open panel document.
        // The panel independently loads High tier (300 DPI) regardless of canvas zoom.
        bool is_panel_doc = g_input.panel_open() && (di == g_input.panel_doc_index());

        for (auto& page : doc.pages) {
            bool vis = is_page_visible(page);

            if (vis) {
                if (page.needs_lod(target))
                    enqueue_rast(doc.path, loader, page.page_index, target);
                if (!is_panel_doc && target < LodTier::High && page.tex_high)
                    g_cache.evict_tier(page, LodTier::High);
                if (target < LodTier::Low  && page.tex_low)
                    g_cache.evict_tier(page, LodTier::Low);
            } else {
                if (!is_panel_doc && page.tex_high)
                    g_cache.evict_tier(page, LodTier::High);
                if (page.tex_low && g_cache.vram_bytes() > VRAM_BUDGET)
                    g_cache.evict_tier(page, LodTier::Low);
            }
        }
    }
}

static void load_pdfs_from_folder(const std::string& folder) {
    namespace fs = std::filesystem;
    std::vector<fs::path> pdfs;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".pdf" || ext == ".PDF")
            pdfs.push_back(entry.path());
    }
    std::sort(pdfs.begin(), pdfs.end());
    for (const auto& p : pdfs) {
        printf("Loading: %s\n", p.c_str());
        load_pdf(p.string());
    }
}

static void load_pdfs_from_selection(const char* selection) {
    if (!selection) return;
    std::string s(selection);
    size_t start = 0, pos;
    while ((pos = s.find('|', start)) != std::string::npos) {
        load_pdf(s.substr(start, pos - start));
        start = pos + 1;
    }
    load_pdf(s.substr(start));
}

// --- URL download -----------------------------------------------------------

// Returns the last path segment of a URL as a filename, with .pdf ensured.
static std::string filename_from_url(const std::string& url) {
    auto q = url.find('?');
    std::string path = (q != std::string::npos) ? url.substr(0, q) : url;
    auto slash = path.rfind('/');
    std::string name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
    if (name.empty()) name = "download";
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".pdf")
        name += ".pdf";
    return name;
}

// Returns the user's Downloads folder if it exists, otherwise a temp directory.
static std::string download_dir() {
#ifdef _WIN32
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &wpath))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &result[0], len, nullptr, nullptr);
        CoTaskMemFree(wpath);
        if (std::filesystem::is_directory(result)) return result;
    }
    char temp[MAX_PATH]; GetTempPathA(MAX_PATH, temp);
    return std::string(temp);
#else
    if (const char* home = std::getenv("HOME")) {
        std::string dl = std::string(home) + "/Downloads";
        if (std::filesystem::is_directory(dl)) return dl;
    }
    const char* tmp = std::getenv("TMPDIR");
    return tmp ? std::string(tmp) : "/tmp";
#endif
}

static size_t curl_write_cb(void* data, size_t sz, size_t n, void* fp) {
    return fwrite(data, sz, n, static_cast<FILE*>(fp));
}

// Progress callback — returns 1 to abort if g_dl_cancel is set.
static int curl_progress_cb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_dl_cancel.load(std::memory_order_relaxed) ? 1 : 0;
}

static void download_thread_fn(std::string url, std::string dest) {
    FILE* fp = fopen(dest.c_str(), "wb");
    if (!fp) {
        g_dl_error = "Cannot create file: " + dest;
        g_dl_state.store(3, std::memory_order_release);
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp); std::remove(dest.c_str());
        g_dl_error = "curl init failed";
        g_dl_state.store(3, std::memory_order_release);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL,              url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        "Scholion/0.3");
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);

    CURLcode res  = curl_easy_perform(curl);
    long     code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    fclose(fp);

    // Cancelled by the user
    if (g_dl_cancel.load(std::memory_order_relaxed)) {
        std::remove(dest.c_str());
        return;   // caller handles state reset after join
    }

    if (res != CURLE_OK) {
        g_dl_error = curl_easy_strerror(res);
        std::remove(dest.c_str());
        g_dl_state.store(3, std::memory_order_release);
        return;
    }
    if (code >= 400) {
        g_dl_error = "HTTP " + std::to_string(code);
        std::remove(dest.c_str());
        g_dl_state.store(3, std::memory_order_release);
        return;
    }

    // Verify PDF magic
    FILE* chk = fopen(dest.c_str(), "rb");
    if (chk) {
        char magic[4] = {};
        fread(magic, 1, 4, chk);
        fclose(chk);
        if (memcmp(magic, "%PDF", 4) != 0) {
            g_dl_error = "Response is not a PDF (server may require a login)";
            std::remove(dest.c_str());
            g_dl_state.store(3, std::memory_order_release);
            return;
        }
    }

    g_dl_state.store(2, std::memory_order_release);
}

// Abort an in-flight download and block until the thread exits.
static void cancel_download() {
    g_dl_cancel.store(true, std::memory_order_relaxed);
    if (g_dl_thread.joinable()) g_dl_thread.join();
    g_dl_cancel.store(false, std::memory_order_relaxed);
    g_dl_state.store(0, std::memory_order_relaxed);
}

// --- Signal handler ---------------------------------------------------------

static void scholion_signal_handler(int sig) {
    g_signal_received = sig;  // main loop checks this and exits cleanly
}

// --- GLFW callbacks ---------------------------------------------------------

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static int g_doc_z_counter = 0;

static void mouse_button_callback(GLFWwindow* w, int button, int action, int mods) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (g_hovered_box >= 0) return;  // text box owns this click (one-frame lookahead)
    bool was_box_sel = g_input.box_selecting();
    g_input.on_mouse_button(w, button, action, mods);

    // Bring clicked page's document to front
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        const Page* pressed = g_input.pending_drag_page();
        if (pressed) {
            for (auto& doc : g_documents) {
                for (const auto& page : doc.pages) {
                    if (&page == pressed) {
                        doc.z_layer = ++g_doc_z_counter;
                        goto done_z;
                    }
                }
            }
            done_z:;
        }
    }

    // After rubber-band ends, also add text boxes whose world rect is inside the selection rect.
    // box_start/cur_world remain valid after finalize_box_selection clears m_box_selecting.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE
            && was_box_sel && !g_input.box_selecting()) {
        float x0 = std::min(g_input.box_start_world().x, g_input.box_cur_world().x);
        float y0 = std::min(g_input.box_start_world().y, g_input.box_cur_world().y);
        float x1 = std::max(g_input.box_start_world().x, g_input.box_cur_world().x);
        float y1 = std::max(g_input.box_start_world().y, g_input.box_cur_world().y);
        float zoom = g_canvas.get_zoom();
        auto& sel = const_cast<std::unordered_set<int>&>(g_input.selected_text_boxes());
        for (const auto& box : g_text_boxes) {
            float bx0 = box.world_pos.x,  by0 = box.world_pos.y;
            // box.w/h are screen px; world extent = screen px / zoom.
            // h==0 means auto-height (unknown without rendering) — check top-left only.
            float bx1 = (box.w > 0.0f) ? bx0 + box.w / zoom : bx0;
            float by1 = (box.h > 0.0f) ? by0 + box.h / zoom : by0;
            if (bx0 >= x0 && by0 >= y0 && bx1 <= x1 && by1 <= y1)
                sel.insert(box.id);
        }
    }

    // Update g_nav_focus when selection becomes a single page
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        const auto& sel = g_input.selection();
        if (sel.size() == 1 && !g_input.panel_open()) {
            g_nav_focus = *sel.begin();
        }
    }
}

static void cursor_pos_callback(GLFWwindow* w, double x, double y) {
    if (ImGui::GetIO().WantCaptureMouse) { g_input.clear_hover(); return; }
    g_input.on_cursor_move(w, x, y);
}

static void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    g_input.on_scroll(w, xoff, yoff);
}

// Opens a fresh per-call PdfLoader to extract text — avoids racing with the
// background rast thread which owns the shared g_loaders instances.
static std::string extract_page_text(const std::string& path, int page_idx) {
    PdfLoader tmp;
    Document  dummy;
    if (!tmp.load(path, dummy)) return {};
    return tmp.extract_text(page_idx);
}

static void key_callback(GLFWwindow* w, int key, int scancode, int action, int mods) {
    bool super = (mods & GLFW_MOD_SUPER) != 0;
    bool ctrl  = (mods & GLFW_MOD_CONTROL) != 0;

    // These shortcuts fire on PRESS only and are allowed even when ImGui has focus.
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_F && (super || ctrl)) { g_search_open = !g_search_open; return; }
        // Cmd+S: allowed mid text-box edit so you can save without closing the editor.
        if (key == GLFW_KEY_S && (super || ctrl)) { save_project_current(); return; }

        // Tool keys always work regardless of panel focus.
        bool cmd = super || ctrl;
        if (!cmd) {
            if (key == GLFW_KEY_P) {
                bool was_pen = (g_annot_tool == AnnotTool::Pen);
                g_annot_tool = was_pen ? AnnotTool::None : AnnotTool::Pen;
                g_ann_drawing = false;
                if (!was_pen && !g_input.panel_open()) {
                    const Document* sel = g_input.selected_doc();
                    if (sel) {
                        for (int i = 0; i < (int)g_documents.size(); ++i)
                            if (&g_documents[i] == sel) { g_input.open_panel(i, -1); break; }
                    }
                }
                return;
            }
            if (key == GLFW_KEY_H) {
                g_annot_tool = (g_annot_tool == AnnotTool::Highlight)
                               ? AnnotTool::None : AnnotTool::Highlight;
                g_ann_drawing = false;
                return;
            }
            if (key == GLFW_KEY_F && !super && !ctrl) {
                g_annot_tool = (g_annot_tool == AnnotTool::Note)
                               ? AnnotTool::None : AnnotTool::Note;
                g_ann_drawing = false;
                return;
            }
            // ESC: deactivate active tool even when panel has keyboard focus.
            if (key == GLFW_KEY_ESCAPE && !g_search_open) {
                if (g_editing_box >= 0) {
                    g_editing_box = -1;
                    return;
                }
                if (g_text_tool) { g_text_tool = false; return; }
                if (g_annot_tool != AnnotTool::None) {
                    g_annot_tool = AnnotTool::None;
                    g_ann_drawing = false;
                    return;
                }
            }
        }
    }

    // Space key must reach on_key even when the panel has keyboard focus so that
    // the press→release tap sequence that toggles the panel is always detected.
    // WantCaptureKeyboard is true whenever an ImGui window is active, which means
    // the space-up event would be swallowed and m_space_tap_pending never set,
    // making it impossible to close the panel with spacebar.
    if (ImGui::GetIO().WantCaptureKeyboard) {
        if (key == GLFW_KEY_SPACE)
            g_input.on_key(w, key, scancode, action, mods);
        return;
    }

    // InputHandler needs PRESS, REPEAT, and RELEASE so that m_space_held is cleared
    // on key-up. The early "action != GLFW_PRESS" return that used to sit above this
    // call was silently swallowing the space key-up, leaving m_space_held = true
    // forever and causing all subsequent left-drags to pan instead of move items.
    g_input.on_key(w, key, scancode, action, mods);

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

    if (key == GLFW_KEY_Z && (super || ctrl)) undo_last();
    if (key == GLFW_KEY_0 && (super || ctrl)) zoom_to_fit();

    // Arrow key navigation in panel or selection
    if (g_input.panel_open()) {
        int doc_idx = g_input.panel_doc_index();
        if (doc_idx >= 0 && doc_idx < (int)g_documents.size()) {
            int total = (int)g_documents[doc_idx].pages.size();
            if (key == GLFW_KEY_DOWN || key == GLFW_KEY_RIGHT) {
                s_panel_nav_page = std::min(s_panel_nav_page + 1, total - 1);
                g_input.open_panel(doc_idx, s_panel_nav_page);
            } else if (key == GLFW_KEY_UP || key == GLFW_KEY_LEFT) {
                s_panel_nav_page = std::max(s_panel_nav_page - 1, 0);
                g_input.open_panel(doc_idx, s_panel_nav_page);
            }
        }
    } else if (!g_input.selection().empty()) {
        // Arrow key selection navigation (when panel is closed)
        if (key == GLFW_KEY_DOWN || key == GLFW_KEY_RIGHT) {
            Page* next = next_page_in_order(g_nav_focus, +1);
            if (next) {
                g_input.clear_selection();
                const_cast<std::unordered_set<Page*>&>(g_input.selection()).insert(next);
                g_nav_focus = next;
            }
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_LEFT) {
            Page* next = next_page_in_order(g_nav_focus, -1);
            if (next) {
                g_input.clear_selection();
                const_cast<std::unordered_set<Page*>&>(g_input.selection()).insert(next);
                g_nav_focus = next;
            }
        }
    }

    if (action != GLFW_PRESS) return;

    // Tool shortcuts (no modifier)
    bool cmd = super || ctrl;
    if (!cmd) {
        if (key == GLFW_KEY_T) {
            g_text_tool = !g_text_tool;
            g_editing_box = -1;
        }
        if (key == GLFW_KEY_H) {
            g_annot_tool = (g_annot_tool == AnnotTool::Highlight)
                           ? AnnotTool::None : AnnotTool::Highlight;
            g_ann_drawing = false;
        }
        if (key == GLFW_KEY_P) {
            bool was_pen = (g_annot_tool == AnnotTool::Pen);
            g_annot_tool = was_pen ? AnnotTool::None : AnnotTool::Pen;
            g_ann_drawing = false;
            // Open panel if activating pen and panel not already open
            if (!was_pen && !g_input.panel_open()) {
                const Document* sel = g_input.selected_doc();
                if (sel) {
                    for (int i = 0; i < (int)g_documents.size(); ++i)
                        if (&g_documents[i] == sel) { g_input.open_panel(i, -1); break; }
                }
            }
        }
    }

    // Text-box entity copy/paste. Only reachable when no ImGui text field has
    // focus (guarded above), so copy/paste inside an open box goes to the text
    // field via ImGui instead. Here it duplicates the selected box.
    if (cmd && key == GLFW_KEY_C && g_selected_box >= 0 && g_editing_box < 0) {
        for (const auto& b : g_text_boxes)
            if (b.id == g_selected_box) { g_clip_box = b; g_clip_valid = true; break; }
    }
    if (cmd && key == GLFW_KEY_V && g_clip_valid) {
        CanvasTextBox nb = g_clip_box;
        nb.id        = g_next_box_id++;
        nb.world_pos = nb.world_pos + Vec2{16.0f, 16.0f};  // offset so the copy is visible
        g_text_boxes.push_back(nb);
        g_selected_box = nb.id;
        g_editing_box  = -1;
        UndoRecord r; r.type = UndoRecord::Type::TextBoxCreate; r.box_id = nb.id; push_undo(r);
    }
}

static void framebuffer_size_callback(GLFWwindow* w, int width, int height) {
    glViewport(0, 0, width, height);
    int win_w, win_h;
    glfwGetWindowSize(w, &win_w, &win_h);
    g_canvas.set_viewport_size(static_cast<float>(win_w), static_cast<float>(win_h));
}

static void focus_callback(GLFWwindow* /*w*/, int focused) {
    // When the window loses focus, key-up events for held keys are never delivered.
    // Clear panning state so space+drag doesn't stay active after a cmd-tab.
    if (!focused) g_input.clear_held_keys();
}

static void load_project_from_path(const std::string&);  // defined after project I/O helpers

static void drop_callback(GLFWwindow* /*w*/, int count, const char** paths) {
    namespace fs = std::filesystem;
    for (int i = 0; i < count; ++i) {
        fs::path p(paths[i]);
        if (fs::is_directory(p))
            load_pdfs_from_folder(paths[i]);
        else if (p.extension() == ".scholion")
            load_project_from_path(paths[i]);
        else
            load_pdf(paths[i]);
    }
}

// --- URL modal --------------------------------------------------------------

static void draw_url_modal() {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({480.0f, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);

    if (!ImGui::BeginPopupModal("Add from URL##modal", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    int state = g_dl_state.load(std::memory_order_acquire);

    if (state == 2) {
        // Download finished — load the file and close.
        if (g_dl_thread.joinable()) g_dl_thread.join();
        printf("Loading (from URL): %s\n", g_dl_path.c_str());
        load_pdf(g_dl_path);
        g_dl_state.store(0, std::memory_order_relaxed);
        g_url_buf[0] = '\0';
        ImGui::CloseCurrentPopup();

    } else if (state == 1) {
        // In-flight: spinner + cancel
        const char* frames[] = {"|", "/", "-", "\\"};
        int fi = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
        ImGui::Text("Downloading  %s", frames[fi]);
        ImGui::Spacing();
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                           {-1.0f, 0.0f}, "");
        ImGui::Spacing();
        if (ImGui::Button("Cancel")) {
            cancel_download();
            g_url_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

    } else {
        // Idle (0) or failed (3): show input field
        ImGui::Text("Paste a direct link to a PDF:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        // Auto-focus the text field the first time the modal opens
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##url_input", g_url_buf, sizeof(g_url_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);

        if (state == 3) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("Error: %s", g_dl_error.c_str());
            ImGui::PopStyleColor();
            // Reset to idle so error doesn't persist after a retry
            g_dl_state.store(0, std::memory_order_relaxed);
        }

        ImGui::Spacing();
        bool can_dl = g_url_buf[0] != '\0';
        if (!can_dl) ImGui::BeginDisabled();
        bool go = ImGui::Button("Download") || (enter && can_dl);
        if (!can_dl) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (g_dl_thread.joinable()) g_dl_thread.join();
            g_dl_state.store(0, std::memory_order_relaxed);
            g_url_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        if (go) {
            std::string url(g_url_buf);
            g_dl_path = download_dir() + "/" + filename_from_url(url);
            g_dl_error.clear();
            if (g_dl_thread.joinable()) g_dl_thread.join();
            g_dl_state.store(1, std::memory_order_release);
            g_dl_thread = std::thread(download_thread_fn, url, g_dl_path);
        }
    }

    ImGui::EndPopup();
}

// --- Cursor tool icon -------------------------------------------------------
// Draws a small pen or highlighter icon next to the cursor when a tool is active.

static void draw_cursor_tool_icon() {
    if (g_annot_tool == AnnotTool::None && !g_text_tool) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 m = ImGui::GetMousePos();

    if (g_annot_tool == AnnotTool::Pen) {
        ImU32 pc = IM_COL32((int)(g_pen_r*255), (int)(g_pen_g*255), (int)(g_pen_b*255), 240);
        ImVec2 a = {m.x + 14.0f, m.y + 4.0f};
        ImVec2 b = {m.x + 5.0f,  m.y + 13.0f};
        dl->AddLine(a, b, pc, 2.0f);
        dl->AddCircleFilled(b, 2.5f, pc);
    } else if (g_annot_tool == AnnotTool::Highlight) {
        ImVec2 tl = {m.x + 10.0f, m.y +  8.0f};
        ImVec2 br = {m.x + 24.0f, m.y + 16.0f};
        dl->AddRectFilled(tl, br, IM_COL32(255, 215, 0, 180));
        dl->AddRect(tl, br, IM_COL32(200, 165, 0, 230), 0.0f, 0, 1.0f);
    } else if (g_annot_tool == AnnotTool::Note) {
        float fx = m.x + 14.0f, fy = m.y + 3.0f;
        dl->AddLine({fx, fy}, {fx, fy + 13.0f}, IM_COL32(50, 110, 220, 240), 1.5f);
        dl->AddTriangleFilled({fx, fy}, {fx + 9.0f, fy + 4.0f}, {fx, fy + 8.0f},
                              IM_COL32(50, 110, 220, 210));
    } else if (g_annot_tool == AnnotTool::Eraser) {
        dl->AddCircle({m.x + 12.0f, m.y + 12.0f}, 10.0f, IM_COL32(220, 220, 220, 200), 16, 1.5f);
    } else if (g_text_tool) {
        float tx = m.x + 12.0f, ty = m.y + 2.0f;
        dl->AddRect({tx, ty}, {tx + 14.0f, ty + 16.0f}, IM_COL32(200, 200, 200, 200), 1.5f, 0, 1.2f);
        ImVec2 tsz = ImGui::CalcTextSize("A");
        dl->AddText({tx + (14.0f - tsz.x) * 0.5f, ty + (16.0f - tsz.y) * 0.5f},
                    IM_COL32(230, 230, 230, 220), "A");
    }
}

// --- Canvas text boxes -------------------------------------------------------

static constexpr float TBOX_W        = 280.0f;  // editing window width (fixed fallback)
static constexpr float TBOX_MIN_W    = 80.0f;
static constexpr float TBOX_MAX_W    = 420.0f;
static constexpr float TBOX_PAD      = 7.0f;
static constexpr float TBOX_MIN_H    = 28.0f;
static constexpr float TBOX_DEFAULT_W = 200.0f; // bare-click box width
static constexpr float TBOX_MIN_DRAG  = 8.0f;   // px; smaller drags count as a click

// Draw a dotted rectangle on an ImGui draw list (no native dashed support).
static void imgui_dashed_rect(ImDrawList* dl, ImVec2 tl, ImVec2 br, ImU32 col,
                              float dash = 6.0f, float gap = 4.0f, float thickness = 1.0f) {
    ImVec2 c[4] = { {tl.x, tl.y}, {br.x, tl.y}, {br.x, br.y}, {tl.x, br.y} };  // clockwise
    const float period = dash + gap;
    for (int e = 0; e < 4; ++e) {
        ImVec2 p0 = c[e], p1 = c[(e + 1) & 3];
        float ex = p1.x - p0.x, ey = p1.y - p0.y;
        float len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-3f) continue;
        float ux = ex / len, uy = ey / len;
        for (float s = 0.0f; s < len; s += period) {
            float d1 = std::min(s + dash, len);
            dl->AddLine({p0.x + ux * s, p0.y + uy * s},
                        {p0.x + ux * d1, p0.y + uy * d1}, col, thickness);
        }
    }
}

static void draw_canvas_text_boxes() {
    // ForegroundDrawList renders above all ImGui windows, so skip canvas text-box
    // drawing whenever a blocking overlay is up. The user can't interact with
    // boxes through the overlay anyway.
    if (g_settings_open) return;

    ImDrawList* dl    = ImGui::GetForegroundDrawList();
    ImFont*     font  = ImGui::GetFont();
    ImVec2      mouse = ImGui::GetMousePos();
    ImGuiIO&    io    = ImGui::GetIO();

    // Keep g_selected_box in sync with the unified selection. If the box is no longer in
    // m_selected_text_boxes (e.g. Escape cleared the selection) and isn't being edited,
    // drop the text-tool focus so the dotted border and style picker don't linger.
    if (g_selected_box >= 0 && g_editing_box < 0 &&
        !g_input.selected_text_boxes().count(g_selected_box))
        g_selected_box = -1;

    // Delete all selected items (pages + text boxes; Delete or Backspace; not while editing)
    if ((g_editing_box < 0 && !io.WantCaptureKeyboard)
        && (ImGui::IsKeyPressed(ImGuiKey_Delete, false)
            || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        const auto& sel_pages = g_input.selection();
        const auto& sel_boxes = g_input.selected_text_boxes();

        bool has_items = !sel_pages.empty() || !sel_boxes.empty() || g_selected_box >= 0;
        if (has_items) {
            // Delete selected text boxes (including g_selected_box if not in unified selection)
            for (auto& b : g_text_boxes) {
                if (sel_boxes.count(b.id) > 0 || b.id == g_selected_box) {
                    UndoRecord r;
                    r.type        = UndoRecord::Type::TextBoxDelete;
                    r.box_id      = b.id;
                    r.deleted_box = b;
                    push_undo(r);
                }
            }
            g_text_boxes.erase(std::remove_if(g_text_boxes.begin(), g_text_boxes.end(),
                [&sel_boxes](const CanvasTextBox& b){ return sel_boxes.count(b.id) > 0; }),
                g_text_boxes.end());

            // Delete selected pages (remove entire documents that have selected pages)
            if (!sel_pages.empty()) {
                std::set<int> docs_to_remove;
                for (int d = 0; d < (int)g_documents.size(); ++d) {
                    for (const auto& p : g_documents[d].pages) {
                        if (sel_pages.count(const_cast<Page*>(&p))) {
                            docs_to_remove.insert(d);
                            break;
                        }
                    }
                }
                // Snapshot undo records for each document before removing (in reverse order to preserve indices)
                std::vector<UndoRecord> undo_records;
                for (auto it = docs_to_remove.rbegin(); it != docs_to_remove.rend(); ++it) {
                    int doc_idx = *it;
                    UndoRecord r;
                    r.type = UndoRecord::Type::DocumentRemove;
                    r.removed_doc_idx = doc_idx;
                    r.removed_doc = g_documents[doc_idx];
                    r.removed_loader = g_loaders[doc_idx];
                    undo_records.push_back(r);
                }
                // Remove documents
                for (auto it = docs_to_remove.rbegin(); it != docs_to_remove.rend(); ++it)
                    remove_document(*it);
                // Push undo records after removal
                for (auto& r : undo_records)
                    push_undo(r);
            }

            g_selected_box = -1;
            g_input.clear_selection();
        }
    }

    // Continue drag if LMB still held
    if (g_box_dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Vec2 w = g_canvas.screen_to_world({mouse.x, mouse.y});
            Vec2 delta = w - g_box_drag_start_world;
            for (auto& box : g_text_boxes) {
                for (const auto& state : g_box_drag_states) {
                    if (box.id == state.id) { box.world_pos = state.initial_pos + delta; break; }
                }
            }
            // Move any selected pages that were recorded when the drag started
            for (const auto& ps : g_page_drag_states)
                ps.page->world_pos = ps.initial_pos + delta;
        } else {
            // Drag ended — push undo for moved text boxes
            for (const auto& state : g_box_drag_states) {
                for (const auto& box : g_text_boxes) {
                    if (box.id == state.id) {
                        if (box.world_pos.x != state.initial_pos.x ||
                            box.world_pos.y != state.initial_pos.y) {
                            UndoRecord r;
                            r.type        = UndoRecord::Type::TextBoxMove;
                            r.box_id      = state.id;
                            r.old_box_pos = state.initial_pos;
                            push_undo(r);
                        }
                        break;
                    }
                }
            }
            // Push undo for moved pages (text-box-initiated drag)
            if (!g_page_drag_states.empty()) {
                UndoRecord r;
                r.type = UndoRecord::Type::PageMove;
                for (const auto& ps : g_page_drag_states)
                    if (ps.page->world_pos.x != ps.initial_pos.x ||
                        ps.page->world_pos.y != ps.initial_pos.y)
                        r.page_moves.push_back({ps.page, ps.initial_pos});
                if (!r.page_moves.empty()) push_undo(r);
                g_page_drag_states.clear();
            }
            g_box_drag_states.clear();
            g_box_dragging = false;
        }
    }

    // Per-box rendering and hit-testing
    int new_hovered = -1;
    ImVec2 vp = ImGui::GetMainViewport()->Size;

    // Clip all ForegroundDrawList drawing to the canvas area so text boxes don't
    // render on top of the panel when it is open. The panel occupies the rightmost
    // s_panel_w pixels; boxes that overlap it are clipped at the panel's left edge.
    float canvas_right = g_input.panel_open() ? (vp.x - s_panel_w) : vp.x;
    dl->PushClipRect({0.0f, 0.0f}, {canvas_right, vp.y}, true);

    for (auto& box : g_text_boxes) {
        if (g_editing_box == box.id) continue;  // editing box shown as ImGui window below

        Vec2 sp = g_canvas.world_to_screen(box.world_pos);

        // Rough off-screen cull
        if (sp.x + TBOX_MAX_W < 0 || sp.x > vp.x || sp.y + 300 < 0 || sp.y > vp.y) continue;

        const char* content = box.text[0] ? box.text : " ";
        float box_w, wrap, box_h;
        if (box.w > 0.0f) {
            // Drag-sized (or default-width) box: width is fixed, text wraps to it,
            // and the dragged height is a floor that grows downward to fit content.
            box_w = box.w;
            wrap  = box_w - 2.0f * TBOX_PAD;
            ImVec2 text_sz = font->CalcTextSizeA(box.font_size, FLT_MAX, wrap, content);
            box_h = std::max(box.h, text_sz.y + 2.0f * TBOX_PAD);
        } else {
            // Legacy boxes (saved before w/h existed): auto-size width to text.
            float natural_w = font->CalcTextSizeA(box.font_size, FLT_MAX, 0.0f, content).x + 2.0f * TBOX_PAD;
            box_w = std::clamp(natural_w, TBOX_MIN_W, TBOX_MAX_W);
            wrap  = box_w - 2.0f * TBOX_PAD;
            ImVec2 text_sz = font->CalcTextSizeA(box.font_size, FLT_MAX, wrap, content);
            box_h = std::max(TBOX_MIN_H, text_sz.y + 2.0f * TBOX_PAD);
        }
        ImVec2 tl = {sp.x, sp.y};
        ImVec2 br = {sp.x + box_w, sp.y + box_h};

        bool hit = !io.WantCaptureMouse
                && mouse.x >= tl.x && mouse.x <= br.x
                && mouse.y >= tl.y && mouse.y <= br.y
                && mouse.x < canvas_right;  // exclude clicks inside the panel

        if (hit) new_hovered = box.id;

        // Draw text (or placeholder)
        if (box.text[0]) {
            dl->AddText(font, box.font_size, {tl.x + TBOX_PAD, tl.y + TBOX_PAD},
                        IM_COL32((int)(box.r*255), (int)(box.g*255), (int)(box.b*255), 220),
                        box.text, nullptr, wrap);
        } else {
            dl->AddText(font, box.font_size, {tl.x + TBOX_PAD, tl.y + TBOX_PAD},
                        IM_COL32(160, 160, 160, 130), "Double-click to edit...");
        }

        // Selection border — thin dotted grey on any selected text box (unified with
        // page selection). The single `g_selected_box` is still used by the text tool
        // for editing/styling, but this shows the dotted border on all selected boxes.
        bool is_selected = g_input.selected_text_boxes().count(box.id) > 0;
        if (is_selected || g_selected_box == box.id) {
            const float m = 6.0f;
            imgui_dashed_rect(dl, {tl.x - m, tl.y - m}, {br.x + m, br.y + m},
                              IM_COL32(215, 215, 222, 235));
        }

        // Click / drag / double-click
        if (hit) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g_selected_box   = box.id;
                g_editing_box    = box.id;
                g_tbox_r         = box.r; g_tbox_g = box.g; g_tbox_b = box.b;
                g_tbox_font_size = box.font_size;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                bool cmd      = (io.KeyCtrl || io.KeySuper);
                bool in_sel   = g_input.selected_text_boxes().count(box.id) > 0;
                auto& sel_ref = const_cast<std::unordered_set<int>&>(g_input.selected_text_boxes());

                // Add/toggle text box in the unified selection
                if (cmd) {
                    // Cmd+click: toggle in/out without clearing other items
                    if (in_sel) sel_ref.erase(box.id);
                    else        sel_ref.insert(box.id);
                } else if (in_sel) {
                    // Plain click on already-selected box: drag the whole group, keep selection
                } else {
                    // Plain click on unselected box: replace entire selection
                    g_input.clear_selection();
                    sel_ref.insert(box.id);
                }

                g_selected_box     = box.id;
                // Selecting a box shows its style in the picker (and becomes the
                // template for the next new box once this one is deselected).
                g_tbox_r = box.r; g_tbox_g = box.g; g_tbox_b = box.b;
                g_tbox_font_size = box.font_size;

                // Start multi-box drag: record initial positions of all selected text boxes
                Vec2 w = g_canvas.screen_to_world({mouse.x, mouse.y});
                g_box_drag_start_world = w;
                g_box_drag_states.clear();
                for (int sel_id : g_input.selected_text_boxes()) {
                    for (const auto& b : g_text_boxes) {
                        if (b.id == sel_id) {
                            g_box_drag_states.push_back({sel_id, b.world_pos});
                            break;
                        }
                    }
                }
                // Record initial positions of selected pages so they move with the boxes
                g_page_drag_states.clear();
                for (Page* p : g_input.selection())
                    g_page_drag_states.push_back({p, p->world_pos});
                g_box_dragging = true;
            }
        }
    }
    g_hovered_box = new_hovered;

    // Deselect text box when clicking on empty canvas
    if (!io.WantCaptureMouse && g_hovered_box < 0 && g_editing_box < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_selected_box >= 0)
        g_selected_box = -1;

    // Create a new box via press-drag-release (text tool active, empty canvas).
    // The drag rectangle sets the box size; a bare click makes a default box.
    if (g_text_tool && !io.WantCaptureMouse && g_editing_box < 0
        && !g_tbox_creating && g_hovered_box < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_tbox_creating     = true;
        g_tbox_create_start = {mouse.x, mouse.y};
    }
    if (g_tbox_creating) {
        ImVec2 a = {std::min(g_tbox_create_start.x, mouse.x), std::min(g_tbox_create_start.y, mouse.y)};
        ImVec2 b = {std::max(g_tbox_create_start.x, mouse.x), std::max(g_tbox_create_start.y, mouse.y)};
        dl->AddRect(a, b, IM_COL32(175, 195, 235, 200), 3.0f, 0, 1.5f);  // live preview

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dw = b.x - a.x, dh = b.y - a.y;
            CanvasTextBox nb;
            nb.id        = g_next_box_id++;
            nb.text[0]   = '\0';
            nb.r = g_tbox_r; nb.g = g_tbox_g; nb.b = g_tbox_b;
            nb.font_size = g_tbox_font_size;
            if (dw >= TBOX_MIN_DRAG && dh >= TBOX_MIN_DRAG) {
                nb.world_pos = g_canvas.screen_to_world({a.x, a.y});
                nb.w = dw; nb.h = dh;
            } else {
                // Bare click → default-width, auto-height box anchored at the click.
                nb.world_pos = g_canvas.screen_to_world({g_tbox_create_start.x, g_tbox_create_start.y});
                nb.w = TBOX_DEFAULT_W; nb.h = 0.0f;
            }
            g_text_boxes.push_back(nb);
            g_selected_box = nb.id;
            g_editing_box  = nb.id;
            g_just_created = true;
            { UndoRecord r; r.type = UndoRecord::Type::TextBoxCreate; r.box_id = nb.id; push_undo(r); }
            g_tbox_creating = false;
        }
    }

    dl->PopClipRect();

    // Editing ImGui window
    if (g_editing_box >= 0) {
        CanvasTextBox* eb = nullptr;
        for (auto& box : g_text_boxes)
            if (box.id == g_editing_box) { eb = &box; break; }

        if (!eb) {
            g_editing_box = -1;
        } else {
            Vec2 sp = g_canvas.world_to_screen(eb->world_pos);
            float ew = (eb->w > 0.0f) ? eb->w : TBOX_W;
            float eh = std::max((eb->h > 0.0f) ? eb->h : 130.0f, 60.0f);
            ImGui::SetNextWindowPos({sp.x, sp.y}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({ew, eh}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {TBOX_PAD, TBOX_PAD});
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.14f, 0.92f));

            char wid[32]; snprintf(wid, sizeof(wid), "##tbedit%d", g_editing_box);
            ImGui::Begin(wid, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // First frame of this editing session — g_prev_editing_box is updated
            // at the end of this function, so here it still holds last frame's value.
            bool first_edit_frame = (g_editing_box != g_prev_editing_box);
            if (first_edit_frame)
                ImGui::SetKeyboardFocusHere();
            // Save text before InputTextMultiline: ImGui reverts the buffer on ESC,
            // so we need our own copy to restore ("confirm") rather than discard.
            char saved_text[2048];
            strncpy(saved_text, eb->text, sizeof(saved_text));
            ImGui::InputTextMultiline("##tbtxt", eb->text, sizeof(eb->text), {-1.0f, -1.0f});
            bool esc_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

            // Border around editing window
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRect(
                wp, {wp.x + ws.x, wp.y + ws.y}, IM_COL32(100, 140, 230, 220), 3.0f, 0, 2.0f);

            // Close editing only when the user left-clicks on the canvas (outside all
            // ImGui windows). Clicking a toolbar widget must NOT close editing, because
            // the toolbar color/size controls need g_editing_box to still be set when
            // they fire in the same frame.
            // Skip on the first frame: the window was just created, so
            // WantCaptureMouse is still stale (false) and the double-click that
            // opened editing would otherwise close it immediately.
            bool lost_focus = !first_edit_frame
                              && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                              && !ImGui::GetIO().WantCaptureMouse;
            ImGui::End();

            if (esc_pressed) {
                strncpy(eb->text, saved_text, sizeof(eb->text));  // restore what ImGui reverted
                g_tbox_r = eb->r; g_tbox_g = eb->g; g_tbox_b = eb->b;
                g_tbox_font_size = eb->font_size;
                g_editing_box = -1;   // confirm + close; box stays selected, tool stays active.
                                      // A second ESC (handled globally) exits the tool.
            } else if (lost_focus) {
                g_tbox_r = eb->r; g_tbox_g = eb->g; g_tbox_b = eb->b;
                g_tbox_font_size = eb->font_size;
                g_editing_box = -1;
            }
        }
    }

    // ---- Undo session tracking -------------------------------------------------
    // Text edits: snapshot text when an edit session begins, push one TextBoxEdit
    // record when it ends (if the text changed and the box wasn't freshly created).
    if (g_editing_box != g_prev_editing_box) {
        if (g_prev_editing_box >= 0 && !g_edit_was_new) {
            for (auto& b : g_text_boxes)
                if (b.id == g_prev_editing_box) {
                    if (strcmp(b.text, g_edit_text0) != 0) {
                        UndoRecord r; r.type = UndoRecord::Type::TextBoxEdit;
                        r.box_id = b.id; r.prev_text = g_edit_text0;
                        push_undo(r);
                    }
                    break;
                }
        }
        if (g_editing_box >= 0) {
            for (auto& b : g_text_boxes)
                if (b.id == g_editing_box) {
                    strncpy(g_edit_text0, b.text, sizeof(g_edit_text0) - 1);
                    g_edit_text0[sizeof(g_edit_text0) - 1] = '\0';
                    break;
                }
            g_edit_was_new = g_just_created;
        }
        g_just_created     = false;
        g_prev_editing_box = g_editing_box;
    }

    // Style changes: snapshot color/size when a box is selected, push one
    // TextBoxStyle record when the selection ends (if the style changed).
    if (g_selected_box != g_prev_selected_box) {
        if (g_prev_selected_box >= 0) {
            for (auto& b : g_text_boxes)
                if (b.id == g_prev_selected_box) {
                    if (b.r != g_style_r0 || b.g != g_style_g0 ||
                        b.b != g_style_b0 || b.font_size != g_style_fs0) {
                        UndoRecord r; r.type = UndoRecord::Type::TextBoxStyle;
                        r.box_id = b.id;
                        r.old_r = g_style_r0; r.old_g = g_style_g0;
                        r.old_b = g_style_b0; r.old_fs = g_style_fs0;
                        push_undo(r);
                    }
                    break;
                }
        }
        if (g_selected_box >= 0) {
            for (auto& b : g_text_boxes)
                if (b.id == g_selected_box) {
                    g_style_r0 = b.r; g_style_g0 = b.g; g_style_b0 = b.b; g_style_fs0 = b.font_size;
                    break;
                }
        }
        g_prev_selected_box = g_selected_box;
    }
}

// --- Annotation finalization ------------------------------------------------

static void finalize_annotation() {
    if (g_ann_doc_idx < 0 || g_ann_doc_idx >= (int)g_documents.size()) {
        g_ann_drawing = false; return;
    }
    Document& fdoc = g_documents[g_ann_doc_idx];
    if (g_ann_page_idx < 0 || g_ann_page_idx >= (int)fdoc.pages.size()) {
        g_ann_drawing = false; return;
    }
    Page& fpage = fdoc.pages[g_ann_page_idx];
    if (g_annot_tool == AnnotTool::Pen) {
        if (g_ann_cur_stroke.pts.size() >= 2) {
            fpage.annots.strokes.push_back(std::move(g_ann_cur_stroke));
            UndoRecord r;
            r.type     = UndoRecord::Type::PenStroke;
            r.doc_idx  = g_ann_doc_idx;
            r.page_idx = g_ann_page_idx;
            push_undo(r);
        }
        g_ann_cur_stroke = {};
    } else if (g_annot_tool == AnnotTool::Highlight) {
        AnnotHighlight hl;
        hl.x0 = std::min(g_ann_hl_start.x, g_ann_cur_norm.x);
        hl.y0 = std::min(g_ann_hl_start.y, g_ann_cur_norm.y);
        hl.x1 = std::max(g_ann_hl_start.x, g_ann_cur_norm.x);
        hl.y1 = std::max(g_ann_hl_start.y, g_ann_cur_norm.y);
        if (hl.x1 > hl.x0 && hl.y1 > hl.y0) {
            // Text-snap: collect characters whose centre falls inside the
            // selection rect, snap the highlight bbox to them, and capture
            // their text. Falls back to a plain rect when the page has no
            // selectable text (scanned images, figures, etc.).
            auto* loader = (g_ann_doc_idx >= 0 && g_ann_doc_idx < (int)g_loaders.size())
                           ? g_loaders[g_ann_doc_idx].get() : nullptr;
            if (loader && loader->valid()) {
                const auto& quads = loader->get_char_quads(g_ann_page_idx);
                std::vector<const CharQuad*> sel;
                sel.reserve(quads.size());
                for (const auto& q : quads) {
                    float cx = (q.x0 + q.x1) * 0.5f;
                    float cy = (q.y0 + q.y1) * 0.5f;
                    if (cx >= hl.x0 && cx <= hl.x1 && cy >= hl.y0 && cy <= hl.y1)
                        sel.push_back(&q);
                }
                if (!sel.empty()) {
                    std::sort(sel.begin(), sel.end(),
                              [](const CharQuad* a, const CharQuad* b){
                                  return a->order < b->order; });
                    // Snap bbox to the tight union of all selected char rects
                    float sx0 = sel[0]->x0, sy0 = sel[0]->y0;
                    float sx1 = sel[0]->x1, sy1 = sel[0]->y1;
                    std::string text;
                    for (const auto* q : sel) {
                        sx0 = std::min(sx0, q->x0); sy0 = std::min(sy0, q->y0);
                        sx1 = std::max(sx1, q->x1); sy1 = std::max(sy1, q->y1);
                        text += q->utf8;
                        if (q->line_end) text += ' ';
                    }
                    // Trim trailing space added by line_end logic
                    while (!text.empty() && text.back() == ' ') text.pop_back();
                    hl.x0 = sx0; hl.y0 = sy0; hl.x1 = sx1; hl.y1 = sy1;
                    hl.text = std::move(text);
                }
            }
            fpage.annots.highlights.push_back(hl);
            UndoRecord r;
            r.type     = UndoRecord::Type::Highlight;
            r.doc_idx  = g_ann_doc_idx;
            r.page_idx = g_ann_page_idx;
            push_undo(r);
        }
    }
    g_ann_drawing = false;
}

// Open the system file manager and highlight the given path.
// macOS: fork/execl with /usr/bin/open -R — no shell, immune to special chars.
// Windows: ShellExecuteW with explorer /select — equivalent native approach.
static void reveal_in_file_manager(const std::string& path) {
#ifdef __APPLE__
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/open", "open", "-R", path.c_str(), (char*)nullptr);
        _exit(127);
    }
#elif defined(_WIN32)
    // Convert UTF-8 path to UTF-16 for Windows wide-string APIs.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    std::wstring args = L"/select,\"" + wpath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOW);
#else
    (void)path;  // Linux: xdg-open stub — implement when porting to Linux
#endif
}

// --- Context menu -----------------------------------------------------------

static void draw_context_menu() {
    static ImVec2 s_popup_pos;

    if (g_input.ctx_pending()) {
        s_popup_pos = {g_input.ctx_pos().x, g_input.ctx_pos().y};
        ImGui::OpenPopup("##page_ctx");
        g_input.consume_ctx();
    }

    ImGui::SetNextWindowPos(s_popup_pos, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##page_ctx")) {
        Document* doc  = g_input.ctx_doc();
        Page*     page = g_input.ctx_page();

        if (doc && page) {
            if (doc->missing) {
                ImGui::TextDisabled("PDF not found:");
                ImGui::TextDisabled("%s", doc->path.c_str());
                if (ImGui::MenuItem("Relink PDF...")) {
#ifdef __APPLE__
                    scholion_activate_app();
#endif
                    const char* pats[] = {"*.pdf", "*.PDF"};
                    const char* picked = tinyfd_openFileDialog(
                        "Locate the missing PDF", doc->path.c_str(), 2, pats, "PDF Documents", 0);
                    if (picked) {
                        for (int i = 0; i < (int)g_documents.size(); ++i)
                            if (&g_documents[i] == doc) { relink_document(i, picked); break; }
                    }
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return;   // doc pointer is now invalid after relink
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Return to stack")) {
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                r.page_moves.push_back({page, page->world_pos});
                push_undo(r);
                page->world_pos = {
                    doc->stack_origin.x + page->page_index * DocumentStack::FAN_OFFSET,
                    doc->stack_origin.y + page->page_index * DocumentStack::FAN_OFFSET
                };
            }
            if (ImGui::MenuItem("View in panel")) {
                for (int i = 0; i < static_cast<int>(g_documents.size()); ++i) {
                    if (&g_documents[i] == doc) {
                        g_input.open_panel(i, page->page_index);
                        break;
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fan pages vertically")) {
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                for (auto& p : doc->pages) r.page_moves.push_back({&p, p.world_pos});
                push_undo(r);
                float y = doc->stack_origin.y;
                for (auto& p : doc->pages) { p.world_pos = {doc->stack_origin.x, y}; y += p.world_h + 20.0f; }
            }
            if (ImGui::MenuItem("Fan pages horizontally")) {
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                for (auto& p : doc->pages) r.page_moves.push_back({&p, p.world_pos});
                push_undo(r);
                float x = doc->stack_origin.x;
                for (auto& p : doc->pages) { p.world_pos = {x, doc->stack_origin.y}; x += p.world_w + 20.0f; }
            }
            if (ImGui::MenuItem("Stack pages")) {
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                for (auto& p : doc->pages) r.page_moves.push_back({&p, p.world_pos});
                push_undo(r);
                for (auto& p : doc->pages) {
                    p.world_pos = {
                        doc->stack_origin.x + p.page_index * DocumentStack::FAN_OFFSET,
                        doc->stack_origin.y + p.page_index * DocumentStack::FAN_OFFSET
                    };
                }
            }
            // Align + Normalize — only shown when ≥2 pages are selected and
            // the right-clicked page is part of that selection.
            const auto& sel = g_input.selection();
            if (sel.size() >= 2 && sel.count(page)) {
                ImGui::Separator();

                // Helper: snapshot selection for undo, then call op()
                auto with_move_undo = [&](auto op) {
                    UndoRecord r; r.type = UndoRecord::Type::PageMove;
                    for (Page* p : sel) r.page_moves.push_back({p, p->world_pos});
                    push_undo(r);
                    op();
                };

                if (ImGui::BeginMenu("Align Selection")) {
                    if (ImGui::MenuItem("Left Edges")) with_move_undo([&]{
                        for (Page* p : sel) p->world_pos.x = page->world_pos.x;
                    });
                    if (ImGui::MenuItem("Right Edges")) with_move_undo([&]{
                        float ref = page->world_pos.x + page->world_w;
                        for (Page* p : sel) p->world_pos.x = ref - p->world_w;
                    });
                    if (ImGui::MenuItem("Top Edges")) with_move_undo([&]{
                        for (Page* p : sel) p->world_pos.y = page->world_pos.y;
                    });
                    if (ImGui::MenuItem("Bottom Edges")) with_move_undo([&]{
                        float ref = page->world_pos.y + page->world_h;
                        for (Page* p : sel) p->world_pos.y = ref - p->world_h;
                    });
                    if (ImGui::MenuItem("Centers Horizontal")) with_move_undo([&]{
                        float ref = page->world_pos.x + page->world_w * 0.5f;
                        for (Page* p : sel) p->world_pos.x = ref - p->world_w * 0.5f;
                    });
                    if (ImGui::MenuItem("Centers Vertical")) with_move_undo([&]{
                        float ref = page->world_pos.y + page->world_h * 0.5f;
                        for (Page* p : sel) p->world_pos.y = ref - p->world_h * 0.5f;
                    });
                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem("Normalize Width to This")) {
                    if (page->world_w > 0.0f) {
                        UndoRecord r; r.type = UndoRecord::Type::PageResize;
                        for (Page* p : sel)
                            r.page_moves.push_back({p, p->world_pos, p->world_w, p->world_h});
                        push_undo(r);
                        for (Page* p : sel) {
                            if (p == page || p->world_w <= 0.0f) continue;
                            float scale   = page->world_w / p->world_w;
                            p->world_w    = page->world_w;
                            p->world_h   *= scale;
                        }
                    }
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Zoom to Fit Document")) {
                float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
                for (const auto& p : doc->pages) {
                    bx0 = std::min(bx0, p.world_pos.x);
                    by0 = std::min(by0, p.world_pos.y);
                    bx1 = std::max(bx1, p.world_pos.x + p.world_w);
                    by1 = std::max(by1, p.world_pos.y + p.world_h);
                }
                zoom_to_rect(bx0, by0, bx1, by1);
            }
            if (ImGui::MenuItem("Copy Page Text")) {
                std::string text = extract_page_text(doc->path, page->page_index);
                if (!text.empty()) glfwSetClipboardString(g_window, text.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reveal in Finder")) {
                reveal_in_file_manager(doc->path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Document")) {
                for (int i = 0; i < (int)g_documents.size(); ++i) {
                    if (&g_documents[i] == doc) {
                        // Snapshot for undo before removing
                        UndoRecord r;
                        r.type = UndoRecord::Type::DocumentRemove;
                        r.removed_doc_idx = i;
                        r.removed_doc = g_documents[i];  // full copy
                        r.removed_loader = g_loaders[i];
                        remove_document(i);   // purges dangling page* records
                        push_undo(r);         // push AFTER removal so no dangling pointers
                        break;
                    }
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// --- JSON string helpers ----------------------------------------------------

static std::string json_escape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        if      (*s == '\\') out += "\\\\";
        else if (*s == '"')  out += "\\\"";
        else if (*s == '\n') out += "\\n";
        else if (*s == '\r') out += "\\r";
        else                 out += *s;
    }
    return out;
}

// Reads the value of the first "text": "..." field on a line, handling escapes.
static bool extract_json_text(const char* line, char* out, int out_sz) {
    const char* p = strstr(line, "\"text\": \"");
    if (!p) return false;
    p += 9;
    int j = 0;
    while (*p && j < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) {
            ++p;
            switch (*p) {
                case 'n':  out[j++] = '\n'; break;
                case 'r':  out[j++] = '\r'; break;
                case '"':  out[j++] = '"';  break;
                case '\\': out[j++] = '\\'; break;
                default:   out[j++] = *p;   break;
            }
        } else if (*p == '"') {
            break;
        } else {
            out[j++] = *p;
        }
        ++p;
    }
    out[j] = '\0';
    return true;
}

// --- Save / load project ----------------------------------------------------

static std::string g_project_path;
static std::chrono::steady_clock::time_point g_last_save_time;

static std::vector<std::string> g_recents;
static constexpr int RECENTS_MAX = 10;

static std::string recents_file_path() {
#ifdef _WIN32
    // Store in %APPDATA%\Scholion\recents (roaming so it follows the user profile).
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wpath))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
        std::string appdata(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &appdata[0], len, nullptr, nullptr);
        CoTaskMemFree(wpath);
        std::string dir = appdata + "\\Scholion";
        // Use error_code overload — prevents std::filesystem exceptions if the
        // directory already exists (known MinGW / GCC behaviour on second run).
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir + "\\recents";
    }
    return "";
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.scholion_recents" : "";
#endif
}

static void save_recents() {
    std::string p = recents_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    for (const auto& r : g_recents) fprintf(f, "%s\n", r.c_str());
    fclose(f);
}

static void load_recents() {
    std::string p = recents_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) g_recents.push_back(s);
    }
    fclose(f);
}

static void add_to_recents(const std::string& path) {
    g_recents.erase(std::remove(g_recents.begin(), g_recents.end(), path), g_recents.end());
    g_recents.insert(g_recents.begin(), path);
    if ((int)g_recents.size() > RECENTS_MAX) g_recents.resize(RECENTS_MAX);
    save_recents();
}

// --- Application preferences persistence ---

static std::string prefs_file_path() {
#ifdef _WIN32
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wpath))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
        std::string appdata(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &appdata[0], len, nullptr, nullptr);
        CoTaskMemFree(wpath);
        std::string dir = appdata + "\\Scholion";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir + "\\prefs";
    }
    return "";
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.scholion_prefs" : "";
#endif
}

static void save_prefs() {
    std::string p = prefs_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "dark_mode=%d\n",   g_settings.dark_mode   ? 1 : 0);
    fprintf(f, "grid_mode=%d\n",   (int)g_settings.grid_mode);
    fprintf(f, "compat_mode=%d\n", g_settings.compat_mode ? 1 : 0);
    fprintf(f, "vignette_on=%d\n", g_settings.vignette_on ? 1 : 0);
    fprintf(f, "panel_w=%.1f\n",   g_settings.panel_w);
    fclose(f);
}

static void load_prefs() {
    std::string p = prefs_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char key[64];
    float fval;
    while (fscanf(f, " %63[^=]=%f", key, &fval) == 2) {
        int ival = (int)fval;
        if      (!strcmp(key, "dark_mode"))   g_settings.dark_mode   = ival;
        else if (!strcmp(key, "grid_mode"))   g_settings.grid_mode   = (GridMode)ival;
        else if (!strcmp(key, "compat_mode")) g_settings.compat_mode = ival;
        else if (!strcmp(key, "vignette_on")) g_settings.vignette_on = ival;
        else if (!strcmp(key, "panel_w"))     g_settings.panel_w     = fval;
    }
    fclose(f);
}

// Theme application with custom rounding
static void apply_theme(bool dark) {
    if (dark) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
        ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.93f, 0.91f, 0.96f);
    }
    ImGui::GetStyle().WindowRounding   = 6.0f;
    ImGui::GetStyle().PopupRounding    = 5.0f;
    ImGui::GetStyle().FrameRounding    = 4.0f;
    ImGui::GetStyle().WindowBorderSize = 0.0f;
}

static void update_window_title() {
    if (!g_window) return;
    if (g_project_path.empty()) {
        glfwSetWindowTitle(g_window, "Scholion");
    } else {
        std::string name = std::filesystem::path(g_project_path).stem().string();
        glfwSetWindowTitle(g_window, ("Scholion \xe2\x80\x94 " + name).c_str());
    }
}

static void new_project() {
    clear_documents();
    g_text_boxes.clear();
    g_selected_box = g_editing_box = -1;
    g_prev_selected_box = g_prev_editing_box = -1;
    g_just_created = g_edit_was_new = false;
    g_undo_stack.clear();
    g_next_note_idx = 0;
    g_next_box_id   = 0;
    g_clip_valid    = false;   // don't carry a copied box across projects
    g_project_path.clear();
    update_window_title();
}

static bool save_to_path(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "save_project: cannot open %s\n", path.c_str()); return false; }

    Vec2 offset = g_canvas.get_offset();
    fprintf(f, "{\n");
    fprintf(f, "  \"viewport\": { \"x\": %.4f, \"y\": %.4f, \"zoom\": %.6f },\n",
            offset.x, offset.y, g_canvas.get_zoom());
    fprintf(f, "  \"note_idx\": %d,\n", g_next_note_idx);
    fprintf(f, "  \"documents\": [\n");
    for (int di = 0; di < (int)g_documents.size(); ++di) {
        const Document& doc = g_documents[di];
        fprintf(f, "    {\n");
        fprintf(f, "      \"path\": \"%s\",\n", doc.path.c_str());
        fprintf(f, "      \"stack_origin\": [%.4f, %.4f],\n",
                doc.stack_origin.x, doc.stack_origin.y);
        fprintf(f, "      \"pages\": [\n");
        for (int pi = 0; pi < (int)doc.pages.size(); ++pi) {
            const Page& page = doc.pages[pi];
            fprintf(f, "        { \"index\": %d, \"x\": %.4f, \"y\": %.4f, \"w\": %.2f, \"h\": %.2f }%s\n",
                    page.page_index, page.world_pos.x, page.world_pos.y,
                    page.world_w, page.world_h,
                    pi + 1 < (int)doc.pages.size() ? "," : "");
        }
        fprintf(f, "      ]\n");
        fprintf(f, "    }%s\n", di + 1 < (int)g_documents.size() ? "," : "");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"text_boxes\": [\n");
    for (int i = 0; i < (int)g_text_boxes.size(); ++i) {
        const auto& box = g_text_boxes[i];
        std::string esc = json_escape(box.text);
        fprintf(f, "    { \"id\": %d, \"x\": %.4f, \"y\": %.4f, \"r\": %.3f, \"g\": %.3f, \"b\": %.3f, \"fs\": %.1f, \"w\": %.2f, \"h\": %.2f, \"text\": \"%s\" }%s\n",
                box.id, box.world_pos.x, box.world_pos.y, box.r, box.g, box.b, box.font_size,
                box.w, box.h, esc.c_str(),
                i + 1 < (int)g_text_boxes.size() ? "," : "");
    }
    fprintf(f, "  ],\n");

    // Annotations — flat list, one record per line.
    // Stroke points follow their stroke header as "{ \"p\": [x, y] }" lines.
    fprintf(f, "  \"annots\": [\n");
    bool first_annot = true;
    auto sep = [&]{ fprintf(f, first_annot ? "" : ",\n"); first_annot = false; };

    for (int di = 0; di < (int)g_documents.size(); ++di) {
        for (int pi = 0; pi < (int)g_documents[di].pages.size(); ++pi) {
            const PageAnnotations& an = g_documents[di].pages[pi].annots;
            for (const auto& hl : an.highlights) {
                sep();
                if (hl.text.empty()) {
                    fprintf(f, "    { \"doc\": %d, \"page\": %d, \"hl\": [%.6f, %.6f, %.6f, %.6f] }",
                            di, pi, hl.x0, hl.y0, hl.x1, hl.y1);
                } else {
                    std::string esc = json_escape(hl.text.c_str());
                    fprintf(f, "    { \"doc\": %d, \"page\": %d, \"hl\": [%.6f, %.6f, %.6f, %.6f], \"ht\": \"%s\" }",
                            di, pi, hl.x0, hl.y0, hl.x1, hl.y1, esc.c_str());
                }
            }
            for (const auto& note : an.notes) {
                sep();
                fprintf(f, "    { \"doc\": %d, \"page\": %d, \"note\": \"%s\" }",
                        di, pi, note.label.c_str());
            }
            for (const auto& stroke : an.strokes) {
                sep();
                fprintf(f, "    { \"doc\": %d, \"page\": %d, \"sr\": %.5f, \"sg\": %.5f, \"sb\": %.5f }",
                        di, pi, stroke.r, stroke.g, stroke.b);
                for (const auto& pt : stroke.pts) {
                    fprintf(f, ",\n    { \"p\": [%.6f, %.6f] }", pt.x, pt.y);
                }
            }
        }
    }
    if (!first_annot) fprintf(f, "\n");
    fprintf(f, "  ]\n}\n");
    fclose(f);

    if (g_debug) {
        int s_hl = 0, s_note = 0, s_stroke = 0;
        for (const auto& doc : g_documents)
            for (const auto& pg : doc.pages) {
                s_hl += (int)pg.annots.highlights.size();
                s_note += (int)pg.annots.notes.size();
                s_stroke += (int)pg.annots.strokes.size();
            }
        printf("Project saved: %s — %d text boxes, %d highlights, %d notes, %d strokes\n",
               path.c_str(), (int)g_text_boxes.size(), s_hl, s_note, s_stroke);
    }
    // Trigger save feedback (manual save by default; auto-save will override)
    if (g_save_feedback_type == SaveFeedbackType::None)
        g_save_feedback_type = SaveFeedbackType::Manual;
    g_save_feedback_time = std::chrono::steady_clock::now();
    return true;
}

static void save_project() {
#ifdef __APPLE__
    scholion_activate_app();
#endif
    const char* filter_patterns[] = {"*.scholion"};
    const char* picked = tinyfd_saveFileDialog(
        "Save Project", "project.scholion", 1, filter_patterns, "Scholion Project");
    if (!picked) return;

    if (save_to_path(picked)) {
        g_project_path   = picked;
        g_last_save_time = std::chrono::steady_clock::now();
        add_to_recents(picked);
        update_window_title();
        printf("Project saved: %s\n", picked);
    }
}

// Cmd+S: silently overwrite the known project file; if none is set yet, fall
// back to the Save-As dialog (which sets g_project_path on success).
static void save_project_current() {
    if (g_project_path.empty()) {
        save_project();
        return;
    }
    if (save_to_path(g_project_path)) {
        g_last_save_time = std::chrono::steady_clock::now();
        printf("Project saved: %s\n", g_project_path.c_str());
    } else {
        fprintf(stderr, "save_project: failed to write %s\n", g_project_path.c_str());
    }
}

// --- Load project -----------------------------------------------------------
// load_project_from_path reads the entire file into memory and locates tokens
// by position rather than by line, making it tolerant of compact/pretty-printed
// JSON, CRLF line endings, and reordered top-level keys.
//
// doc_map (saved index → actual g_documents index) handles the case where one
// or more PDFs are missing at load time: a missing PDF becomes a placeholder
// Document (doc.missing = true, loader = nullptr) that keeps its pages and
// annotations alive. Subsequent saved docs shift their actual index relative to
// their saved index. Annotations are re-keyed through doc_map after all docs
// load so they attach to the right document even when indices shifted.
static void load_project_from_path(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { fprintf(stderr, "load_project: cannot open %s\n", path.c_str()); return; }

    struct SavedPage { int idx; float x, y; float w = 0.0f, h = 0.0f; };
    struct SavedDoc  { std::string path; float sox, soy; std::vector<SavedPage> pages; };

    // Saved annotation records collected during parse, applied after docs load.
    struct SavedHL     { int doc, page; AnnotHighlight hl; };
    struct SavedNote   { int doc, page; std::string label; };
    struct SavedStroke { int doc, page; AnnotStroke stroke; };

    std::vector<SavedDoc>      saved;
    std::vector<SavedHL>       saved_hls;
    std::vector<SavedNote>     saved_notes;
    std::vector<SavedStroke>   saved_strokes;
    std::vector<CanvasTextBox> saved_boxes;   // applied after the reset, like annotations

    float vx = 0.0f, vy = 0.0f, vz = 0.6f;
    int   cur = -1;

    enum class Section { Docs, TextBoxes, Annots, Other };

    // State for multi-line stroke parsing
    int   stroke_doc = -1, stroke_page = -1;
    AnnotStroke building_stroke;
    bool  building = false;

    auto flush_stroke = [&]() {
        if (building && !building_stroke.pts.empty())
            saved_strokes.push_back({stroke_doc, stroke_page, std::move(building_stroke)});
        building_stroke = {};
        building = false;
    };

    // Read the whole file (CR-stripped for CRLF tolerance). We then locate key
    // tokens by position rather than by line, so a record may span lines or share
    // a line and field order/whitespace doesn't matter — sscanf treats any run of
    // whitespace (incl. newlines) the same, so the same field patterns apply.
    std::string content;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0) {
            if ((size_t)sz > 20 * 1024 * 1024) {
                fclose(f);
                fprintf(stderr, "load: project file exceeds 20 MB limit (%ld bytes)\n", sz);
                return;
            }
            content.resize((size_t)sz);
            fseek(f, 0, SEEK_SET);
            size_t rd = fread(&content[0], 1, (size_t)sz, f);
            content.resize(rd);
        }
    }
    fclose(f);
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());

    // Section is determined by which named-array key most recently precedes a
    // token's position (the file writes viewport, documents, text_boxes, annots
    // in that order). Colon-anchored so a path value can't be mistaken for one.
    const size_t npos = std::string::npos;
    size_t doc_key = content.find("\"documents\":");
    size_t tb_key  = content.find("\"text_boxes\":");
    size_t an_key  = content.find("\"annots\":");
    auto section_at = [&](size_t at) -> Section {
        Section sec = Section::Other;
        if (doc_key != npos && at > doc_key) sec = Section::Docs;
        if (tb_key  != npos && at > tb_key)  sec = Section::TextBoxes;
        if (an_key  != npos && at > an_key)  sec = Section::Annots;
        return sec;
    };

    enum Tok { T_VIEWPORT, T_NOTE_IDX, T_PATH, T_STACK, T_INDEX, T_ID, T_DOC, T_POINT };
    struct TokDef { const char* s; size_t len; Tok t; };
    static const TokDef toks[] = {
        {"\"viewport\":",     11, T_VIEWPORT},
        {"\"note_idx\":",     11, T_NOTE_IDX},
        {"\"path\":",          7, T_PATH},
        {"\"stack_origin\":", 15, T_STACK},
        {"\"index\":",         8, T_INDEX},
        {"\"id\":",            5, T_ID},
        {"\"doc\":",           6, T_DOC},
        {"\"p\":",             4, T_POINT},
    };
    bool note_idx_loaded = false;

    size_t scan = 0;
    while (scan < content.size()) {
        size_t best = npos; const TokDef* bt = nullptr;
        for (const auto& td : toks) {
            size_t fnd = content.find(td.s, scan);
            if (fnd < best) { best = fnd; bt = &td; }
        }
        if (!bt) break;
        const char* at  = content.c_str() + best;
        Section     sec = section_at(best);
        float a, b, c, d; int n; char s[4096];

        switch (bt->t) {
            case T_VIEWPORT:
                if (sscanf(at, "\"viewport\": { \"x\": %f, \"y\": %f, \"zoom\": %f }", &a, &b, &c) == 3) {
                    vx = a; vy = b; vz = c;
                }
                break;
            case T_NOTE_IDX:
                if (sscanf(at, "\"note_idx\": %d", &n) == 1) {
                    g_next_note_idx = n;
                    note_idx_loaded = true;
                }
                break;
            case T_PATH:
                if (sec == Section::Docs && sscanf(at, "\"path\": \"%[^\"]\"", s) == 1) {
                    saved.push_back({s, 0.0f, 0.0f, {}});
                    cur = (int)saved.size() - 1;
                }
                break;
            case T_STACK:
                if (sec == Section::Docs && cur >= 0 &&
                    sscanf(at, "\"stack_origin\": [%f, %f]", &a, &b) == 2) {
                    saved[cur].sox = a; saved[cur].soy = b;
                }
                break;
            case T_INDEX:
                if (sec == Section::Docs && cur >= 0) {
                    float pw = 0.0f, ph = 0.0f;
                    int np = sscanf(at, "\"index\": %d, \"x\": %f, \"y\": %f, \"w\": %f, \"h\": %f",
                                    &n, &a, &b, &pw, &ph);
                    if (np >= 3) saved[cur].pages.push_back({n, a, b, pw, ph});
                }
                break;
            case T_ID:
                if (sec == Section::TextBoxes) {
                    int id; float x, y, tr = 0.82f, tg = 0.06f, tb2 = 0.06f, tfs = 16.0f, tw = 0.0f, th = 0.0f;
                    int np = sscanf(at,
                        "\"id\": %d, \"x\": %f, \"y\": %f, \"r\": %f, \"g\": %f, \"b\": %f, \"fs\": %f, \"w\": %f, \"h\": %f",
                        &id, &x, &y, &tr, &tg, &tb2, &tfs, &tw, &th);
                    if (np >= 3) {
                        CanvasTextBox tb; tb.id = id; tb.world_pos = {x, y}; tb.text[0] = '\0';
                        if (np >= 7) { tb.r = tr; tb.g = tg; tb.b = tb2; tb.font_size = tfs; }
                        if (np >= 9) { tb.w = tw; tb.h = th; }
                        extract_json_text(at, tb.text, sizeof(tb.text));  // finds this record's "text"
                        saved_boxes.push_back(tb);
                    }
                }
                break;
            case T_DOC:
                if (sec == Section::Annots) {
                    int di, pi;
                    if (sscanf(at, "\"doc\": %d, \"page\": %d, \"hl\": [%f, %f, %f, %f]",
                               &di, &pi, &a, &b, &c, &d) == 6) {
                        flush_stroke();
                        AnnotHighlight parsed_hl{a, b, c, d, {}};
                        // Restore captured text if present (optional field added in M29)
                        const char* ht = strstr(at, "\"ht\": \"");
                        if (ht) {
                            ht += 7;
                            while (*ht && *ht != '"') {
                                if (*ht == '\\' && *(ht + 1)) {
                                    ++ht;
                                    switch (*ht) {
                                        case 'n': parsed_hl.text += '\n'; break;
                                        case '"': parsed_hl.text += '"';  break;
                                        case '\\': parsed_hl.text += '\\'; break;
                                        default: parsed_hl.text += *ht; break;
                                    }
                                } else {
                                    parsed_hl.text += *ht;
                                }
                                ++ht;
                            }
                        }
                        saved_hls.push_back({di, pi, std::move(parsed_hl)});
                    } else if (sscanf(at, "\"doc\": %d, \"page\": %d, \"note\": \"%[^\"]\"",
                                      &di, &pi, s) == 3) {
                        flush_stroke(); saved_notes.push_back({di, pi, s});
                    } else if (sscanf(at, "\"doc\": %d, \"page\": %d, \"sr\": %f, \"sg\": %f, \"sb\": %f",
                                      &di, &pi, &a, &b, &c) == 5) {
                        flush_stroke();
                        stroke_doc = di; stroke_page = pi;
                        building_stroke = {}; building_stroke.r = a; building_stroke.g = b; building_stroke.b = c;
                        building = true;
                    }
                }
                break;
            case T_POINT:
                if (sec == Section::Annots && building &&
                    sscanf(at, "\"p\": [%f, %f]", &a, &b) == 2) {
                    building_stroke.pts.push_back({a, b});
                }
                break;
        }
        scan = best + bt->len;
    }
    flush_stroke();

    clear_documents();
    g_text_boxes.clear();
    g_selected_box = g_editing_box = -1;
    g_prev_selected_box = g_prev_editing_box = -1;
    g_just_created = g_edit_was_new = false;
    namespace fs = std::filesystem;

    // Restore the parsed text boxes now that the reset is done.
    g_text_boxes = std::move(saved_boxes);
    for (const auto& tb : g_text_boxes)
        g_next_box_id = std::max(g_next_box_id, tb.id + 1);

    // Map each saved document index → its actual index in g_documents. A skipped
    // (missing) PDF shifts later indices, so annotations must be re-keyed through
    // this map or they'd attach to the wrong document (or be dropped).
    // Default placeholder page size (US Letter, in PDF points = world units).
    // Page dimensions aren't stored in the project file, so a missing PDF's
    // pages fall back to this until the real file is available again.
    constexpr float PLACEHOLDER_PAGE_W = 612.0f, PLACEHOLDER_PAGE_H = 792.0f;

    std::vector<int> doc_map(saved.size(), -1);
    for (size_t si = 0; si < saved.size(); ++si) {
        const auto& sd = saved[si];
        if (!fs::exists(sd.path)) {
            fprintf(stderr, "load_project: PDF not found, keeping placeholder: %s\n", sd.path.c_str());
            // Keep a placeholder document so its reference + annotations survive a
            // re-save and document indices stay aligned with the saved file.
            Document d;
            d.path         = sd.path;
            d.missing      = true;
            d.stack_origin = {sd.sox, sd.soy};
            auto& c = DOC_PALETTE[g_documents.size() % PALETTE_SIZE];
            d.hue_r = c[0]; d.hue_g = c[1]; d.hue_b = c[2];
            for (const auto& sp : sd.pages) {
                Page p;
                p.page_index = sp.idx;
                p.world_pos  = {sp.x, sp.y};
                p.world_w    = sp.w > 0.0f ? sp.w : PLACEHOLDER_PAGE_W;  // saved size if present
                p.world_h    = sp.h > 0.0f ? sp.h : PLACEHOLDER_PAGE_H;
                d.pages.push_back(p);
            }
            doc_map[si] = (int)g_documents.size();
            g_documents.push_back(std::move(d));
            g_loaders.push_back(nullptr);   // keep parallel arrays aligned
            continue;
        }
        printf("Loading: %s\n", sd.path.c_str());
        size_t before = g_documents.size();
        load_pdf(sd.path);
        if (g_documents.size() == before) continue;  // load failed; leave map at -1

        doc_map[si] = (int)g_documents.size() - 1;
        Document& doc = g_documents.back();
        doc.stack_origin = {sd.sox, sd.soy};
        for (const auto& sp : sd.pages) {
            for (auto& pg : doc.pages) {
                if (pg.page_index == sp.idx) { pg.world_pos = {sp.x, sp.y}; break; }
            }
        }
    }
    g_input.set_documents(&g_documents);  // ensure input has docs even if all are placeholders
    auto map_doc = [&](int sd) -> int {
        return (sd >= 0 && sd < (int)doc_map.size()) ? doc_map[sd] : -1;
    };

    // Apply saved annotations now that documents and pages are in place.
    auto safe_page = [&](int di, int pi) -> Page* {
        if (di < 0 || di >= (int)g_documents.size()) return nullptr;
        auto& pages = g_documents[di].pages;
        auto it = std::find_if(pages.begin(), pages.end(),
                               [pi](const Page& p){ return p.page_index == pi; });
        return it != pages.end() ? &*it : nullptr;
    };

    for (const auto& sh : saved_hls)    if (auto* p = safe_page(map_doc(sh.doc), sh.page)) p->annots.highlights.push_back(sh.hl);
    for (const auto& sn : saved_notes)  if (auto* p = safe_page(map_doc(sn.doc), sn.page)) p->annots.notes.push_back({sn.label});
    for (auto& ss : saved_strokes)      if (auto* p = safe_page(map_doc(ss.doc), ss.page)) p->annots.strokes.push_back(std::move(ss.stroke));

    // Restore note counter. Newer files save it directly; older files fall back
    // to counting notes (correct as long as no notes were deleted before saving).
    if (!note_idx_loaded) {
        g_next_note_idx = 0;
        for (const auto& doc : g_documents)
            for (const auto& pg : doc.pages)
                g_next_note_idx += (int)pg.annots.notes.size();
    }

    g_canvas.set_offset({vx, vy});
    g_canvas.set_zoom(vz);

    g_project_path   = path;
    g_last_save_time = std::chrono::steady_clock::now();
    add_to_recents(path);
    update_window_title();

    if (g_debug) {
        int n_hl = 0, n_note = 0, n_stroke = 0;
        for (const auto& doc : g_documents)
            for (const auto& pg : doc.pages) {
                n_hl += (int)pg.annots.highlights.size();
                n_note += (int)pg.annots.notes.size();
                n_stroke += (int)pg.annots.strokes.size();
            }
        printf("Project loaded: %s — %d docs, %d text boxes, %d highlights, %d notes, %d strokes\n",
               path.c_str(), (int)g_documents.size(), (int)g_text_boxes.size(), n_hl, n_note, n_stroke);
    }
}

static void load_project() {
#ifdef __APPLE__
    scholion_activate_app();
#endif
    // No type filter: tinyfiledialogs passes the extension to AppleScript's
    // "choose file of type", which on macOS 12+ treats it as a UTI lookup.
    // Since "scholion" isn't a registered UTI, all .scholion files get greyed
    // out. Showing all files is more reliable; the dialog title guides the user.
    const char* p = tinyfd_openFileDialog("Open Project", "", 0, nullptr, nullptr, 0);
    if (p) load_project_from_path(p);
}

// --- Startup chooser (bare/native launch only) ------------------------------
// Shown when the app is opened with no document. Lets the user open a saved
// project, load PDF file(s), or load a whole folder of PDFs — or start blank.
static bool g_startup_chooser = false;

static void draw_startup_chooser() {
    if (!g_startup_chooser) return;
    if (!ImGui::IsPopupOpen("##startup")) ImGui::OpenPopup("##startup");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("##startup", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted("Open a project or add PDFs to begin");
        ImGui::Spacing();
        const ImVec2 bsz = {260.0f, 0.0f};

        if (ImGui::Button("Open Project File", bsz)) {
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
#ifdef __APPLE__
            scholion_activate_app();
#endif
            load_project();
        }
        if (ImGui::Button("Add PDF File(s)", bsz)) {
#ifdef __APPLE__
            scholion_activate_app();
#endif
            const char* pats[] = {"*.pdf", "*.PDF"};
            const char* r = tinyfd_openFileDialog("Select PDF files", nullptr, 2, pats, "PDF Documents", 1);
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
            if (r) load_pdfs_from_selection(r);
        }
        if (ImGui::Button("Add Folder of PDFs", bsz)) {
#ifdef __APPLE__
            scholion_activate_app();
#endif
            const char* d = tinyfd_selectFolderDialog("Select PDF folder", nullptr);
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
            if (d) load_pdfs_from_folder(d);
        }
        ImGui::Separator();
        if (ImGui::Button("Start with blank canvas", bsz)) {
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// --- Settings popup --------------------------------------------------------

static void draw_settings_popup() {
    if (!g_settings_open) return;

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos({vp.x * 0.5f, vp.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({360.0f, 100.0f}, {520.0f, vp.y * 0.9f});

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("Settings", &g_settings_open, flags)) {
        ImGui::End();
        return;
    }

    // Dismiss on ESC or ENTER
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        g_settings_open = false;
        ImGui::End();
        return;
    }

    // --- Appearance ---
    ImGui::SeparatorText("Appearance");
    bool was_dark = g_settings.dark_mode;
    ImGui::RadioButton("Dark",  (int*)&g_settings.dark_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Light", (int*)&g_settings.dark_mode, 0);
    if (g_settings.dark_mode != was_dark) {
        apply_theme(g_settings.dark_mode);
        save_prefs();
    }
    bool prev_vignette = g_settings.vignette_on;
    ImGui::Checkbox("Canvas vignette", &g_settings.vignette_on);
    if (g_settings.vignette_on != prev_vignette)
        save_prefs();

    // --- Canvas Grid ---
    ImGui::SeparatorText("Canvas Grid");
    GridMode prev_grid = g_settings.grid_mode;
    ImGui::RadioButton("Off",        (int*)&g_settings.grid_mode, (int)GridMode::Off);
    ImGui::RadioButton("Line grid",  (int*)&g_settings.grid_mode, (int)GridMode::Lines);
    ImGui::RadioButton("Dot matrix", (int*)&g_settings.grid_mode, (int)GridMode::Dots);
    if (g_settings.grid_mode != prev_grid) {
        save_prefs();
    }

    // --- Compatibility Mode ---
    ImGui::SeparatorText("Performance");
    bool prev_compat = g_settings.compat_mode;
    ImGui::Checkbox("Compatibility mode", &g_settings.compat_mode);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 340.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Caps PDF rendering at Low quality when zoomed out, "
                       "freezes re-rendering at extreme zoom levels, and limits "
                       "the frame rate to 30 fps. Saves globally across all projects.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (g_settings.compat_mode != prev_compat) {
        save_prefs();
    }

    // --- Keyboard Shortcuts ---
    ImGui::SeparatorText("Keyboard Shortcuts");
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_BordersInnerV
                                     | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Key",    ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();

        auto row = [](const char* action, const char* key) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", key);
        };

        row("Pan",                         "Middle-drag / Space+drag");
        row("Zoom",                        "Scroll wheel");
        row("Zoom to Fit",                 "Cmd+0 / Middle double-click");
        row("Toggle status overlay",       "F3");
        row("Full-text search",            "Cmd+F");
        row("Select page",                 "Click");
        row("Open panel",                  "Double-click");
        row("Toggle panel",                "Space (tap)");
        row("Move page",                   "Drag");
        row("Toggle whole document select", "Shift+click");
        row("Toggle item in selection",    "Cmd+click");
        row("Select all",                  "Cmd+A");
        row("Rubber-band select",          "Drag empty canvas");
        row("Clear selection / close panel","Escape");
        row("Text tool",                   "T");
        row("Pen tool (+ open panel)",     "P");
        row("Highlight tool",              "H");
        row("Create text box",             "Drag (T active)");
        row("Edit text box",               "Double-click box");
        row("Duplicate text box",          "Cmd+C, Cmd+V");
        row("Delete text box",             "Delete / Backspace");
        row("Undo",                        "Cmd+Z");
        row("Save",                        "Cmd+S");
        ImGui::EndTable();
    }

    // Footer with attribution
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted("Scholion is a canvas-style PDF review utility designed and built by @ARMMRDN (2026)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 340.0f);
    ImGui::TextWrapped("\"a scholion is an explanatory comment typically written in the margin of a manuscript "
                       "by its ancient authors or students, as a guide\"");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::End();
}

// --- Quit confirmation dialog -----------------------------------------------

static void draw_quit_dialog() {
    if (g_quit_requested && g_quit_state == QuitState::None) {
        g_quit_state = QuitState::Waiting;
        ImGui::OpenPopup("Quit Scholion?");
    }

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos({vp.x * 0.5f, vp.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});

    if (ImGui::BeginPopupModal("Quit Scholion?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextWrapped("Do you want to save your project before quitting?");
        ImGui::Spacing();

        // Button layout
        float button_width = 100.0f;
        float spacing = 10.0f;
        float total_width = (button_width * 3) + (spacing * 2);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() / 2 - total_width / 2);

        if (ImGui::Button("Save", {button_width, 0})) {
            save_project_current();
            g_quit_state = QuitState::Confirmed;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Save the project and quit");

        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Discard", {button_width, 0})) {
            g_quit_state = QuitState::Confirmed;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Quit without saving");

        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Cancel", {button_width, 0})) {
            g_quit_requested = false;
            g_quit_state = QuitState::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Go back to the app");

        ImGui::EndPopup();
    }
}

// --- Canvas context menu (right-click on empty canvas) ---------------------

static void draw_canvas_context_menu() {
    static ImVec2 s_popup_pos;

    if (g_input.canvas_ctx_pending()) {
        s_popup_pos = {g_input.canvas_ctx_pos().x, g_input.canvas_ctx_pos().y};
        ImGui::OpenPopup("##canvas_ctx");
        g_input.consume_canvas_ctx();
    }

    ImGui::SetNextWindowPos(s_popup_pos, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##canvas_ctx")) {
        // Most useful operations first
        if (ImGui::MenuItem("Search...", "Cmd+F"))
            g_search_open = true;
        if (ImGui::MenuItem("Zoom to Fit", "Cmd+0"))
            zoom_to_fit();
        ImGui::Separator();

        // Project operations
        if (ImGui::MenuItem("New Project"))
            new_project();
        if (ImGui::MenuItem("Open project..."))
            load_project();
        if (!g_recents.empty()) {
            if (ImGui::BeginMenu("Open Recent")) {
                for (const auto& rp : g_recents) {
                    std::string name = std::filesystem::path(rp).filename().string();
                    if (ImGui::MenuItem(name.c_str()))
                        load_project_from_path(rp);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent"))
                    { g_recents.clear(); save_recents(); }
                ImGui::EndMenu();
            }
        }
        if (ImGui::MenuItem("Save Project", "Cmd+S"))
            save_project_current();   // update existing file, or Save-As if none set
        ImGui::Separator();

        // Document operations
        if (ImGui::MenuItem("Add PDF...")) {
#ifdef __APPLE__
            scholion_activate_app();
#endif
            const char* patterns[] = {"*.pdf", "*.PDF"};
            const char* r = tinyfd_openFileDialog("Select PDF files", nullptr,
                                                  2, patterns, "PDF Documents", 1);
            if (r) load_pdfs_from_selection(r);
        }
        if (ImGui::MenuItem("Add folder...")) {
#ifdef __APPLE__
            scholion_activate_app();
#endif
            const char* r = tinyfd_selectFolderDialog("Select PDF folder", nullptr);
            if (r) load_pdfs_from_folder(r);
        }
        ImGui::Separator();

        // App preferences
        if (ImGui::MenuItem("Settings"))
            g_settings_open = true;
        ImGui::Separator();

        // Exit
        if (ImGui::MenuItem("Quit", "Cmd+Q")) {
            // TODO: Show save confirmation dialog
            // For now, just set a flag to trigger the quit dialog
            g_quit_requested = true;
        }
        ImGui::EndPopup();
    }
}

// --- References tab (inside panel) ------------------------------------------

static void draw_references_tab() {
    namespace fs = std::filesystem;

    // Right-aligned export button
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - 95.0f);
    if (ImGui::SmallButton("Export .md")) {
#ifdef __APPLE__
        scholion_activate_app();
#endif
        const char* filters[] = {"*.md"};
        const char* out_path = tinyfd_saveFileDialog(
            "Export References", "references.md", 1, filters, "Markdown");
        if (out_path) {
            FILE* f = fopen(out_path, "w");
            if (f) {
                fprintf(f, "# Scholion References\n\n");
                for (int di = 0; di < (int)g_documents.size(); ++di) {
                    const Document& doc = g_documents[di];
                    bool has_refs = false;
                    for (const auto& pg : doc.pages)
                        for (const auto& hl : pg.annots.highlights)
                            if (!hl.text.empty()) { has_refs = true; break; }
                    if (!has_refs) continue;
                    std::string fname = fs::path(doc.path).filename().string();
                    fprintf(f, "## %s\n\n", fname.c_str());
                    for (const auto& pg : doc.pages)
                        for (const auto& hl : pg.annots.highlights)
                            if (!hl.text.empty())
                                fprintf(f, "- **p.%d** — %s\n",
                                        pg.page_index + 1, hl.text.c_str());
                    fprintf(f, "\n");
                }
                fclose(f);
            }
        }
    }
    ImGui::SetItemTooltip("Export all text highlights as a Markdown file");
    ImGui::Separator();

    bool any = false;
    for (int di = 0; di < (int)g_documents.size(); ++di) {
        const Document& doc = g_documents[di];
        // Count text-bearing highlights for this document
        int ref_count = 0;
        for (const auto& pg : doc.pages)
            for (const auto& hl : pg.annots.highlights)
                if (!hl.text.empty()) ++ref_count;
        if (ref_count == 0) continue;
        any = true;

        // Document header in its hue color
        std::string fname = fs::path(doc.path).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, {doc.hue_r, doc.hue_g, doc.hue_b, 1.0f});
        ImGui::TextUnformatted(fname.c_str());
        ImGui::PopStyleColor();

        for (int pi = 0; pi < (int)doc.pages.size(); ++pi) {
            const Page& pg = doc.pages[pi];
            for (int hi = 0; hi < (int)pg.annots.highlights.size(); ++hi) {
                const AnnotHighlight& hl = pg.annots.highlights[hi];
                if (hl.text.empty()) continue;

                // Truncate display text; full text appears on hover
                std::string display = hl.text;
                bool truncated = display.size() > 90;
                if (truncated) { display.resize(87); display += "..."; }

                // Combined selectable row "p.N  text..."
                char row[640];
                snprintf(row, sizeof(row), "p.%d  %s##ref%d_%d_%d",
                         pg.page_index + 1, display.c_str(), di, pi, hi);

                if (ImGui::Selectable(row, false, ImGuiSelectableFlags_None, {0.0f, 0.0f})) {
                    zoom_to_rect(pg.world_pos.x, pg.world_pos.y,
                                 pg.world_pos.x + pg.world_w,
                                 pg.world_pos.y + pg.world_h);
                    g_input.open_panel(di, pg.page_index);
                }
                if (truncated)
                    ImGui::SetItemTooltip("%s", hl.text.c_str());
            }
        }
        ImGui::Spacing();
    }

    if (!any) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, {0.5f, 0.5f, 0.5f, 1.0f});
        ImGui::TextWrapped(
            "No text highlights yet.\n\n"
            "Open a document, select the Highlight tool, then drag across text on the page "
            "— it will appear here with the filename and page number.");
        ImGui::PopStyleColor();
    }
}

// --- Panel viewer -----------------------------------------------------------

static void draw_panel_ui() {
    if (!g_input.panel_open()) return;

    int doc_idx = g_input.panel_doc_index();
    if (doc_idx < 0 || doc_idx >= static_cast<int>(g_documents.size())) {
        g_input.close_panel();
        return;
    }

    Document& doc = g_documents[doc_idx];
    ImVec2 vp = ImGui::GetMainViewport()->Size;

    ImGui::SetNextWindowPos({vp.x - s_panel_w, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({s_panel_w, vp.y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar           |
        ImGuiWindowFlags_NoResize             |
        ImGuiWindowFlags_NoMove               |
        ImGuiWindowFlags_NoCollapse           |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f, 10.0f});
    ImGui::Begin("##panel", nullptr, kFlags);
    ImGui::PopStyleVar();

    // Subtle visual indicator on the left edge showing the resize handle
    {
        ImVec2 wp = ImGui::GetWindowPos();
        float  wh = ImGui::GetWindowHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(
            {wp.x, wp.y}, {wp.x + 3.0f, wp.y + wh},
            IM_COL32(90, 90, 100, 140));
    }

    // Track the "active" page for nav and copy (updated on scroll-to events)
    int scroll_to_peek = g_input.panel_scroll_page();  // read before clear
    if (ImGui::IsWindowAppearing()) s_panel_nav_page = std::max(0, scroll_to_peek);
    else if (scroll_to_peek >= 0)  s_panel_nav_page = scroll_to_peek;

    // Header: colored filename + close button
    namespace fs = std::filesystem;
    std::string fname = fs::path(doc.path).filename().string();
    ImGui::PushStyleColor(ImGuiCol_Text, {doc.hue_r, doc.hue_g, doc.hue_b, 1.0f});
    ImGui::TextUnformatted(fname.c_str());
    ImGui::PopStyleColor();

    if (ImGui::SmallButton("Close##panel")) {
        g_annot_tool  = AnnotTool::None;
        g_ann_drawing = false;
        g_input.close_panel();
        ImGui::End();
        return;
    }
    ImGui::SetItemTooltip("Close this document panel");
    ImGui::SameLine();

    // Annotation tool buttons — capture active state before button call to keep Push/Pop balanced
    {
        bool pen_was    = (g_annot_tool == AnnotTool::Pen);
        bool hl_was     = (g_annot_tool == AnnotTool::Highlight);
        bool note_was   = (g_annot_tool == AnnotTool::Note);
        bool eraser_was = (g_annot_tool == AnnotTool::Eraser);

        if (pen_was)    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.08f, 0.08f, 1.0f));
        if (ImGui::SmallButton("Pen"))       { g_annot_tool = pen_was    ? AnnotTool::None : AnnotTool::Pen;       g_ann_drawing = false; }
        if (pen_was)    ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Freehand pen - draw on the page (Escape to cancel)");
        if (g_annot_tool == AnnotTool::Pen) {
            ImGui::SameLine();
            float pcol[3] = {g_pen_r, g_pen_g, g_pen_b};
            if (ImGui::ColorEdit3("##pencolor", pcol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                { g_pen_r = pcol[0]; g_pen_g = pcol[1]; g_pen_b = pcol[2]; }
            ImGui::SetItemTooltip("Pen color");
        }
        ImGui::SameLine();

        if (hl_was)     ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.55f, 0.0f,  1.0f));
        if (ImGui::SmallButton("Highlight")) { g_annot_tool = hl_was     ? AnnotTool::None : AnnotTool::Highlight; g_ann_drawing = false; }
        if (hl_was)     ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Highlight tool - drag to draw a yellow rectangle (Escape to cancel)");
        ImGui::SameLine();

        if (note_was)   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.40f, 0.70f, 1.0f));
        if (ImGui::SmallButton("Flag"))      { g_annot_tool = note_was   ? AnnotTool::None : AnnotTool::Note;      g_ann_drawing = false; }
        if (note_was)   ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Flag - mark a page for quick reference (A, B, C...); visible even when zoomed out");
        ImGui::SameLine();

        if (eraser_was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        if (ImGui::SmallButton("Erase"))     { g_annot_tool = eraser_was ? AnnotTool::None : AnnotTool::Eraser;    g_ann_drawing = false; }
        if (eraser_was) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Eraser - drag over strokes or highlights to remove them (Escape to cancel)");

        ImGui::SameLine();
        ImGui::Spacing(); ImGui::SameLine();

        // Page navigation  < p.N/Total >
        int total = (int)doc.pages.size();
        if (ImGui::SmallButton("<") && s_panel_nav_page > 0) {
            s_panel_nav_page--;
            g_input.open_panel(doc_idx, s_panel_nav_page);
        }
        ImGui::SetItemTooltip("Previous page");
        ImGui::SameLine();
        ImGui::Text("p.%d/%d", s_panel_nav_page + 1, total);
        ImGui::SameLine();
        if (ImGui::SmallButton(">") && s_panel_nav_page < total - 1) {
            s_panel_nav_page++;
            g_input.open_panel(doc_idx, s_panel_nav_page);
        }
        ImGui::SetItemTooltip("Next page");
    }

    // Tab bar: "Document" shows the PDF viewer; "References" lists all text highlights.
    ImGui::BeginTabBar("##panel_tabs");

    if (ImGui::BeginTabItem("Document")) {
    // Child window fills the remaining panel height and is the only scrollable region.
    // The toolbar above stays pinned regardless of scroll position.
    ImGui::BeginChild("##panel_scroll", {0.0f, 0.0f}, false, ImGuiWindowFlags_None);
    ImGui::Spacing();

    float avail_w  = ImGui::GetContentRegionAvail().x;
    int   scroll_to = g_input.panel_scroll_page();

    for (int pi = 0; pi < (int)doc.pages.size(); ++pi) {
        Page& page = doc.pages[pi];

        // Panel always targets High tier (300 DPI), independent of canvas zoom.
        // Enqueue on demand; tex_for_lod falls back to Low/Thumb while High loads.
        if (page.needs_lod(LodTier::High) && g_loaders[doc_idx])
            enqueue_rast(doc.path, g_loaders[doc_idx], page.page_index, LodTier::High);
        uint32_t tex = page.tex_for_lod(LodTier::High);

        float img_w = avail_w;
        float img_h = (page.world_w > 0.0f)
            ? page.world_h * (avail_w / page.world_w)
            : avail_w * 1.41f;

        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Check if this page is the highlighted search result
        bool is_highlighted = false;
        if (g_highlighted_search_result >= 0 &&
            g_highlighted_search_result < (int)g_search_results.size()) {
            const auto& hit = g_search_results[g_highlighted_search_result];
            is_highlighted = (hit.doc_idx == doc_idx && hit.page_idx == pi);
        }

        // Render image or placeholder
        if (tex) {
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                         {img_w, img_h});
        } else {
            dl->AddRectFilled(img_pos, {img_pos.x + img_w, img_pos.y + img_h},
                              IM_COL32(55, 55, 58, 255));
            ImGui::Dummy({img_w, img_h});
        }

        // Draw blue highlight border if this is the highlighted search result
        if (is_highlighted) {
            dl->AddRect(img_pos, {img_pos.x + img_w, img_pos.y + img_h},
                       IM_COL32(100, 150, 255, 200), 0.0f, 0, 4.0f);
        }

        // Draw saved annotations on top of page
        for (const auto& hl : page.annots.highlights) {
            ImVec2 tl = {img_pos.x + hl.x0 * img_w, img_pos.y + hl.y0 * img_h};
            ImVec2 br = {img_pos.x + hl.x1 * img_w, img_pos.y + hl.y1 * img_h};
            dl->AddRectFilled(tl, br, IM_COL32(255, 224, 0, 80));
        }
        for (const auto& stroke : page.annots.strokes) {
            for (int si = 1; si < (int)stroke.pts.size(); ++si) {
                ImVec2 a = {img_pos.x + stroke.pts[si-1].x * img_w,
                            img_pos.y + stroke.pts[si-1].y * img_h};
                ImVec2 b = {img_pos.x + stroke.pts[si  ].x * img_w,
                            img_pos.y + stroke.pts[si  ].y * img_h};
                dl->AddLine(a, b,
                    IM_COL32((int)(stroke.r*255), (int)(stroke.g*255), (int)(stroke.b*255), 220),
                    2.0f);
            }
        }

        // Note badges — stacked from top-right corner, left to right.
        // When the Note tool is active, clicking a badge removes that flag.
        {
            constexpr float NBW = 16.0f, NBH = 16.0f, NGAP = 2.0f;
            float nbx = img_pos.x + img_w;
            int remove_idx = -1;
            for (int ni = 0; ni < (int)page.annots.notes.size(); ++ni) {
                nbx -= NBW + NGAP;
                ImVec2 ntl = {nbx,        img_pos.y};
                ImVec2 nbr = {nbx + NBW,  img_pos.y + NBH};
                dl->AddRectFilled(ntl, nbr, IM_COL32(50, 110, 210, 230), 2.0f);
                const auto& note = page.annots.notes[ni];
                ImVec2 tsz = ImGui::CalcTextSize(note.label.c_str());
                dl->AddText({ntl.x + (NBW - tsz.x) * 0.5f, ntl.y + (NBH - tsz.y) * 0.5f},
                            IM_COL32(255, 255, 255, 255), note.label.c_str());
                if (g_annot_tool == AnnotTool::Note) {
                    char bid[40];
                    snprintf(bid, sizeof(bid), "##rmflag_%d_%d", page.page_index, ni);
                    ImGui::SetCursorScreenPos(ntl);
                    if (ImGui::InvisibleButton(bid, {NBW, NBH}))
                        remove_idx = ni;
                }
            }
            if (remove_idx >= 0) {
                UndoRecord r;
                r.type           = UndoRecord::Type::ErasedNote;
                r.doc_idx        = doc_idx;
                r.page_idx       = page.page_index;
                r.erased_note    = page.annots.notes[remove_idx];
                r.erased_note_at = remove_idx;
                push_undo(r);
                page.annots.notes.erase(page.annots.notes.begin() + remove_idx);
            }
        }

        // Mouse capture overlay when a tool is active
        if (g_annot_tool != AnnotTool::None) {
            ImGui::SetCursorScreenPos(img_pos);
            char btn_id[32];
            snprintf(btn_id, sizeof(btn_id), "##ann_%d", page.page_index);
            ImGui::InvisibleButton(btn_id, {img_w, img_h});
            bool pressed = ImGui::IsItemActivated();
            bool active  = ImGui::IsItemActive();

            ImVec2 mpos = ImGui::GetMousePos();
            float nx = std::clamp((mpos.x - img_pos.x) / img_w, 0.0f, 1.0f);
            float ny = std::clamp((mpos.y - img_pos.y) / img_h, 0.0f, 1.0f);

            // --- Eraser tool ---
            if (g_annot_tool == AnnotTool::Eraser && active) {
                constexpr float ER = 0.025f;  // eraser radius in normalized page coords
                // Erase highlights
                for (int hi = (int)page.annots.highlights.size() - 1; hi >= 0; --hi) {
                    auto& hl = page.annots.highlights[hi];
                    float cx = std::clamp(nx, hl.x0, hl.x1);
                    float cy = std::clamp(ny, hl.y0, hl.y1);
                    if ((nx-cx)*(nx-cx) + (ny-cy)*(ny-cy) <= ER*ER) {
                        UndoRecord r;
                        r.type             = UndoRecord::Type::ErasedHighlight;
                        r.doc_idx          = doc_idx;
                        r.page_idx         = page.page_index;
                        r.erased_highlight = hl;
                        push_undo(r);
                        page.annots.highlights.erase(page.annots.highlights.begin() + hi);
                    }
                }
                // Erase strokes — remove any stroke with a point within the eraser circle
                for (int si = (int)page.annots.strokes.size() - 1; si >= 0; --si) {
                    bool hit = false;
                    for (auto& pt : page.annots.strokes[si].pts) {
                        float dx = nx - pt.x, dy = ny - pt.y;
                        if (dx*dx + dy*dy <= ER*ER) { hit = true; break; }
                    }
                    if (hit) {
                        UndoRecord r;
                        r.type          = UndoRecord::Type::ErasedStroke;
                        r.doc_idx       = doc_idx;
                        r.page_idx      = page.page_index;
                        r.erased_stroke = page.annots.strokes[si];
                        push_undo(r);
                        page.annots.strokes.erase(page.annots.strokes.begin() + si);
                    }
                }
            }

            // --- Draw/stamp tools ---
            if (g_annot_tool != AnnotTool::Eraser) {
                if (pressed) {
                    if (g_annot_tool == AnnotTool::Note) {
                        int snap = g_next_note_idx;
                        page.annots.notes.push_back({note_label(g_next_note_idx++)});
                        UndoRecord r;
                        r.type            = UndoRecord::Type::Note;
                        r.doc_idx         = doc_idx;
                        r.page_idx        = page.page_index;
                        r.note_idx_before = snap;
                        push_undo(r);
                    } else {
                        g_ann_drawing  = true;
                        g_ann_doc_idx  = doc_idx;
                        g_ann_page_idx = page.page_index;
                        if (g_annot_tool == AnnotTool::Pen) {
                            g_ann_cur_stroke = {};
                            g_ann_cur_stroke.r = g_pen_r;
                            g_ann_cur_stroke.g = g_pen_g;
                            g_ann_cur_stroke.b = g_pen_b;
                            g_ann_cur_stroke.pts.push_back({nx, ny});
                        } else {
                            g_ann_hl_start = {nx, ny};
                            g_ann_cur_norm = {nx, ny};
                        }
                    }
                }

                bool this_page_drawing = g_ann_drawing
                    && g_ann_doc_idx  == doc_idx
                    && g_ann_page_idx == page.page_index;

                if (active && this_page_drawing) {
                    if (g_annot_tool == AnnotTool::Pen)
                        g_ann_cur_stroke.pts.push_back({nx, ny});
                    else
                        g_ann_cur_norm = {nx, ny};
                }

                // Live preview
                if (this_page_drawing) {
                    if (g_annot_tool == AnnotTool::Pen) {
                        for (int si = 1; si < (int)g_ann_cur_stroke.pts.size(); ++si) {
                            ImVec2 a = {img_pos.x + g_ann_cur_stroke.pts[si-1].x * img_w,
                                        img_pos.y + g_ann_cur_stroke.pts[si-1].y * img_h};
                            ImVec2 b = {img_pos.x + g_ann_cur_stroke.pts[si  ].x * img_w,
                                        img_pos.y + g_ann_cur_stroke.pts[si  ].y * img_h};
                            dl->AddLine(a, b, IM_COL32((int)(g_pen_r*255),(int)(g_pen_g*255),(int)(g_pen_b*255),220), 2.0f);
                        }
                    } else {
                        float x0 = std::min(g_ann_hl_start.x, g_ann_cur_norm.x);
                        float y0 = std::min(g_ann_hl_start.y, g_ann_cur_norm.y);
                        float x1 = std::max(g_ann_hl_start.x, g_ann_cur_norm.x);
                        float y1 = std::max(g_ann_hl_start.y, g_ann_cur_norm.y);
                        ImVec2 tl = {img_pos.x + x0 * img_w, img_pos.y + y0 * img_h};
                        ImVec2 br = {img_pos.x + x1 * img_w, img_pos.y + y1 * img_h};
                        dl->AddRectFilled(tl, br, IM_COL32(255, 224, 0, 80));
                    }
                }
            }
        }

        if (scroll_to == page.page_index) {
            ImGui::SetScrollHereY(0.0f);
            g_input.clear_panel_scroll();
        }

        ImGui::Spacing();
    }

    ImGui::EndChild();
    ImGui::EndTabItem();
    } // Document tab

    if (ImGui::BeginTabItem("References")) {
        ImGui::BeginChild("##panel_ref_scroll", {0.0f, 0.0f}, false, ImGuiWindowFlags_None);
        ImGui::Spacing();
        draw_references_tab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

// --- Canvas note badges (drawn via ImGui background list, on top of GL) ----

static void draw_canvas_note_badges() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2      vp = ImGui::GetMainViewport()->Size;

    constexpr float BW = 20.0f, BH = 20.0f, GAP = 3.0f;

    for (const auto& doc : g_documents) {
        for (const auto& page : doc.pages) {
            if (page.annots.notes.empty()) continue;

            // Top-right corner of page in screen space
            Vec2 tr = g_canvas.world_to_screen({page.world_pos.x + page.world_w,
                                                page.world_pos.y});

            // Rough visibility cull
            float span = (BW + GAP) * (float)page.annots.notes.size();
            if (tr.x + 20.0f < 0.0f || tr.x - span > vp.x) continue;
            if (tr.y + BH    < 0.0f || tr.y          > vp.y) continue;

            float cx = tr.x;
            for (const auto& note : page.annots.notes) {
                cx -= BW + GAP;
                ImVec2 tl = {cx,      tr.y};
                ImVec2 br = {cx + BW, tr.y + BH};
                dl->AddRectFilled(tl, br, IM_COL32(50, 110, 210, 220), 3.0f);
                ImVec2 tsz = ImGui::CalcTextSize(note.label.c_str());
                dl->AddText({tl.x + (BW - tsz.x) * 0.5f, tl.y + (BH - tsz.y) * 0.5f},
                            IM_COL32(255, 255, 255, 255), note.label.c_str());
            }
        }
    }
}

// --- Panel resize handle ----------------------------------------------------

static void draw_panel_resize_handle() {
    if (!g_input.panel_open()) return;

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    constexpr float STRIP_W = 8.0f;

    ImGui::SetNextWindowPos({vp.x - s_panel_w - STRIP_W * 0.5f, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({STRIP_W, vp.y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0f, 1.0f});
    constexpr ImGuiWindowFlags kRFlags =
        ImGuiWindowFlags_NoTitleBar          |
        ImGuiWindowFlags_NoResize            |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoScrollbar         |
        ImGuiWindowFlags_NoSavedSettings     |
        ImGuiWindowFlags_NoBackground        |
        ImGuiWindowFlags_NoFocusOnAppearing  |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##panel_resize", nullptr, kRFlags);
    ImGui::PopStyleVar(2);

    ImGui::InvisibleButton("##drag", {STRIP_W, vp.y});
    bool was_dragging = ImGui::IsItemActive();
    if (was_dragging) {
        s_panel_w -= ImGui::GetIO().MouseDelta.x;
        s_panel_w  = std::clamp(s_panel_w, 180.0f, vp.x - 30.0f);
    } else if (ImGui::IsItemDeactivated()) {
        g_settings.panel_w = s_panel_w;
        save_prefs();
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImGui::End();
}

// --- Page tooltip -----------------------------------------------------------

static void draw_page_tooltip() {
    const Page*     hov_page = g_input.hovered_page();
    const Document* hov_doc  = g_input.hovered_doc();
    if (!hov_page || !hov_doc) return;

    // Track how long this specific page has been continuously hovered.
    static const Page* s_last_page  = nullptr;
    static double      s_hover_t0   = 0.0;

    double now = ImGui::GetTime();
    if (hov_page != s_last_page) {
        s_last_page = hov_page;
        s_hover_t0  = now;
    }

    static constexpr double DELAY = 0.7;
    if (now - s_hover_t0 < DELAY) return;

    namespace fs = std::filesystem;
    std::string fname = fs::path(hov_doc->path).filename().string();

    ImGui::BeginTooltip();

    // Filename in normal color
    ImGui::TextUnformatted(fname.c_str());

    // Full path dimmed on a second line
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.58f, 1.0f));
    ImGui::TextUnformatted(hov_doc->path.c_str());
    ImGui::PopStyleColor();

    ImGui::EndTooltip();
}

// --- Search panel (Cmd+F) ---------------------------------------------------

static void run_search() {
    g_search_results.clear();
    std::string q(g_search_buf);
    if (q.empty()) return;

    namespace fs = std::filesystem;
    for (int di = 0; di < (int)g_documents.size(); ++di) {
        PdfLoader tmp;
        Document  dummy;
        if (!tmp.load(g_documents[di].path, dummy)) continue;
        auto hits = tmp.search_text(q, 50);
        std::string name = fs::path(g_documents[di].path).filename().string();
        for (auto& h : hits)
            g_search_results.push_back({di, h.page_index, std::move(h.excerpt), name});
    }
}

static void draw_search_panel() {
    if (!g_search_open) return;

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos({vp.x * 0.5f - 260.0f, 50.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({520.0f, 440.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);

    if (!ImGui::Begin("Search##search_panel", &g_search_open,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##sq", g_search_buf, sizeof(g_search_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);

    // Clear highlight if search term changed
    if (g_last_search_term != std::string(g_search_buf)) {
        g_highlighted_search_result = -1;
        g_last_search_term = std::string(g_search_buf);
    }

    ImGui::SameLine();
    if (ImGui::Button("Search") || enter) run_search();
    ImGui::SameLine();
    ImGui::TextDisabled("%d result%s", (int)g_search_results.size(),
                        g_search_results.size() == 1 ? "" : "s");

    // Escape closes search and clears highlight
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_search_open = false;
        g_highlighted_search_result = -1;
        ImGui::End();
        return;
    }

    ImGui::Separator();
    ImGui::BeginChild("##search_results", {0.0f, 0.0f}, false);

    for (int i = 0; i < (int)g_search_results.size(); ++i) {
        auto& hit = g_search_results[i];
        ImGui::PushID(&hit);

        float hr = 0.5f, hg = 0.5f, hb = 0.5f;
        if (hit.doc_idx < (int)g_documents.size()) {
            hr = g_documents[hit.doc_idx].hue_r;
            hg = g_documents[hit.doc_idx].hue_g;
            hb = g_documents[hit.doc_idx].hue_b;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(hr, hg, hb, 1.0f));
        ImGui::TextUnformatted(hit.doc_name.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("p.%d", hit.page_idx + 1);

        bool clicked = ImGui::IsItemClicked();
        ImGui::TextWrapped("%s", hit.excerpt.c_str());
        clicked = clicked || ImGui::IsItemClicked();

        if (clicked && hit.doc_idx < (int)g_documents.size()) {
            Document& doc = g_documents[hit.doc_idx];
            // Zoom canvas to the target page
            Page& pg = doc.pages[std::min(hit.page_idx, (int)doc.pages.size() - 1)];
            zoom_to_rect(pg.world_pos.x, pg.world_pos.y,
                         pg.world_pos.x + pg.world_w,
                         pg.world_pos.y + pg.world_h);
            g_input.open_panel(hit.doc_idx, hit.page_idx);
            g_highlighted_search_result = i;  // Mark this result as highlighted
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

// --- Toolbar UI -------------------------------------------------------------

static void draw_toolbar_ui() {
    ImGui::SetNextWindowPos({14.0f, 14.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f, 6.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  {10.0f, 5.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   {6.0f, 4.0f});
    ImGui::Begin("##toolbar", nullptr, kFlags);
    ImGui::PopStyleVar(3);

    if (ImGui::Button("+")) ImGui::OpenPopup("add_popup");
    ImGui::SetItemTooltip("Add PDFs - from folder, file, or URL");
    ImGui::SameLine();
    bool text_was_active = g_text_tool;
    if (text_was_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
    if (ImGui::Button("T")) {
        g_text_tool    = !g_text_tool;
        g_editing_box  = -1;
    }
    if (text_was_active) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Text box tool - click canvas to insert (Escape to cancel)");

    if (g_text_tool || g_selected_box >= 0) {
        ImGui::SameLine();
        float tcol[3] = {g_tbox_r, g_tbox_g, g_tbox_b};
        if (ImGui::ColorEdit3("##tboxcolor", tcol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            g_tbox_r = tcol[0]; g_tbox_g = tcol[1]; g_tbox_b = tcol[2];
            // Apply to the selected box only (covers the editing box too).
            for (auto& b : g_text_boxes)
                if (b.id == g_selected_box) { b.r = tcol[0]; b.g = tcol[1]; b.b = tcol[2]; break; }
        }
        ImGui::SetItemTooltip("Text color");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(52.0f);
        if (ImGui::DragFloat("##tboxsz", &g_tbox_font_size, 0.5f, 10.0f, 48.0f, "%.0fpt")) {
            for (auto& b : g_text_boxes)
                if (b.id == g_selected_box) { b.font_size = g_tbox_font_size; break; }
        }
        ImGui::SetItemTooltip("Font size (drag to adjust)");
    }

    static bool open_url_modal = false;

    if (ImGui::BeginPopup("add_popup")) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f, 6.0f});

        if (ImGui::MenuItem("Add from folder...")) {
#ifdef __APPLE__
            scholion_activate_app();
#endif
            const char* path = tinyfd_selectFolderDialog("Select PDF folder", nullptr);
            if (path) load_pdfs_from_folder(path);
        }
        if (ImGui::MenuItem("Add from file...")) {
#ifdef __APPLE__
            scholion_activate_app();
#endif
            const char* patterns[] = {"*.pdf", "*.PDF"};
            const char* result = tinyfd_openFileDialog(
                "Select PDF files", nullptr,
                2, patterns, "PDF Documents", 1);
            if (result) load_pdfs_from_selection(result);
        }
        if (ImGui::MenuItem("Add from URL...")) {
            open_url_modal = true;
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    // Open the modal from outside the popup so ImGui sees it at the right level.
    if (open_url_modal) {
        ImGui::OpenPopup("Add from URL##modal");
        open_url_modal = false;
    }
    draw_url_modal();

    ImGui::End();
}

// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    signal(SIGINT,  scholion_signal_handler);
    signal(SIGTERM, scholion_signal_handler);
    g_debug = (std::getenv("SCHOLION_DEBUG") != nullptr);

#ifdef __APPLE__
    scholion_register_early();   // registers WillFinishLaunching observer before glfwInit
#endif
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }
#ifdef __APPLE__
    scholion_register_file_handler();  // re-register after glfwInit to override NSApp's default handler
#endif

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    g_window = glfwCreateWindow(1280, 800, "Scholion", nullptr, nullptr);
    GLFWwindow* window = g_window;
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);

#ifdef _WIN32
    // Load all OpenGL 3.3 core function pointers via GLAD.
    // Must happen after a GL context is made current; before any GL calls.
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialise GLAD OpenGL loader\n");
        glfwTerminate();
        return 1;
    }
#endif

#ifdef _WIN32
    // Disable VSync on Windows — some GPU drivers deadlock inside glfwSwapBuffers
    // when swap interval is 1 (causes hang-on-first-frame on affected hardware).
    // Frame rate is still capped by glfwWaitEventsTimeout(0.016) in the main loop.
    glfwSwapInterval(0);
#else
    glfwSwapInterval(1);
#endif

    g_cursor_hand  = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
    g_cursor_arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);

    // Set our callbacks BEFORE ImGui init so ImGui can chain to them.
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_pos_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetWindowFocusCallback(window, focus_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetDropCallback(window, drop_callback);

    int win_w, win_h;
    glfwGetWindowSize(window, &win_w, &win_h);
    g_canvas.set_viewport_size(static_cast<float>(win_w), static_cast<float>(win_h));

    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = nullptr;
    try { load_prefs(); } catch (...) {}   // guard against filesystem exceptions on second run
    apply_theme(g_settings.dark_mode);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    Renderer renderer;
    if (!renderer.init()) {
        fprintf(stderr, "Failed to initialize renderer\n");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Bake an elliptical vignette gradient texture (256×256, single upload).
    // Stretched to the full viewport each frame: the circle in texture-space
    // becomes a viewport-filling ellipse, giving smooth per-edge falloff with
    // no corner doubling or visible rectangle edges.
    {
        constexpr int V = 256;
        std::vector<uint8_t> px(V * V * 4, 0);
        for (int y = 0; y < V; ++y) {
            for (int x = 0; x < V; ++x) {
                float nx = (x / (float)(V - 1)) * 2.0f - 1.0f;  // [-1, 1]
                float ny = (y / (float)(V - 1)) * 2.0f - 1.0f;
                float r  = sqrtf(nx * nx + ny * ny);
                // Smoothstep from inner edge (0.45) to outer clamp (1.15).
                // At r=1.0 (viewport edge midpoints) alpha ≈ 72; at corners
                // (r≈1.41) clamped to max alpha 82 — smooth, no jump.
                float t = std::clamp((r - 0.45f) / (1.15f - 0.45f), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);  // smoothstep curve
                px[(y * V + x) * 4 + 3] = (uint8_t)(t * 82.0f);  // alpha only; RGB stays 0
            }
        }
        glGenTextures(1, &g_vignette_tex);
        glBindTexture(GL_TEXTURE_2D, g_vignette_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, V, V, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    PerformanceOverlay overlay;
    double last_frame_time = glfwGetTime();

    printf("Scholion v0.5.0 — Milestones 1–10 complete\n");
#ifdef SCHOLION_HAVE_MUPDF
    printf("  MuPDF: enabled\n");
#else
    printf("  MuPDF: NOT built — PDF loading will show a warning\n");
#endif
    printf("Controls:\n");
    printf("  + button (top-left) or drag PDF/folder — load document(s)\n");
    printf("  Left click page — open panel viewer, scroll to that page\n");
    printf("  Left drag page — move page freely on canvas\n");
    printf("  Right click page — context menu (Return to stack / View in panel)\n");
    printf("  Middle mouse / Space+drag — pan\n");
    printf("  Scroll wheel — zoom (cursor-centered)\n");
    printf("  Escape — clear selection, then close panel\n");
  printf("  Shift+click doc — toggle document in/out of selection\n");
  printf("  Drag empty canvas — rubber-band box select\n");
  printf("  Cmd+A — select all pages\n");
  printf("  Right-click empty canvas — save project\n");

    try { load_recents(); } catch (...) {}   // guard against filesystem exceptions on second run
    s_panel_w = g_settings.panel_w;
    g_rast_thread = std::thread(rast_worker);

#ifndef __APPLE__
    // On macOS a file argument is delivered via the open-document Apple event
    // (drained at startup / in the main loop), so processing argv there too would
    // load it twice. Other platforms have no such event — handle argv directly.
    for (int i = 1; i < argc; ++i) {
        printf("Loading: %s\n", argv[i]);
        if (std::filesystem::path(argv[i]).extension() == ".scholion")
            load_project_from_path(argv[i]);   // open a saved project from the CLI
        else
            load_pdf(argv[i]);
    }
#endif

    // Startup chooser — shown only on a bare launch (no document).
    // Two suppression paths:
    //   argc > 1          — file passed on the CLI (non-Apple platforms).
    //   kAEOpenDocuments  — macOS file-association launch (double-click or Finder
    //                       "Open With"). The Apple event arrives almost immediately
    //                       but asynchronously; the 0.25 s grace window below pumps
    //                       the event loop so we drain it before deciding whether to
    //                       show the chooser. Without the grace window, a cold launch
    //                       would briefly show the chooser before the event arrived.
    {
        bool launched_with_doc = (argc > 1);
#ifdef __APPLE__
        // The open-document Apple event arrives almost immediately on a cold
        // file-association launch. Pump the event loop for a brief grace window
        // so we don't race it, draining anything that lands exactly as the main
        // loop would. Skip entirely if a document was already given on the CLI
        // (otherwise the same file loads twice).
        char  pending_path[4096];
        double probe_start = glfwGetTime();
        while (!launched_with_doc && glfwGetTime() - probe_start < 0.25) {
            glfwWaitEventsTimeout(0.01);
            bool got = false;
            while (scholion_pop_pending_open(pending_path, sizeof(pending_path))) {
                namespace fs = std::filesystem;
                if (fs::path(pending_path).extension() == ".scholion")
                    load_project_from_path(pending_path);
                else
                    load_pdf(pending_path);
                launched_with_doc = true;
                got = true;
            }
            if (got) break;
        }
#endif
        // Bare launch: show the startup chooser (open project / add PDF / add
        // folder / blank). Drawn in the main loop below.
        if (!launched_with_doc)
            g_startup_chooser = true;
    }

    while (!glfwWindowShouldClose(window)) {
        // Graceful exit on SIGINT/SIGTERM — autosave before breaking
        if (g_signal_received) {
            if (!g_project_path.empty() && !g_documents.empty()) {
                fprintf(stderr, "Signal %d — saving %s\n",
                        (int)g_signal_received, g_project_path.c_str());
                save_to_path(g_project_path);
            }
            break;
        }

#ifdef _WIN32
        // glfwWaitEventsTimeout (MsgWaitForMultipleObjectsEx) has been observed
        // to block indefinitely on some Windows GPU/driver configurations, causing
        // the window to freeze after the first rendered frame. Use PollEvents +
        // Sleep instead: same ~60fps cap, simpler Win32 code path, no hang risk.
        glfwPollEvents();
        Sleep(g_settings.compat_mode ? 33 : 16);
#else
        glfwWaitEventsTimeout(g_settings.compat_mode ? 0.033 : 0.016);
#endif

        // Per-frame timing for the performance overlay
        double now_t   = glfwGetTime();
        float  delta_t = static_cast<float>(now_t - last_frame_time);
        last_frame_time = now_t;
        overlay.update(delta_t);
        if (g_input.consume_overlay_toggle()) overlay.toggle();

        g_input.update(window);
        drain_rast_results();
#ifdef __APPLE__
        {
            char pending_path[4096];
            while (scholion_pop_pending_open(pending_path, sizeof(pending_path))) {
                namespace fs = std::filesystem;
                if (fs::path(pending_path).extension() == ".scholion")
                    load_project_from_path(pending_path);
                else
                    load_pdf(pending_path);
            }
        }
#endif
        stream_lod();

        // --- Page drag undo tracking -------------------------------------------
        // Snapshot positions when a drag begins; push undo record when it ends.
        {
            const Page* cur_pending = g_input.pending_drag_page();
            const Page* cur_dragged = g_input.dragged_page();
            bool        cur_multi   = g_input.is_multi_dragging();

            // Single-page drag: capture at mouse-down (pending), commit on release
            if (cur_pending && cur_pending != g_drag_snap_page) {
                g_drag_snap_page = cur_pending;
                g_drag_snap_pos  = cur_pending->world_pos;
            }
            if (g_was_page_dragging && !cur_dragged && g_drag_snap_page) {
                // Drag just ended — push undo if the page actually moved
                Vec2 new_pos = g_drag_snap_page->world_pos;
                if (new_pos.x != g_drag_snap_pos.x || new_pos.y != g_drag_snap_pos.y) {
                    UndoRecord r;
                    r.type = UndoRecord::Type::PageMove;
                    r.page_moves.push_back({const_cast<Page*>(g_drag_snap_page), g_drag_snap_pos});
                    push_undo(r);
                }
                g_drag_snap_page = nullptr;
            }
            g_was_page_dragging = (cur_dragged != nullptr);

            // Multi-page drag: snapshot all selected pages when drag begins
            if (!g_was_multi_drag && cur_multi) {
                g_multi_drag_snaps.clear();
                for (Page* p : g_input.selection())
                    g_multi_drag_snaps.push_back({p, p->world_pos});
                // If drag started from a page (not a text box), also track text boxes.
                // g_page_drag_states is not needed here — pages move via on_cursor_move.
                g_page_drag_states.clear();
                if (!g_box_dragging && !g_input.selected_text_boxes().empty()) {
                    g_box_drag_start_world = g_input.multi_drag_grab_world();
                    g_box_drag_states.clear();
                    for (int sid : g_input.selected_text_boxes()) {
                        for (const auto& b : g_text_boxes) {
                            if (b.id == sid) {
                                g_box_drag_states.push_back({sid, b.world_pos});
                                break;
                            }
                        }
                    }
                    g_box_dragging = true;
                }
            }
            if (g_was_multi_drag && !cur_multi && !g_multi_drag_snaps.empty()) {
                UndoRecord r;
                r.type = UndoRecord::Type::PageMove;
                for (auto& snap : g_multi_drag_snaps)
                    if (snap.page->world_pos.x != snap.old_pos.x ||
                        snap.page->world_pos.y != snap.old_pos.y)
                        r.page_moves.push_back({snap.page, snap.old_pos});
                if (!r.page_moves.empty()) push_undo(r);
                g_multi_drag_snaps.clear();
            }
            g_was_multi_drag = cur_multi;
        }

        glfwGetWindowSize(window, &win_w, &win_h);
        g_canvas.set_viewport_size(static_cast<float>(win_w), static_cast<float>(win_h));

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);

        // Update cursor shape based on hover/drag state
        glfwSetCursor(window,
            (g_input.hovered_page() || g_input.dragged_page() || g_input.is_multi_dragging())
                ? g_cursor_hand : nullptr);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Elliptical vignette — single texture stretched to viewport.
        // The texture is a circular alpha gradient baked at startup; stretching
        // it to the viewport turns the circle into an ellipse that fits the screen
        // with smooth per-edge falloff and no corner-doubling artefact.
        if (g_settings.vignette_on && g_vignette_tex) {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            ImVec2 vp      = ImGui::GetMainViewport()->Size;
            dl->AddImage(
                (ImTextureID)(uintptr_t)g_vignette_tex,
                {0.0f, 0.0f}, {vp.x, vp.y});
        }

        try {

        // Escape: confirm+close editing → then deactivate text tool → then deactivate annot tool
        if (!g_search_open && !ImGui::GetIO().WantCaptureKeyboard
            && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (g_editing_box >= 0) {
                g_editing_box = -1;          // confirm text, keep tool active
            } else if (g_text_tool) {
                g_text_tool = false;
            } else if (g_annot_tool != AnnotTool::None) {
                g_annot_tool  = AnnotTool::None;
                g_ann_drawing = false;
            }
        }

        // Finalize annotation when mouse is released mid-draw
        if (g_ann_drawing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            finalize_annotation();

        draw_cursor_tool_icon();
        draw_canvas_text_boxes();

        // Double-click on a page -> open panel scrolled to that page
        if (!ImGui::GetIO().WantCaptureMouse && g_hovered_box < 0
                && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const Page*     dbl_page = g_input.hovered_page();
            const Document* dbl_doc  = g_input.hovered_doc();
            if (dbl_page && dbl_doc) {
                for (int i = 0; i < (int)g_documents.size(); ++i) {
                    if (&g_documents[i] == dbl_doc) {
                        g_input.open_panel(i, dbl_page->page_index);
                        break;
                    }
                }
            }
        }

        // Middle mouse double-click -> Zoom to Fit
        if (!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
            zoom_to_fit();
        }

        // Space tap (no drag) -> toggle panel for the selected document.
        // WantCaptureKeyboard is intentionally NOT checked here so spacebar
        // closes the panel even when the sidebar has keyboard focus.
        if (g_input.consume_space_tap() && !g_text_tool && !g_search_open) {
            if (g_input.panel_open()) {
                g_input.close_panel();
            } else {
                const Document* sel_doc = g_input.selected_doc();
                if (sel_doc) {
                    for (int i = 0; i < (int)g_documents.size(); ++i) {
                        if (&g_documents[i] == sel_doc) {
                            int scroll_page = -1;
                            for (int pi = 0; pi < (int)sel_doc->pages.size(); ++pi) {
                                if (g_input.selection().count(
                                        const_cast<Page*>(&sel_doc->pages[pi]))) {
                                    scroll_page = pi;
                                    break;
                                }
                            }
                            g_input.open_panel(i, scroll_page);
                            break;
                        }
                    }
                }
            }
        }

        draw_canvas_note_badges();
        draw_toolbar_ui();
        draw_context_menu();
        draw_canvas_context_menu();
        draw_settings_popup();
        draw_quit_dialog();
        draw_startup_chooser();
        draw_panel_ui();
        draw_panel_resize_handle();   // rendered after panel so it sits on top
        draw_page_tooltip();
        draw_search_panel();

        DrawHints hints;
        hints.selected_doc   = g_input.selected_doc();
        hints.hovered_page   = g_input.hovered_page();
        hints.dragged_page   = g_input.dragged_page();
        hints.selection      = &g_input.selection();
        hints.box_selecting  = g_input.box_selecting();
        hints.box_start_world = g_input.box_start_world();
        hints.box_cur_world   = g_input.box_cur_world();
        hints.grid_mode      = g_settings.grid_mode;
        hints.dark_mode      = g_settings.dark_mode;
        renderer.draw(g_canvas, g_documents, hints);

        // Page counter: visible pages / total pages across all documents.
        {
            int total_pages = 0, visible_pages = 0;
            for (const auto& doc : g_documents)
                for (const auto& pg : doc.pages) {
                    ++total_pages;
                    if (is_page_visible(pg)) ++visible_pages;
                }
            overlay.set_page_count(visible_pages, total_pages);
        }

        // Render save feedback (temporary "Saved!" message)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - g_save_feedback_time).count();
            float fade_duration_ms = (g_save_feedback_type == SaveFeedbackType::Manual) ? 2500.0f : 500.0f;

            if (g_save_feedback_type != SaveFeedbackType::None && elapsed_ms < fade_duration_ms) {
                float alpha = 1.0f - (float)elapsed_ms / fade_duration_ms;
                const char* msg = (g_save_feedback_type == SaveFeedbackType::Manual) ? "Saved!" : "saved";

                // Position under the + and T toolbar buttons.
                ImGui::SetNextWindowPos({20.0f, 112.0f}, ImGuiCond_Always, {0.0f, 0.0f});
                ImGui::SetNextWindowBgAlpha(0.55f * alpha);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f, 5.0f});
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
                ImGui::Begin("##save_feedback", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::SetWindowFontScale(1.2f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.9f, alpha));
                ImGui::TextUnformatted(msg);
                ImGui::PopStyleColor();
                ImGui::SetWindowFontScale(1.0f);
                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);
            } else if (elapsed_ms >= fade_duration_ms) {
                g_save_feedback_type = SaveFeedbackType::None;  // reset after fade
            }
        }

        overlay.draw(g_canvas);

        } catch (const std::exception& e) {
            fprintf(stderr, "Frame error: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "Unknown frame error — continuing\n");
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Autosave every 60 s. g_last_save_time resets on manual save (Cmd+S,
        // right-click Save) and on project open, so a user who just saved doesn't
        // get an autosave 1 s later. No autosave when no project path is set yet
        // (new blank session) to avoid a Save-As dialog popping up unexpectedly.
        if (!g_project_path.empty() && !g_documents.empty()) {
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - g_last_save_time).count();
            if (elapsed >= 60) {
                g_save_feedback_type = SaveFeedbackType::Auto;  // override to show auto-save feedback
                if (save_to_path(g_project_path))
                    printf("Autosaved: %s\n", g_project_path.c_str());
                g_last_save_time = now;
            }
        }

        glfwSwapBuffers(window);

        // Exit the main loop if quit was confirmed
        if (g_quit_state == QuitState::Confirmed) {
            break;
        }
    }

    // Final save on clean exit (red button / Cmd+Q) so the last edits since the
    // previous autosave aren't lost. Only when a project path is already set.
    if (!g_project_path.empty() && !g_documents.empty()) {
        if (save_to_path(g_project_path))
            printf("Saved on exit: %s\n", g_project_path.c_str());
    }

    // Stop background workers before cleaning up shared state.
    if (g_dl_state.load() == 1) cancel_download();
    if (g_dl_thread.joinable()) g_dl_thread.join();

    g_rast_stop.store(true);
    g_rast_cv.notify_one();
    if (g_rast_thread.joinable()) g_rast_thread.join();

    if (g_vignette_tex) { glDeleteTextures(1, &g_vignette_tex); g_vignette_tex = 0; }
    if (g_cursor_hand)  { glfwDestroyCursor(g_cursor_hand);  g_cursor_hand  = nullptr; }
    if (g_cursor_arrow) { glfwDestroyCursor(g_cursor_arrow); g_cursor_arrow = nullptr; }

    clear_documents();
    renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
