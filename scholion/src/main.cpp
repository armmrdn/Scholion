#include "app_settings.h"
#include "app_state.h"
#include "version.h"
#include "logo_data.h"
#include "font_data.h"
#include "canvas.h"
#include "canvas_annot.h"
#include "canvas_text_box.h"
#include "input.h"
#include "project_io.h"
#include "rast_pipeline.h"
#include "renderer.h"
#include "overlay.h"
#include "pdf_loader.h"
#include "save_feedback.h"
#include "texture_cache.h"
#include "undo.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
extern "C" void        scholion_register_early(void);
extern "C" void        scholion_register_file_handler(void);
extern "C" int         scholion_pop_pending_open(char* buf, int buf_len);
extern "C" void        scholion_activate_app(void);
extern "C" void        scholion_prewarm_dialogs(void);
extern "C" const char* scholion_open_file(const char* title,
                                          const char** ext_patterns, int n_ext,
                                          int allow_multi);
extern "C" const char* scholion_save_file(const char* title, const char* default_name,
                                          const char** ext_patterns, int n_ext);
extern "C" const char* scholion_select_folder(const char* title);
#include <unistd.h>           // fork / execl
#elif defined(_WIN32)
// Windows: GLAD must be included before GLFW so it wins the GL symbol race.
// GLFW_INCLUDE_NONE prevents GLFW from pulling in its own GL headers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW (reveal in Explorer)
#include <shlobj.h>     // SHGetKnownFolderPath, FOLDERID_*
#include <dwmapi.h>     // DwmSetWindowAttribute — dark title bar
#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#else
// Linux: GLAD provides OpenGL function pointers (same as Windows).
// GLFW_INCLUDE_NONE prevents GLFW from pulling in its own GL headers.
#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <unistd.h>   // fork / execl / _exit
#endif

#include <GLFW/glfw3.h>

#ifdef _WIN32
// glfw3native.h uses GLFWAPI which is defined in glfw3.h — must come after.
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // glfwGetWin32Window
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "tinyfiledialogs.h"
#include <curl/curl.h>


// --- Globals for GLFW callbacks (GLFW C callbacks can't capture state) ------
// Globals declared extern in app_state.h are defined here (no static).

Canvas       g_canvas;
InputHandler g_input(g_canvas);

static GLFWcursor* g_cursor_hand  = nullptr;
static GLFWcursor* g_cursor_arrow = nullptr;

GLFWwindow*                              g_window  = nullptr;
std::vector<Document>                    g_documents;
std::vector<PageGroup>                   g_groups;
int                                      g_next_group_id = 1;
uint64_t                                 g_next_page_id  = 1;   // monotonic; 0 = unassigned
std::vector<std::shared_ptr<PdfLoader>>  g_loaders;
TextureCache                             g_cache;
float                                    g_content_scale = 1.0f;
std::unordered_map<std::string, uint32_t> g_tile_cache;
bool                                     g_debug   = false;

// --- Definitions for globals declared extern in headers ---------------------
// (types are defined in their own headers; only the instances live here)

SaveFeedbackType                          g_save_feedback_type = SaveFeedbackType::None;
std::chrono::steady_clock::time_point     g_save_feedback_time;

AppSettings  g_settings;
static bool  g_settings_open = false;

static GLuint g_vignette_tex = 0;  // elliptical gradient texture, created once after GL init
static GLuint g_logo_tex     = 0;  // app logo (embedded), created once after GL init

// --- Quit confirmation -------------------------------------------------------
static bool g_quit_requested = false;
enum class QuitState { None, Waiting, Confirmed };
static QuitState g_quit_state = QuitState::None;

// --- Panel width (shared between panel and resize handle) -------------------
float g_panel_w = 360.0f;   // also exposed as extern in app_state.h
static int  s_panel_nav_page    = 0;     // current page index in open panel (0-based)
static int   s_last_panel_doc    = 0;      // last doc shown in panel — used by edge tabs to reopen
static bool  g_panel_open_to_refs = false; // true = next panel open should land on References tab
static float s_toolbar_bottom     = 51.0f; // measured each frame by draw_toolbar_ui()
static Page* g_nav_focus = nullptr; // focused page for arrow navigation (when panel closed)

// --- Canvas text boxes -------------------------------------------------------
// w/h are the box's on-screen size in pixels (0 = auto-size to text, used by
// bare-click and legacy boxes). Text wraps to w; h is a floor that grows to fit.
std::vector<CanvasTextBox> g_text_boxes;
int  g_next_box_id    = 0;
int  g_selected_box   = -1;  // id, -1 = none
int  g_editing_box    = -1;  // id, -1 = not editing
static bool  g_text_tool      = false;
static float g_tbox_r = 0.82f, g_tbox_g = 0.06f, g_tbox_b = 0.06f;  // default red, like the pen
static float g_tbox_font_size = 16.0f;
static bool  g_tbox_zoom_scaled = false;  // template for new boxes: scale text with zoom
static int  g_hovered_box    = -1;  // id under cursor (set per-frame); -1 = none
static int  text_box_at(float sx, float sy);  // synchronous top-most text box under a point; -1 = none

// Page-group helpers (defined below; forward-declared for mouse/key/context-menu use).
static std::vector<Page*> group_pages(int gid);            // all pages with group_id == gid
static int  group_handle_at(float sx, float sy);           // group whose frame/label is under a point; 0 = none
static void create_group_from_selection();                 // group the selected pages (>=1)
static void ungroup_group(int gid);                        // dissolve a group (nondestructive)
static bool g_box_dragging    = false;
struct TextBoxDragState { int id; Vec2 initial_pos; };
static std::vector<TextBoxDragState> g_box_drag_states = {};
struct PageDragState { Page* page; Vec2 initial_pos; };
static std::vector<PageDragState>    g_page_drag_states = {};
static Vec2 g_box_drag_start_world = {};  // world position where drag began (grab point)

// Press-drag-release creation of a new text box (text tool active)
static bool g_tbox_creating     = false;
static Vec2 g_tbox_create_start = {};   // screen pos where the drag began
bool g_just_created      = false; // set on create; consumed by edit-session tracking

// Entity clipboard for Cmd+C / Cmd+V duplication of selected boxes
static CanvasTextBox g_clip_box   = {};
static bool          g_clip_valid = false;

// Undo session tracking — one record per editing session (text) and per
// selection session (style). Snapshots taken on begin, compared on end.
int   g_prev_editing_box  = -1;
static char  g_edit_text0[2048]  = "";
bool  g_edit_was_new      = false;
int   g_prev_selected_box = -1;
static float g_style_r0 = 0, g_style_g0 = 0, g_style_b0 = 0, g_style_fs0 = 0;

// --- Undo stack -------------------------------------------------------------
// UndoRecord is defined in undo.h. push_undo declared there, defined here.

static std::vector<UndoRecord> g_undo_stack;
static constexpr int           UNDO_LIMIT = 60;

// Resolve a stable page id to a live Page* (nullptr if id==0 or not found). Linear scan —
// fine at the ~100-page target scale; resolves happen only on discrete events (undo, drag
// grab), never per-frame per-page. This is what makes stored ids safe across reallocation:
// a stale id simply resolves to nullptr and is skipped.
Page* page_by_id(uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.id == id) return &p;
    return nullptr;
}

// The current page selection resolved to live Page* (skipping any stale ids). Use this to
// iterate the selection; use g_input.selection().count(page->id) for membership tests.
static std::vector<Page*> selected_pages() {
    std::vector<Page*> out;
    out.reserve(g_input.selection().size());
    for (uint64_t id : g_input.selection())
        if (Page* p = page_by_id(id)) out.push_back(p);
    return out;
}

void push_undo(UndoRecord r) {
    g_dirty = true;   // any undoable edit marks the project modified since last save
    g_undo_stack.push_back(std::move(r));
    if ((int)g_undo_stack.size() > UNDO_LIMIT)
        g_undo_stack.erase(g_undo_stack.begin());
}

void undo_last() {
    if (g_undo_stack.empty()) return;
    g_dirty = true;   // undoing is itself a change to the working state
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
                if (Page* p = page_by_id(pm.page_id)) p->world_pos = pm.old_pos;
            break;
        case UndoRecord::Type::PageResize:
            for (auto& pm : r.page_moves)
                if (Page* p = page_by_id(pm.page_id)) { p->world_w = pm.old_w; p->world_h = pm.old_h; }
            break;
        case UndoRecord::Type::DocScale:
            for (auto& pm : r.page_moves)
                if (Page* p = page_by_id(pm.page_id)) { p->world_pos = pm.old_pos;
                                                        p->world_w = pm.old_w; p->world_h = pm.old_h; }
            break;
        case UndoRecord::Type::PageRotate:
            for (auto& pr : r.page_rots)
                if (Page* p = page_by_id(pr.page_id)) {
                    p->rotation = pr.old_rot;
                    p->world_w  = pr.old_w;
                    p->world_h  = pr.old_h;
                }
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
            // The GPU textures were deleted when the doc was removed; zero the
            // stale handles so stream_lod() re-rasterizes on the next frame.
            for (auto& page : g_documents[idx].pages) {
                page.tex_thumb = 0;
                page.tex_low   = 0;
                page.tex_high  = 0;
            }
            g_input.set_documents(g_documents.empty() ? nullptr : &g_documents);
            break;
        }
        case UndoRecord::Type::CanvasStroke:
            if (!g_canvas_strokes.empty()) g_canvas_strokes.pop_back();
            break;
        case UndoRecord::Type::ErasedCanvasStroke: {
            int at = std::clamp(r.canvas_idx, 0, (int)g_canvas_strokes.size());
            g_canvas_strokes.insert(g_canvas_strokes.begin() + at, r.erased_stroke);
            break;
        }
        case UndoRecord::Type::Group:
            // Undo a group creation: revert members' group_id, then drop the group row.
            for (auto& gm : r.group_members) if (Page* p = page_by_id(gm.page_id)) p->group_id = gm.old_group;
            g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(),
                           [&](const PageGroup& g){ return g.id == r.group_row.id; }),
                           g_groups.end());
            break;
        case UndoRecord::Type::Ungroup:
            // Undo an ungroup: restore members' group_id and re-add the group row.
            for (auto& gm : r.group_members) if (Page* p = page_by_id(gm.page_id)) p->group_id = gm.old_group;
            if (r.group_row.id != 0) g_groups.push_back(r.group_row);
            break;
        case UndoRecord::Type::GroupJoin:
            // Undo a drag-to-add: revert members to their prior group (no row change —
            // the target group already existed and keeps its original members).
            for (auto& gm : r.group_members) if (Page* p = page_by_id(gm.page_id)) p->group_id = gm.old_group;
            break;
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
static bool s_panel_ann_active = false;  // true while annotating from the sidebar panel
static bool g_search_open    = false;
static std::atomic<bool> g_search_running{false};
static char g_search_buf[256] = {};
struct SearchResult {
    int         doc_idx;
    int         page_idx;
    std::string excerpt;
    std::string doc_name;
    std::vector<std::array<float,4>> hit_rects;  // normalized [0,1] quads from MuPDF
};
static std::vector<SearchResult> g_search_results;
static std::mutex                g_search_results_mutex;
static std::thread               g_search_thread;
static int g_highlighted_search_result = -1;
static std::string g_last_search_term;

// Transient canvas highlight for the currently selected search result.
struct SearchHighlight {
    int doc_idx  = -1;
    int page_idx = -1;
    std::vector<std::array<float,4>> rects;
    bool active() const { return doc_idx >= 0 && !rects.empty(); }
};
static SearchHighlight g_search_highlight;

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

void load_pdf(const std::string& path) {
    // Prevent duplicate loads
    for (const auto& d : g_documents)
        if (d.path == path) { printf("Already loaded: %s\n", path.c_str()); return; }

    auto loader = std::make_shared<PdfLoader>();
    if (!loader->valid()) {
        fprintf(stderr, "PdfLoader init failed for: %s\n", path.c_str());
        return;
    }
    loader->set_content_scale(g_content_scale);

    Document doc;
    if (!loader->load(path, doc, next_doc_origin())) {
        fprintf(stderr, "Failed to load PDF: %s\n", path.c_str());
        return;
    }

    int idx = static_cast<int>(g_documents.size());
    auto& c = DOC_PALETTE[idx % PALETTE_SIZE];
    doc.hue_r = c[0]; doc.hue_g = c[1]; doc.hue_b = c[2];

    // Assign a stable id to every freshly-loaded page. A project load calls this too, then
    // restores the saved ids over these (see load_project_from_path).
    for (auto& page : doc.pages) page.id = g_next_page_id++;

    // Register document immediately so it appears on canvas; pages show as
    // warm-white placeholders until background rasterization completes. A locked
    // (password-protected) doc can't be rasterized — skip it; it renders as a
    // distinct locked placeholder instead.
    if (!doc.locked) {
        printf("  Queuing %zu pages for background rasterization\xe2\x80\xa6\n", doc.pages.size());
        for (const auto& page : doc.pages)
            enqueue_rast(path, loader, page.page_index, LodTier::Thumb);
    }

    g_documents.push_back(std::move(doc));
    g_loaders.push_back(loader);
    g_input.set_documents(&g_documents);
}

void clear_documents() {
    rast_cancel_all();
    for (auto& doc : g_documents)
        for (auto& page : doc.pages)
            g_cache.evict(page);
    g_documents.clear();
    g_loaders.clear();
    g_cache.clear();
    g_input.set_documents(nullptr);
}

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

void remove_document(int doc_idx) {
    if (doc_idx < 0 || doc_idx >= (int)g_documents.size()) return;

    // Collect raw Page* addresses before the vector is erased so we can purge
    // every data structure that stores raw pointers into it.  Must happen first.
    std::unordered_set<const Page*> removing;
    for (const auto& page : g_documents[doc_idx].pages)
        removing.insert(&page);

    // Undo records now reference pages by stable id, so a removed page's records need no
    // scrubbing — they simply resolve to nullptr on undo (see page_by_id). This deletes the
    // old dangling-pointer hazard entirely.

    // Per-frame drag-snapshot globals hold raw Page*, but only for the lifetime of an active
    // drag (which can't span a document add/remove), so they never dangle. Cleared here as
    // belt-and-suspenders in case a removal ever races an in-flight drag.
    if (removing.count(g_drag_snap_page)) g_drag_snap_page = nullptr;
    g_multi_drag_snaps.erase(
        std::remove_if(g_multi_drag_snaps.begin(), g_multi_drag_snaps.end(),
            [&](const PageSnap& s){ return removing.count(s.page); }),
        g_multi_drag_snaps.end());

    const std::string& rpath = g_documents[doc_idx].path;
    rast_cancel_doc(rpath);
    for (auto& page : g_documents[doc_idx].pages) {
        g_cache.evict(page);
        evict_page_tiles(rpath, page.page_index);
    }
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

    // Carry over saved positions + annotations + group membership + stable id onto the
    // matching real pages, so page identity (selection, undo, groups) survives the relink.
    for (auto& fp : fresh.pages)
        for (auto& op : old.pages)
            if (op.page_index == fp.page_index) {
                fp.id        = op.id;         // preserve stable identity across the relink
                fp.world_pos = op.world_pos;
                fp.group_id  = op.group_id;   // keep the page in its group across a relink
                fp.annots    = std::move(op.annots);
                break;
            }
    // Any fresh page with no old counterpart (new PDF has more pages) gets a new id.
    for (auto& fp : fresh.pages) if (fp.id == 0) fp.id = g_next_page_id++;

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

#ifdef _WIN32
// Windows unhandled-exception filter: runs the minimum GL teardown so the GPU
// driver releases the OpenGL context before the process dies. Without this,
// some drivers hold the context open until the machine restarts, preventing
// a relaunch. EXCEPTION_CONTINUE_SEARCH lets WER / any attached debugger handle
// the crash normally after we've freed the context.
static LONG WINAPI scholion_seh_filter(EXCEPTION_POINTERS*) {
    if (g_window) { glfwDestroyWindow(g_window); g_window = nullptr; }
    glfwTerminate();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// --- GLFW callbacks ---------------------------------------------------------

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static int g_doc_z_counter = 0;

// The highlight whose research note is being edited in the panel (nullptr = none). A stable
// pointer (not a list index) so editing can never target the wrong reference. Safe for the
// edit session: highlights aren't added/removed while a note field has focus, and it is reset
// on project load/new.
static AnnotHighlight* s_editing_ref_hl = nullptr;
static char s_ref_note_buf[2048] = {};

static void mouse_button_callback(GLFWwindow* w, int button, int action, int mods) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    // A text box under the cursor owns this click, so no annotation (pen/highlight/note)
    // starts on the page beneath it. Synchronous hit-test replaces the old g_hovered_box
    // guard, which was set one frame late in draw_canvas_text_boxes — a same-frame
    // move-then-click could slip past it (e.g. dropping a stray Note flag under a box).
    {
        double hcx, hcy; glfwGetCursorPos(w, &hcx, &hcy);
        if (text_box_at((float)hcx, (float)hcy) >= 0) return;
    }

    // Annotation tool: intercept LMB press on a page before InputHandler sees it.
    // The page owns the click — InputHandler does not receive it, so no drag/select starts.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS
            && g_annot_tool != AnnotTool::None) {
        double cx, cy;
        glfwGetCursorPos(w, &cx, &cy);
        int doc_idx = -1;
        Page* page = hit_test_page((float)cx, (float)cy, &doc_idx);
        if (page) {
            Vec2 norm = screen_to_page_norm(*page, (float)cx, (float)cy);
            if (g_annot_tool == AnnotTool::Note) {
                int snap = g_next_note_idx;
                page->annots.notes.push_back({note_label(g_next_note_idx++)});
                UndoRecord r;
                r.type            = UndoRecord::Type::Note;
                r.doc_idx         = doc_idx;
                r.page_idx        = page->page_index;
                r.note_idx_before = snap;
                push_undo(r);
            } else {
                g_ann_drawing  = true;
                g_ann_doc_idx  = doc_idx;
                g_ann_page_idx = page->page_index;
                // Pen and the freehand highlighter capture a point path; the box highlighter
                // (and eraser) just track the start/current corner via g_ann_hl_start/cur_norm.
                if (g_annot_tool == AnnotTool::Pen ||
                    (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode)) {
                    g_ann_cur_stroke = {};
                    if (g_annot_tool == AnnotTool::Pen) {
                        g_ann_cur_stroke.r = g_pen_r; g_ann_cur_stroke.g = g_pen_g; g_ann_cur_stroke.b = g_pen_b;
                    } else {
                        g_ann_cur_stroke.r = g_hl_r; g_ann_cur_stroke.g = g_hl_g; g_ann_cur_stroke.b = g_hl_b;
                        g_ann_cur_stroke.width = MARKER_HALF_W; g_ann_cur_stroke.alpha = MARKER_ALPHA;
                    }
                    g_ann_cur_stroke.pts.push_back(norm);
                }
                g_ann_hl_start = norm;   // box highlighter / eraser radius origin
                g_ann_cur_norm = norm;
            }
            return; // page owns this click — don't pass to InputHandler
        } else if (g_annot_tool == AnnotTool::Pen) {
            // Pen started on empty canvas → a world-locked canvas stroke (drawn in front).
            g_ann_drawing = true; g_ann_canvas = true;
            g_ann_doc_idx = -1; g_ann_page_idx = -1;
            g_ann_cur_stroke = {};
            g_ann_cur_stroke.r = g_pen_r; g_ann_cur_stroke.g = g_pen_g; g_ann_cur_stroke.b = g_pen_b;
            g_ann_cur_stroke.pts.push_back(g_canvas.screen_to_world({(float)cx, (float)cy}));
            return;
        }
    }

    bool was_box_sel = g_input.box_selecting();
    bool tools_active = g_text_tool || g_annot_tool != AnnotTool::None;

    // Group frame handle: a left-press on a group's frame outline or label grabs the
    // whole group and moves it as a unit. Opt-in and gated on no tool active / not
    // panning, so dragging a page interior or a document (Shift+drag) is unchanged.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS
            && !tools_active && !g_input.is_space_held()) {
        double gcx, gcy; glfwGetCursorPos(w, &gcx, &gcy);
        int gid = group_handle_at((float)gcx, (float)gcy);
        if (gid != 0) {
            std::vector<Page*> pages = group_pages(gid);
            if (!pages.empty()) {
                g_input.begin_group_drag(pages, {(float)gcx, (float)gcy});
                return;   // group handle owns this press
            }
        }
    }

    g_input.on_mouse_button(w, button, action, mods, tools_active);

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
            g_nav_focus = page_by_id(*sel.begin());
        }
    }
}

static void cursor_pos_callback(GLFWwindow* w, double x, double y) {
    if (ImGui::GetIO().WantCaptureMouse) { g_input.clear_hover(); return; }
    g_input.on_cursor_move(w, x, y);
    // Canvas (world-space) pen stroke — accumulate world points.
    if (g_ann_drawing && g_ann_canvas && g_annot_tool == AnnotTool::Pen) {
        bool ortho = (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS) ||
                     (glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        stroke_add_point_world(g_canvas.screen_to_world({(float)x, (float)y}), ortho);
    }
    // Accumulate pen / freehand-highlighter-swipe points at mouse-move rate (smoother path).
    // Box-mode highlight tracks a rectangle instead (via g_ann_cur_norm), so it's excluded.
    if (g_ann_drawing && !s_panel_ann_active
            && (g_annot_tool == AnnotTool::Pen ||
                (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode))
            && g_ann_doc_idx >= 0 && g_ann_doc_idx < (int)g_documents.size()
            && g_ann_page_idx >= 0) {
        bool ortho = (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS) ||
                     (glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        const Document& doc = g_documents[g_ann_doc_idx];
        for (const auto& pg : doc.pages) {
            if (pg.page_index == g_ann_page_idx) {
                stroke_add_point(pg, screen_to_page_norm(pg, (float)x, (float)y), ortho);
                break;
            }
        }
    }
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
        // Cmd+0 / Ctrl+0: zoom-to-fit regardless of active tool or text editing state.
        if (key == GLFW_KEY_0 && (super || ctrl)) { zoom_to_fit(); return; }

        // Tool keys: suppressed while any ImGui widget has keyboard focus (text input, etc.)
        bool cmd = super || ctrl;
        if (!cmd && !g_search_open && g_editing_box < 0
                && !ImGui::GetIO().WantCaptureKeyboard) {
            if (key == GLFW_KEY_P) {
                bool was_pen = (g_annot_tool == AnnotTool::Pen);
                g_annot_tool = was_pen ? AnnotTool::None : AnnotTool::Pen;
                g_ann_drawing = false;
                if (!was_pen) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_H) {
                bool was_hl = (g_annot_tool == AnnotTool::Highlight);
                g_annot_tool = was_hl ? AnnotTool::None : AnnotTool::Highlight;
                g_ann_drawing = false;
                if (!was_hl) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_F && !super && !ctrl) {
                bool was_note = (g_annot_tool == AnnotTool::Note);
                g_annot_tool = was_note ? AnnotTool::None : AnnotTool::Note;
                g_ann_drawing = false;
                if (!was_note) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_E) {
                bool was_eraser = (g_annot_tool == AnnotTool::Eraser);
                g_annot_tool = was_eraser ? AnnotTool::None : AnnotTool::Eraser;
                g_ann_drawing = false;
                if (!was_eraser) { g_text_tool = false; g_editing_box = -1; }
                return;
            }
            if (key == GLFW_KEY_T) {
                bool was_text = g_text_tool;
                g_text_tool   = !g_text_tool;
                g_editing_box = -1;
                if (!was_text) { g_annot_tool = AnnotTool::None; g_ann_drawing = false; }
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

    // While a ref-note is being edited in the panel, suppress all shortcuts and
    // canvas input — only Escape is allowed (to confirm and close the note).
    if (s_editing_ref_hl) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            s_editing_ref_hl->note = s_ref_note_buf;   // empty buffer clears the note
            s_editing_ref_hl = nullptr;
        }
        return;
    }

    // Space key must reach on_key even when the panel has keyboard focus so that
    // the press→release tap sequence that toggles the panel is always detected.
    // WantCaptureKeyboard is true whenever any ImGui window is active — but we
    // must NOT route space to on_key when the user is actually typing (text box,
    // search field, ref-note editor). WantTextInput is specifically true only when
    // a text input widget has keyboard focus and wants character input.
    if (ImGui::GetIO().WantCaptureKeyboard) {
        if (key == GLFW_KEY_SPACE && g_editing_box < 0 && !ImGui::GetIO().WantTextInput)
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
                const_cast<std::unordered_set<uint64_t>&>(g_input.selection()).insert(next->id);
                g_nav_focus = next;
            }
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_LEFT) {
            Page* next = next_page_in_order(g_nav_focus, -1);
            if (next) {
                g_input.clear_selection();
                const_cast<std::unordered_set<uint64_t>&>(g_input.selection()).insert(next->id);
                g_nav_focus = next;
            }
        }
    }

    if (action != GLFW_PRESS) return;

    bool cmd = super || ctrl;

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

    // Cmd/Ctrl+G groups the selected pages; Cmd/Ctrl+Shift+G ungroups them.
    if (cmd && key == GLFW_KEY_G) {
        bool shift = (mods & GLFW_MOD_SHIFT) != 0;
        if (shift) {
            std::unordered_set<int> gids;
            for (Page* p : selected_pages()) if (p->group_id) gids.insert(p->group_id);
            for (int gid : gids) ungroup_group(gid);
        } else {
            create_group_from_selection();
        }
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
    // Background list: the cursor hint sits above all canvas content but below ImGui
    // windows (drawn last among the canvas layers). See the layering note in main().
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
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
static constexpr float TBOX_PAD      = 4.0f;
static constexpr float TBOX_MIN_H    = 28.0f;
static constexpr float TBOX_DEFAULT_W = 200.0f; // bare-click box width
static constexpr float TBOX_MIN_DRAG  = 8.0f;   // px; smaller drags count as a click

// Screen-space layout of a text box, shared by draw_canvas_text_boxes (render + hover)
// and text_box_at (the synchronous hit-test in mouse_button_callback) so the two can
// never diverge. `scale` is 1.0 today; Part C multiplies it by the canvas zoom for boxes
// flagged zoom_scaled, so glyph size and box dimensions grow with the page.
struct TextBoxLayout { ImVec2 tl, br; float box_w, box_h, wrap, font_px, pad; };
static TextBoxLayout text_box_layout(const CanvasTextBox& box) {
    ImFont* font = ImGui::GetFont();
    Vec2 sp = g_canvas.world_to_screen(box.world_pos);
    // Fixed boxes render at a constant on-screen size; zoom_scaled boxes grow with the
    // canvas zoom so their text stays proportional to the page they annotate. Stored
    // w/h/font_size are always canonical at 100% zoom.
    float scale   = box.zoom_scaled ? g_canvas.get_zoom() : 1.0f;
    float pad     = TBOX_PAD * scale;
    float font_px = box.font_size * scale;
    const char* content = box.text[0] ? box.text : " ";
    float box_w, wrap, box_h;
    if (box.w > 0.0f) {
        box_w = box.w * scale;
        wrap  = box_w - 2.0f * pad;
        ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, wrap, content);
        box_h = std::max(box.h * scale, ts.y + 2.0f * pad);
    } else {
        float natural_w = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, content).x + 2.0f * pad;
        box_w = std::clamp(natural_w, TBOX_MIN_W * scale, TBOX_MAX_W * scale);
        wrap  = box_w - 2.0f * pad;
        ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, wrap, content);
        box_h = std::max(TBOX_MIN_H * scale, ts.y + 2.0f * pad);
    }
    return { {sp.x, sp.y}, {sp.x + box_w, sp.y + box_h}, box_w, box_h, wrap, font_px, pad };
}

// Top-most text box whose screen rect contains (sx, sy), or -1. Iterates in draw order
// so the last (visually top) box wins, matching draw_canvas_text_boxes' hover logic.
static int text_box_at(float sx, float sy) {
    if (g_settings_open || g_search_open) return -1;   // boxes not interactive under overlays
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    if (sx >= canvas_right) return -1;                 // clicks inside the panel excluded
    int found = -1;
    for (const auto& box : g_text_boxes) {
        if (g_editing_box == box.id) continue;         // editing box is an ImGui window
        TextBoxLayout L = text_box_layout(box);
        if (sx >= L.tl.x && sx <= L.br.x && sy >= L.tl.y && sy <= L.br.y)
            found = box.id;
    }
    return found;
}

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

// --- Page groups ------------------------------------------------------------
// A group is an ad-hoc, nondestructive cluster of pages (possibly from several
// documents) that can be moved together by grabbing its frame. Membership lives
// on Page::group_id; g_groups holds each group's color/label. Movement reuses the
// existing multi-drag machinery (begin_group_drag), so nothing about page or
// document dragging changes — the group is opt-in via its frame handle.

static constexpr float GROUP_FRAME_PAD   = 10.0f; // innermost boundary offset from member pages
static constexpr float GROUP_RING_GAP    = 4.0f;  // spacing between per-document boundary rings
static constexpr float GROUP_RING_ROUND  = 20.0f; // corner radius — bubble-like, node-editor feel
static constexpr float GROUP_RING_THICK   = 1.5f; // boundary line thickness
static constexpr int   GROUP_RING_ALPHA   = 165;  // dim, like the thread wires
static constexpr float GROUP_HANDLE_BAND = 11.0f; // grab thickness around the boundary
static constexpr float GROUP_LABEL_H     = 20.0f; // label tab height (screen px)
static constexpr float GROUP_LABEL_INSET = 14.0f; // shift the label in from the rounded corner
static constexpr double GROUP_DRAGADD_DWELL = 0.8; // seconds to hover a group before it swallows pages

// Group name being renamed inline (0 = none) + its edit buffer.
static int  g_editing_group = 0;
static char g_group_name_buf[256] = "";

// Drag-to-add-into-group state (a page held over a group for GROUP_DRAGADD_DWELL joins it).
static std::vector<Page*> g_dragadd_pages;         // pages currently being dragged
static int    g_dragadd_target = 0;                // group under the cursor that could swallow them
static double g_dragadd_since  = 0.0;              // time the cursor entered g_dragadd_target
static bool   g_dragadd_ready  = false;            // dwell satisfied — release will join
static bool   g_was_dragging_for_add = false;      // previous-frame drag state (edge detect on release)

// Brief fading outline on a page as it leaves a group — a subtle cue that its tie released.
// Keyed by a snapshot of the page's world rect + color (not a pointer), so it can't dangle.
static constexpr double GROUP_REMOVE_FLASH_DUR = 0.75;  // seconds
struct GroupRemoveFlash { float x0, y0, x1, y1; float r, g, b; double t0; };
static std::vector<GroupRemoveFlash> g_group_remove_flashes;

static std::vector<Page*> group_pages(int gid) {
    std::vector<Page*> out;
    if (gid == 0) return out;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.group_id == gid) out.push_back(&p);
    return out;
}

static const PageGroup* find_group(int gid) {
    for (const auto& g : g_groups) if (g.id == gid) return &g;
    return nullptr;
}

// Distinct documents contributing pages to a group, in g_documents order. The
// count drives how many concentric boundary rings are drawn (one per document,
// in that document's hue); the order fixes which ring sits where.
static std::vector<const Document*> group_contrib_docs(int gid) {
    std::vector<const Document*> out;
    if (gid == 0) return out;
    for (const auto& doc : g_documents) {
        bool has = false;
        for (const auto& p : doc.pages) if (p.group_id == gid) { has = true; break; }
        if (has) out.push_back(&doc);
    }
    return out;
}

// Tight screen-space AABB of a group's member pages (no padding). false if empty.
static bool group_base_screen(int gid, ImVec2& tl, ImVec2& br) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool any = false;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.group_id == gid) {
                any = true;
                x0 = std::min(x0, p.world_pos.x);
                y0 = std::min(y0, p.world_pos.y);
                x1 = std::max(x1, p.world_pos.x + p.world_w);
                y1 = std::max(y1, p.world_pos.y + p.world_h);
            }
    if (!any) return false;
    Vec2 s_tl = g_canvas.world_to_screen({x0, y0});
    Vec2 s_br = g_canvas.world_to_screen({x1, y1});
    tl = {s_tl.x, s_tl.y};
    br = {s_br.x, s_br.y};
    return true;
}

// Outermost boundary ring rect (screen space) — base bounds grown by the full ring
// stack. Used as the grab region and the anchor for the label tab.
static bool group_outer_frame(int gid, ImVec2& tl, ImVec2& br) {
    ImVec2 b_tl, b_br;
    if (!group_base_screen(gid, b_tl, b_br)) return false;
    int rings = std::max(1, (int)group_contrib_docs(gid).size());
    float off = GROUP_FRAME_PAD + (rings - 1) * GROUP_RING_GAP;
    tl = {b_tl.x - off, b_tl.y - off};
    br = {b_br.x + off, b_br.y + off};
    return true;
}

// Label-tab rectangle, derived identically for draw and hit-test.
static ImVec4 group_label_screen(ImVec2 frame_tl, const char* label) {
    float font_px = ImGui::GetFontSize();
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(font_px, FLT_MAX, 0.0f, label);
    float w = std::max(ts.x + 16.0f, 44.0f);
    // Inset from the corner so the tab clears the boundary's rounded radius.
    float x0 = frame_tl.x + GROUP_LABEL_INSET;
    return { x0, frame_tl.y - GROUP_LABEL_H, x0 + w, frame_tl.y };
}

// Topmost group whose boundary or label tab is under (sx, sy); 0 = none.
static int group_handle_at(float sx, float sy) {
    if (g_settings_open || g_search_open) return 0;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    if (sx >= canvas_right) return 0;
    int found = 0;
    for (const auto& grp : g_groups) {
        ImVec2 tl, br;
        if (!group_outer_frame(grp.id, tl, br)) continue;
        const char* label = grp.name.empty() ? "Group" : grp.name.c_str();
        ImVec4 lr = group_label_screen(tl, label);
        bool in_label = sx >= lr.x && sx <= lr.z && sy >= lr.y && sy <= lr.w;
        // Grab band straddling the outermost boundary line.
        float bnd = GROUP_HANDLE_BAND;
        bool in_outer = sx >= tl.x - bnd && sx <= br.x + bnd && sy >= tl.y - bnd && sy <= br.y + bnd;
        bool in_inner = sx >  tl.x + bnd && sx <  br.x - bnd && sy >  tl.y + bnd && sy <  br.y - bnd;
        if (in_label || (in_outer && !in_inner)) found = grp.id;
    }
    return found;
}

// Topmost group whose outer frame *interior* contains (sx, sy); 0 = none. Used by the
// drag-to-add dwell test (the whole enclosed area is a drop target, not just the border).
static int group_area_at(float sx, float sy) {
    if (g_settings_open || g_search_open) return 0;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    if (sx >= canvas_right) return 0;
    int found = 0;
    for (const auto& grp : g_groups) {
        ImVec2 tl, br;
        if (!group_outer_frame(grp.id, tl, br)) continue;
        if (sx >= tl.x && sx <= br.x && sy >= tl.y && sy <= br.y) found = grp.id;
    }
    return found;
}

static void draw_page_groups() {
    if (g_settings_open || g_search_open) return;

    // Drop a stale rename target if its group is gone.
    if (g_editing_group != 0 && !find_group(g_editing_group)) g_editing_group = 0;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (auto& grp : g_groups) {
        ImVec2 base_tl, base_br;
        if (!group_base_screen(grp.id, base_tl, base_br)) continue;  // empty — nothing to draw

        std::vector<const Document*> docs = group_contrib_docs(grp.id);
        int rings = std::max(0, (int)docs.size() - 1);
        float outer_off = GROUP_FRAME_PAD + rings * GROUP_RING_GAP;

        // Drag-to-add feedback: a soft interior wash while a dragged page dwells over
        // this group, brightening once the dwell is satisfied (release will join).
        if (grp.id == g_dragadd_target) {
            int a = g_dragadd_ready ? 60 : 26;
            dl->AddRectFilled({base_tl.x - outer_off, base_tl.y - outer_off},
                              {base_br.x + outer_off, base_br.y + outer_off},
                              IM_COL32(255, 255, 255, a), GROUP_RING_ROUND);
        }

        // One solid, dim, rounded boundary ring per contributing document, in that
        // document's hue. Multiple documents => concentric multi-colored rings a few
        // px apart — the node-editor thread aesthetic applied to a page cluster.
        for (int i = 0; i < (int)docs.size(); ++i) {
            float off = GROUP_FRAME_PAD + i * GROUP_RING_GAP;
            ImU32 col = IM_COL32((int)(docs[i]->hue_r * 255),
                                 (int)(docs[i]->hue_g * 255),
                                 (int)(docs[i]->hue_b * 255), GROUP_RING_ALPHA);
            dl->AddRect({base_tl.x - off, base_tl.y - off},
                        {base_br.x + off, base_br.y + off},
                        col, GROUP_RING_ROUND, ImDrawFlags_RoundCornersAll, GROUP_RING_THICK);
        }

        ImVec2 outer_tl = {base_tl.x - outer_off, base_tl.y - outer_off};

        const char* label = grp.name.empty() ? "Group" : grp.name.c_str();
        ImVec4 lr = group_label_screen(outer_tl, label);

        // Label tab tinted with the group's lead document hue (darkened for legible light text),
        // so the tab reads as part of the group's color theme.
        ImU32 pill_col;
        if (!docs.empty())
            pill_col = IM_COL32((int)(docs[0]->hue_r * 0.45f * 255),
                                (int)(docs[0]->hue_g * 0.45f * 255),
                                (int)(docs[0]->hue_b * 0.45f * 255), 230);
        else
            pill_col = IM_COL32(38, 35, 32, 215);

        if (g_editing_group == grp.id) {
            // Inline rename: a small borderless InputText at the label tab.
            ImGui::SetNextWindowPos({lr.x, lr.y});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2.0f, 2.0f});
            ImGui::Begin("##grp_rename", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetNextItemWidth(std::max(120.0f, lr.z - lr.x));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            // Enter confirms (no line breaks needed); Escape reverts; clicking out commits.
            ImGui::InputText("##grpname", g_group_name_buf, sizeof(g_group_name_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemDeactivated()) {
                if (ImGui::IsItemDeactivatedAfterEdit()) { grp.name = g_group_name_buf; g_dirty = true; }
                g_editing_group = 0;
            }
            ImGui::End();
            ImGui::PopStyleVar();
        } else {
            dl->AddRectFilled({lr.x, lr.y}, {lr.z, lr.w}, pill_col, 5.0f, ImDrawFlags_RoundCornersTop);
            dl->AddText({lr.x + 8.0f, lr.y + (GROUP_LABEL_H - ImGui::GetFontSize()) * 0.5f},
                        IM_COL32(235, 231, 225, 255), label);
            // Double-click the tab to rename it.
            if (!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImVec2 m = ImGui::GetMousePos();
                if (m.x >= lr.x && m.x <= lr.z && m.y >= lr.y && m.y <= lr.w) {
                    g_editing_group = grp.id;
                    strncpy(g_group_name_buf, grp.name.c_str(), sizeof(g_group_name_buf) - 1);
                    g_group_name_buf[sizeof(g_group_name_buf) - 1] = '\0';
                }
            }
        }
    }
}

// Group the currently selected pages into a fresh group (flat: each page joins
// this group, leaving any previous one). Nondestructive and undoable.
static void create_group_from_selection() {
    const auto& sel = g_input.selection();
    if (sel.empty()) return;
    int gid = g_next_group_id++;
    PageGroup grp;
    grp.id = gid;
    const float* cv = GROUP_PALETTE[(gid - 1) % GROUP_PALETTE_SIZE];
    grp.col_r = cv[0]; grp.col_g = cv[1]; grp.col_b = cv[2];

    UndoRecord r; r.type = UndoRecord::Type::Group; r.group_row = grp;
    for (Page* p : selected_pages()) {
        r.group_members.push_back({p->id, p->group_id});
        p->group_id = gid;
    }
    g_groups.push_back(grp);
    push_undo(r);
}

// Dissolve a group: members revert to ungrouped, the row is removed. Undoable.
static void ungroup_group(int gid) {
    if (gid == 0) return;
    if (g_editing_group == gid) g_editing_group = 0;
    const PageGroup* g = find_group(gid);
    UndoRecord r; r.type = UndoRecord::Type::Ungroup;
    if (g) r.group_row = *g;
    bool any = false;
    for (auto& doc : g_documents)
        for (auto& p : doc.pages)
            if (p.group_id == gid) {
                r.group_members.push_back({p.id, gid});
                p.group_id = 0;
                any = true;
            }
    g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(),
                   [&](const PageGroup& x){ return x.id == gid; }), g_groups.end());
    if (any) push_undo(r);
}

// Add pages to an existing group (drag-to-add). Flat rule: a page joins this group,
// leaving any previous one (old_group recorded for undo). The target row already
// exists, so undo (GroupJoin) only reverts membership — it never removes the row.
static void add_pages_to_group(int gid, const std::vector<Page*>& pages) {
    if (gid == 0 || !find_group(gid)) return;
    UndoRecord r; r.type = UndoRecord::Type::GroupJoin;
    bool any = false;
    for (Page* p : pages) {
        if (!p || p->group_id == gid) continue;
        r.group_members.push_back({p->id, p->group_id});
        p->group_id = gid;
        any = true;
    }
    if (any) push_undo(r);
}

// Per-frame: while pages are being dragged, arm a target group once the cursor dwells
// over it long enough; on release, the target swallows the dragged pages. Add-only —
// dropping outside any group never removes a page (Ungroup is the removal path).
static void update_group_drag_add() {
    bool dragging = (g_input.dragged_page() != nullptr) || g_input.is_multi_dragging();
    if (dragging) {
        g_dragadd_pages.clear();
        if (g_input.is_multi_dragging()) {
            for (Page* p : selected_pages()) g_dragadd_pages.push_back(p);
        } else if (const Page* dp = g_input.dragged_page()) {
            g_dragadd_pages.push_back(const_cast<Page*>(dp));
        }
        ImVec2 m = ImGui::GetMousePos();
        int cand = group_area_at(m.x, m.y);
        if (cand != 0) {
            // Only a candidate if at least one dragged page isn't already in it.
            bool any_new = false;
            for (Page* p : g_dragadd_pages) if (p->group_id != cand) { any_new = true; break; }
            if (!any_new) cand = 0;
        }
        if (cand != g_dragadd_target) {
            g_dragadd_target = cand;
            g_dragadd_since  = ImGui::GetTime();
            g_dragadd_ready  = false;
        } else if (cand != 0 && !g_dragadd_ready &&
                   ImGui::GetTime() - g_dragadd_since >= GROUP_DRAGADD_DWELL) {
            g_dragadd_ready = true;
        }
    } else {
        // Drag ended this frame — commit if a target was armed and satisfied.
        if (g_was_dragging_for_add && g_dragadd_ready && g_dragadd_target != 0 && !g_dragadd_pages.empty())
            add_pages_to_group(g_dragadd_target, g_dragadd_pages);
        g_dragadd_pages.clear();
        g_dragadd_target = 0;
        g_dragadd_ready  = false;
    }
    g_was_dragging_for_add = dragging;
}

// Remove a single page from its group (nondestructive, undoable) — the deliberate,
// menu-driven counterpart to drag-to-add, so a stray drag can't change membership. The
// group's boundary recomputes to omit the page automatically; a brief fading outline in
// the page's document color flags the change.
static void remove_page_from_group(Page* p) {
    if (!p || p->group_id == 0) return;
    UndoRecord r; r.type = UndoRecord::Type::GroupJoin;   // GroupJoin = revert membership only
    r.group_members.push_back({p->id, p->group_id});

    float fr = 0.6f, fg = 0.6f, fb = 0.6f;                // flash color = owning document hue
    for (const auto& doc : g_documents) {
        bool has = false;
        for (const auto& pg : doc.pages) if (&pg == p) { has = true; break; }
        if (has) { fr = doc.hue_r; fg = doc.hue_g; fb = doc.hue_b; break; }
    }
    g_group_remove_flashes.push_back({ p->world_pos.x, p->world_pos.y,
                                       p->world_pos.x + p->world_w, p->world_pos.y + p->world_h,
                                       fr, fg, fb, ImGui::GetTime() });
    p->group_id = 0;
    push_undo(r);
}

// Draw + age the "left the group" page flashes. Fades a rounded outline (and a faint fill)
// out over GROUP_REMOVE_FLASH_DUR. Runs on the canvas layer (below the UI).
static void draw_group_remove_flashes() {
    if (g_group_remove_flashes.empty()) return;
    double now = ImGui::GetTime();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const auto& f : g_group_remove_flashes) {
        float t = (float)((now - f.t0) / GROUP_REMOVE_FLASH_DUR);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) continue;
        float ease = 1.0f - t;                 // fade out
        Vec2 tl = g_canvas.world_to_screen({f.x0, f.y0});
        Vec2 br = g_canvas.world_to_screen({f.x1, f.y1});
        ImU32 line = IM_COL32((int)(f.r * 255), (int)(f.g * 255), (int)(f.b * 255),
                              (int)(235.0f * ease));
        ImU32 fill = IM_COL32((int)(f.r * 255), (int)(f.g * 255), (int)(f.b * 255),
                              (int)(45.0f * ease));
        dl->AddRectFilled({tl.x, tl.y}, {br.x, br.y}, fill, GROUP_RING_ROUND);
        dl->AddRect({tl.x, tl.y}, {br.x, br.y}, line, GROUP_RING_ROUND,
                    ImDrawFlags_RoundCornersAll, 2.0f);
    }
    g_group_remove_flashes.erase(
        std::remove_if(g_group_remove_flashes.begin(), g_group_remove_flashes.end(),
            [&](const GroupRemoveFlash& f){ return (now - f.t0) >= GROUP_REMOVE_FLASH_DUR; }),
        g_group_remove_flashes.end());
}

// Label locked (password-protected) documents' placeholder pages so the distinct
// indigo rect reads clearly instead of looking like a blank page.
static void draw_locked_doc_labels() {
    if (g_settings_open || g_search_open) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const char* txt = "Password-protected PDF";
    for (const auto& doc : g_documents) {
        if (!doc.locked) continue;
        for (const auto& page : doc.pages) {
            Vec2 c = g_canvas.world_to_screen({page.world_pos.x + page.world_w * 0.5f,
                                               page.world_pos.y + page.world_h * 0.5f});
            ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f},
                        IM_COL32(205, 208, 230, 235), txt);
        }
    }
}

static void draw_canvas_text_boxes() {
    // BackgroundDrawList: text boxes live on the canvas layer (above pages/marks but
    // BELOW every ImGui window), so context menus, tooltips, and dialogs correctly draw
    // on top of them. The panel-clip below keeps them from bleeding through a translucent
    // sidebar. These early-outs skip work under the full-screen Settings/Search overlays.
    if (g_settings_open) return;
    if (g_search_open) return;

    ImDrawList* dl    = ImGui::GetBackgroundDrawList();
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
                        if (sel_pages.count(p.id)) {
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
                        r.page_moves.push_back({ps.page->id, ps.initial_pos});
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
    // g_panel_w pixels; boxes that overlap it are clipped at the panel's left edge.
    float canvas_right = g_input.panel_open() ? (vp.x - g_panel_w) : vp.x;
    dl->PushClipRect({0.0f, 0.0f}, {canvas_right, vp.y}, true);

    for (auto& box : g_text_boxes) {
        if (g_editing_box == box.id) continue;  // editing box shown as ImGui window below

        TextBoxLayout L = text_box_layout(box);
        // Off-screen cull using the box's actual on-screen extent (correct for zoom-scaled
        // boxes, whose footprint can be much larger than the fixed-size constants).
        if (L.br.x < 0 || L.tl.x > vp.x || L.br.y < 0 || L.tl.y > vp.y) continue;
        ImVec2 tl = L.tl, br = L.br;
        float  wrap = L.wrap;

        bool hit = !io.WantCaptureMouse
                && mouse.x >= tl.x && mouse.x <= br.x
                && mouse.y >= tl.y && mouse.y <= br.y
                && mouse.x < canvas_right;  // exclude clicks inside the panel

        if (hit) new_hovered = box.id;

        // Draw text (or placeholder). Font size + padding come from the shared layout so
        // a zoom_scaled box (Part C) renders larger; today L.font_px == box.font_size.
        if (box.text[0]) {
            dl->AddText(font, L.font_px, {tl.x + L.pad, tl.y + L.pad},
                        IM_COL32((int)(box.r*255), (int)(box.g*255), (int)(box.b*255), 220),
                        box.text, nullptr, wrap);
        } else {
            dl->AddText(font, L.font_px, {tl.x + L.pad, tl.y + L.pad},
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
                // Entering box editing disarms any annotation tool (symmetric with
                // the tool-activation paths, which already clear g_editing_box). Also
                // cancels any in-progress stroke so a stray pen line can't be committed.
                g_annot_tool     = AnnotTool::None;
                g_ann_drawing    = false;
                g_selected_box   = box.id;
                g_editing_box    = box.id;
                g_tbox_r         = box.r; g_tbox_g = box.g; g_tbox_b = box.b;
                g_tbox_font_size = box.font_size;
                g_tbox_zoom_scaled = box.zoom_scaled;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // A click that lands on a text box (select/drag) must not also leave
                // an annotation stroke running underneath it. mouse_button_callback's
                // "text box owns this click" guard uses g_hovered_box, which lags one
                // frame — a same-frame move-then-click onto a box can slip past it and
                // start a pen/highlight stroke on the page below. Cancel it here so a
                // stray line can't be drawn while the box is selected or dragged.
                g_ann_drawing = false;
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
                g_tbox_zoom_scaled = box.zoom_scaled;

                // Start multi-box drag — suppressed while any tool is active so boxes/pages
                // don't move in tool mode (selection above still applies).
                if (!g_text_tool && g_annot_tool == AnnotTool::None) {
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
                    for (Page* p : selected_pages())
                        g_page_drag_states.push_back({p, p->world_pos});
                    g_box_dragging = true;
                }
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
            nb.zoom_scaled = g_tbox_zoom_scaled;
            // Drag-sized boxes: store w/h canonical at 100% zoom so a scaled box drawn
            // while zoomed in doesn't balloon. font_size and the default width are already
            // canonical sizes.
            float inv = nb.zoom_scaled ? (1.0f / g_canvas.get_zoom()) : 1.0f;
            if (dw >= TBOX_MIN_DRAG && dh >= TBOX_MIN_DRAG) {
                nb.world_pos = g_canvas.screen_to_world({a.x, a.y});
                nb.w = dw * inv; nb.h = dh * inv;
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
            // Scale the editor to match how the box renders, so it doesn't visibly jump
            // between edit and display when the box is zoom_scaled.
            float escale = eb->zoom_scaled ? g_canvas.get_zoom() : 1.0f;
            Vec2 sp = g_canvas.world_to_screen(eb->world_pos);
            float ew = ((eb->w > 0.0f) ? eb->w : TBOX_W) * escale;
            float eh = std::max(((eb->h > 0.0f) ? eb->h : 130.0f) * escale, 60.0f);
            ImGui::SetNextWindowPos({sp.x, sp.y}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({ew, eh}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {TBOX_PAD * escale, TBOX_PAD * escale});
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.14f, 0.92f));

            char wid[32]; snprintf(wid, sizeof(wid), "##tbedit%d", g_editing_box);
            ImGui::Begin(wid, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetWindowFontScale(escale);  // scale the edited glyphs to match display
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
    {
        // Open the file's parent directory in the system file manager via xdg-open.
        std::string parent = std::filesystem::path(path).parent_path().string();
        pid_t pid = fork();
        if (pid == 0) {
            execl("/usr/bin/xdg-open", "xdg-open", parent.c_str(), (char*)nullptr);
            _exit(127);
        }
    }
#endif
}

// --- Context menu -----------------------------------------------------------

static void apply_page_rotation(Page* page, int delta_cw) {
    int d = ((delta_cw % 360) + 360) % 360;   // normalize to [0,360)
    if (d == 0) return;                        // no-op (e.g. reset on an unrotated page)
    UndoRecord r;
    r.type = UndoRecord::Type::PageRotate;
    r.page_rots.push_back({page->id, page->rotation, page->world_w, page->world_h});
    push_undo(r);
    page->rotation = (page->rotation + d) % 360;
    // Swap the page footprint only for odd 90° turns; a 180° (or 180° reset) keeps w/h.
    // The old unconditional swap was wrong for those cases.
    if ((d / 90) % 2 != 0) std::swap(page->world_w, page->world_h);
}

// Scale a whole document so its pages match the standard page size on the canvas — fixes
// documents/images that import wildly under- or over-sized. Target = median page height of
// the OTHER (non-missing) documents; falls back to Letter (792 pt) if this is the only one.
// Scales uniformly about the document's visual centroid; annotations (page-normalized) and
// threads (page geometry) follow automatically.
static void normalize_document_size(Document& doc) {
    if (doc.pages.empty()) return;
    auto median_h = [](std::vector<float> hs) -> float {
        if (hs.empty()) return 0.0f;
        std::sort(hs.begin(), hs.end());
        return hs[hs.size() / 2];
    };
    std::vector<float> mine;
    for (const auto& p : doc.pages) if (p.world_h > 0.0f) mine.push_back(p.world_h);
    float cur_h = median_h(mine);
    if (cur_h <= 0.0f) return;

    std::vector<float> others;
    for (auto& d : g_documents) {
        if (&d == &doc || d.missing) continue;
        for (const auto& p : d.pages) if (p.world_h > 0.0f) others.push_back(p.world_h);
    }
    float target_h = others.empty() ? 792.0f : median_h(others);
    float scale = target_h / cur_h;
    if (!(scale > 0.0f) || std::abs(scale - 1.0f) < 1e-3f) return;   // no-op / degenerate

    Vec2 pivot = {0.0f, 0.0f};
    for (const auto& p : doc.pages) {
        pivot.x += p.world_pos.x + p.world_w * 0.5f;
        pivot.y += p.world_pos.y + p.world_h * 0.5f;
    }
    pivot.x /= (float)doc.pages.size();
    pivot.y /= (float)doc.pages.size();

    UndoRecord r; r.type = UndoRecord::Type::DocScale;
    for (auto& p : doc.pages) {
        r.page_moves.push_back({p.id, p.world_pos, p.world_w, p.world_h});
        p.world_pos = { pivot.x + (p.world_pos.x - pivot.x) * scale,
                        pivot.y + (p.world_pos.y - pivot.y) * scale };
        p.world_w *= scale;
        p.world_h *= scale;
    }
    push_undo(r);
    doc.stack_origin = { pivot.x + (doc.stack_origin.x - pivot.x) * scale,
                         pivot.y + (doc.stack_origin.y - pivot.y) * scale };
}

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
                    const char* pats[] = {"*.pdf", "*.PDF"};
#ifdef __APPLE__
                    const char* picked = scholion_open_file("Locate the missing PDF", pats, 2, 0);
#else
                    before_file_dialog();
                    const char* picked = tinyfd_openFileDialog(
                        "Locate the missing PDF", doc->path.c_str(), 2, pats, "PDF Documents", 0);
                    after_file_dialog();
#endif
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
            if (doc->locked) {
                ImGui::TextDisabled("Password-protected PDF:");
                ImGui::TextDisabled("%s", doc->path.c_str());
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Return to Stack")) {
                if (!doc->pages.empty()) doc->stack_origin = doc->pages[0].world_pos;
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                r.page_moves.push_back({page->id, page->world_pos});
                push_undo(r);
                page->world_pos = {
                    doc->stack_origin.x + page->page_index * PAGE_FAN_OFFSET,
                    doc->stack_origin.y + page->page_index * PAGE_FAN_OFFSET
                };
            }
            if (ImGui::MenuItem("View in Sidebar Viewer")) {
                for (int i = 0; i < static_cast<int>(g_documents.size()); ++i) {
                    if (&g_documents[i] == doc) {
                        g_input.open_panel(i, page->page_index);
                        break;
                    }
                }
            }
            ImGui::Separator();
            // Single rotation option: 90° clockwise. Repeat to reach 180°/270°/upright.
            if (ImGui::MenuItem("Rotate 90\xc2\xb0")) { apply_page_rotation(page, 90); ImGui::CloseCurrentPopup(); }
            if (ImGui::MenuItem("Normalize Size")) {
                normalize_document_size(*doc);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("Scale this document to match the standard page size on the canvas");
            ImGui::Separator();
            if (ImGui::MenuItem("Fan Pages Vertically")) {
                if (!doc->pages.empty()) doc->stack_origin = doc->pages[0].world_pos;
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                for (auto& p : doc->pages) r.page_moves.push_back({p.id, p.world_pos});
                push_undo(r);
                float y = doc->stack_origin.y;
                for (auto& p : doc->pages) { p.world_pos = {doc->stack_origin.x, y}; y += p.world_h + 20.0f; }
            }
            if (ImGui::MenuItem("Fan Pages Horizontally")) {
                if (!doc->pages.empty()) doc->stack_origin = doc->pages[0].world_pos;
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                for (auto& p : doc->pages) r.page_moves.push_back({p.id, p.world_pos});
                push_undo(r);
                float x = doc->stack_origin.x;
                for (auto& p : doc->pages) { p.world_pos = {x, doc->stack_origin.y}; x += p.world_w + 20.0f; }
            }
            if (ImGui::MenuItem("Stack Pages")) {
                if (!doc->pages.empty()) doc->stack_origin = doc->pages[0].world_pos;
                UndoRecord r; r.type = UndoRecord::Type::PageMove;
                for (auto& p : doc->pages) r.page_moves.push_back({p.id, p.world_pos});
                push_undo(r);
                for (auto& p : doc->pages) {
                    p.world_pos = {
                        doc->stack_origin.x + p.page_index * PAGE_FAN_OFFSET,
                        doc->stack_origin.y + p.page_index * PAGE_FAN_OFFSET
                    };
                }
            }
            // Align + Normalize — only shown when ≥2 pages are selected and
            // the right-clicked page is part of that selection.
            const auto& sel = g_input.selection();
            std::vector<Page*> selp = selected_pages();   // resolved live pages for iteration
            if (sel.size() >= 2 && sel.count(page->id)) {
                ImGui::Separator();

                // Helper: snapshot selection for undo, then call op()
                auto with_move_undo = [&](auto op) {
                    UndoRecord r; r.type = UndoRecord::Type::PageMove;
                    for (Page* p : selp) r.page_moves.push_back({p->id, p->world_pos});
                    push_undo(r);
                    op();
                };

                if (ImGui::BeginMenu("Align Selection")) {
                    if (ImGui::MenuItem("Left Edges")) with_move_undo([&]{
                        for (Page* p : selp) p->world_pos.x = page->world_pos.x;
                    });
                    if (ImGui::MenuItem("Right Edges")) with_move_undo([&]{
                        float ref = page->world_pos.x + page->world_w;
                        for (Page* p : selp) p->world_pos.x = ref - p->world_w;
                    });
                    if (ImGui::MenuItem("Top Edges")) with_move_undo([&]{
                        for (Page* p : selp) p->world_pos.y = page->world_pos.y;
                    });
                    if (ImGui::MenuItem("Bottom Edges")) with_move_undo([&]{
                        float ref = page->world_pos.y + page->world_h;
                        for (Page* p : selp) p->world_pos.y = ref - p->world_h;
                    });
                    if (ImGui::MenuItem("Centers Horizontal")) with_move_undo([&]{
                        float ref = page->world_pos.x + page->world_w * 0.5f;
                        for (Page* p : selp) p->world_pos.x = ref - p->world_w * 0.5f;
                    });
                    if (ImGui::MenuItem("Centers Vertical")) with_move_undo([&]{
                        float ref = page->world_pos.y + page->world_h * 0.5f;
                        for (Page* p : selp) p->world_pos.y = ref - p->world_h * 0.5f;
                    });
                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem("Normalize Width to This")) {
                    if (page->world_w > 0.0f) {
                        UndoRecord r; r.type = UndoRecord::Type::PageResize;
                        for (Page* p : selp)
                            r.page_moves.push_back({p->id, p->world_pos, p->world_w, p->world_h});
                        push_undo(r);
                        for (Page* p : selp) {
                            if (p == page || p->world_w <= 0.0f) continue;
                            float scale   = page->world_w / p->world_w;
                            p->world_w    = page->world_w;
                            p->world_h   *= scale;
                        }
                    }
                }
            }

            // Grouping — gather pages into an ad-hoc movable cluster, or dissolve one.
            bool can_group   = sel.size() >= 2 && sel.count(page->id);
            bool can_ungroup = page->group_id != 0;
            if (can_group || can_ungroup) {
                ImGui::Separator();
                if (can_group) {
                    if (ImGui::MenuItem("Group Selected Pages")) {
                        create_group_from_selection();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetItemTooltip("Frame these pages so they move together — drag the frame to move the whole group");
                }
                if (can_ungroup) {
                    if (ImGui::MenuItem("Remove Page from Group")) {
                        remove_page_from_group(page);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetItemTooltip("Take just this page out of the group (the rest stay grouped)");
                    if (ImGui::MenuItem("Ungroup")) {
                        ungroup_group(page->group_id);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetItemTooltip("Dissolve this group; its pages return to their normal document behavior");
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


static void new_project() {
    clear_documents();
    g_text_boxes.clear();
    g_canvas_strokes.clear();
    s_editing_ref_hl  = nullptr;      // notes live on highlights now; cleared with the documents
    s_ref_note_buf[0] = '\0';
    g_groups.clear();
    g_next_group_id = 1;
    g_next_page_id  = 1;
    g_editing_group = 0;
    g_group_remove_flashes.clear();
    g_load_ok = true; g_dirty = false; g_last_save_wall = 0;   // fresh, unsaved state
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
        // Button width tracks the UI scale so labels fit in Larger-UI mode.
        const float ui        = ImGui::GetIO().FontGlobalScale;
        const float btn_w     = 260.0f * ui;
        const char* app_title = "Scholion " SCHOLION_VERSION;
        const char* subtitle  = "Open a project or add PDFs to begin";

        // Measure widths up front (the title is drawn at 1.5x window font scale), then
        // center everything within a single content width so nothing goes off-center
        // no matter which element is widest — holds in both normal and Larger-UI modes.
        ImGui::SetWindowFontScale(1.5f);
        float title_w = ImGui::CalcTextSize(app_title).x;
        ImGui::SetWindowFontScale(1.0f);
        float sub_w   = ImGui::CalcTextSize(subtitle).x;
        float content_w = std::max({btn_w, title_w, sub_w});

        auto center_in = [&](float w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (content_w - w) * 0.5f));
        };

        // App logo — kept small (fixed size) in both UI modes, centered above the title.
        if (g_logo_tex) {
            const float logo = 72.0f;
            center_in(logo);
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_logo_tex)), {logo, logo});
            ImGui::Spacing();
        }

        ImGui::SetWindowFontScale(1.5f);
        center_in(title_w);
        ImGui::TextUnformatted(app_title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();

        center_in(sub_w);
        ImGui::TextUnformatted(subtitle);
        ImGui::Spacing();

        const ImVec2 bsz = {btn_w, 0.0f};
        auto centered_button = [&](const char* label) {
            center_in(btn_w);
            return ImGui::Button(label, bsz);
        };

        if (centered_button("Open Project File")) {
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
            load_project();
        }
        if (centered_button("Add PDF File(s)")) {
            const char* pats[] = {"*.pdf", "*.PDF"};
#ifdef __APPLE__
            const char* r = scholion_open_file("Select PDF files", pats, 2, 1);
#else
            before_file_dialog();
            const char* r = tinyfd_openFileDialog("Select PDF files", nullptr, 2, pats, "PDF Documents", 1);
            after_file_dialog();
#endif
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
            if (r) load_pdfs_from_selection(r);
        }
        if (centered_button("Add Folder of PDFs")) {
#ifdef __APPLE__
            const char* d = scholion_select_folder("Select PDF folder");
#else
            before_file_dialog();
            const char* d = tinyfd_selectFolderDialog("Select PDF folder", nullptr);
            after_file_dialog();
#endif
            g_startup_chooser = false; ImGui::CloseCurrentPopup();
            if (d) load_pdfs_from_folder(d);
        }
        ImGui::Separator();
        if (centered_button("Start with Blank Canvas")) {
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
        apply_appearance();
        save_prefs();
    }
    bool prev_vignette = g_settings.vignette_on;
    ImGui::Checkbox("Canvas Vignette", &g_settings.vignette_on);
    if (g_settings.vignette_on != prev_vignette)
        save_prefs();

    bool prev_large = g_settings.large_ui;
    ImGui::Checkbox("Larger UI", &g_settings.large_ui);
    ImGui::SetItemTooltip("Scale all text, buttons, and panels up for smaller or high-resolution screens");
    if (g_settings.large_ui != prev_large) {
        apply_appearance();
        save_prefs();
    }

    // --- Canvas Grid ---
    ImGui::SeparatorText("Canvas Grid");
    GridMode prev_grid = g_settings.grid_mode;
    ImGui::RadioButton("Off",        (int*)&g_settings.grid_mode, (int)GridMode::Off);
    ImGui::RadioButton("Line Grid",  (int*)&g_settings.grid_mode, (int)GridMode::Lines);
    ImGui::RadioButton("Dot Matrix", (int*)&g_settings.grid_mode, (int)GridMode::Dots);
    if (g_settings.grid_mode != prev_grid) {
        save_prefs();
    }

    // --- Compatibility Mode ---
    ImGui::SeparatorText("Performance");
    bool prev_compat = g_settings.compat_mode;
    ImGui::Checkbox("Compatibility Mode", &g_settings.compat_mode);
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

#ifdef __APPLE__
#define MODK "Cmd"
#else
#define MODK "Ctrl"
#endif
        auto row = [](const char* action, const char* key) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", key);
        };

        row("Pan",                              "Middle-drag / Space+drag");
        row("Zoom",                             "Scroll wheel");
        row("Zoom to Fit",                      MODK "+0 / Middle double-click");
        row("Toggle Status Overlay",            "F3");
        row("Full-Text Search",                 MODK "+F");
        row("Select Page",                      "Click");
        row("Open Sidebar Viewer",              "Double-click");
        row("Toggle Sidebar Viewer",            "Space (tap)");
        row("Move Page",                        "Drag");
        row("Toggle Whole-Document Selection",  "Shift+click");
        row("Toggle Item in Selection",         MODK "+click");
        row("Select All",                       MODK "+A");
        row("Rubber-Band Select",               "Drag empty canvas");
        row("Clear Selection / Close Sidebar Viewer", "Escape");
        row("Text Tool",                        "T");
        row("Pen Tool",                         "P");
        row("Highlight Tool",                   "H");
        row("Create Text Box",                  "Drag (T active)");
        row("Edit Text Box",                    "Double-click box");
        row("Duplicate Text Box",               MODK "+C, " MODK "+V");
        row("Delete Text Box",                  "Delete / Backspace");
        row("Undo",                             MODK "+Z");
        row("Save",                             MODK "+S");
        ImGui::EndTable();
#undef MODK
    }

    // Footer with attribution — three center-aligned lines, wrapping to available width
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

    auto center_line = [](const char* s) {
        float avail = ImGui::GetContentRegionAvail().x;
        float tw    = ImGui::CalcTextSize(s).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - tw) * 0.5f));
        ImGui::TextUnformatted(s);
    };

    // Small centered logo at the top of the footer.
    if (g_logo_tex) {
        const float logo = 44.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - logo) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_logo_tex)), {logo, logo});
        ImGui::Spacing();
    }

    center_line("Scholion is a canvas-style PDF review utility.");
    center_line("designed and built by @armmrdn (2026)");
    ImGui::Spacing();
    center_line("a scholion is any note, detail, or definition");
    center_line("handwritten into the margin of a manuscript");
    center_line("by its previous scholars and readers");

    ImGui::Spacing();
    {
        const char* ver = "v" SCHOLION_VERSION;
        float ver_w = ImGui::CalcTextSize(ver).x;
        float avail  = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, avail - ver_w));
        ImGui::TextDisabled("%s", ver);
    }

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
        if (ImGui::MenuItem("Open Project..."))
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
            const char* patterns[] = {"*.pdf", "*.PDF"};
#ifdef __APPLE__
            const char* r = scholion_open_file("Select PDF files", patterns, 2, 1);
#else
            before_file_dialog();
            const char* r = tinyfd_openFileDialog("Select PDF files", nullptr,
                                                  2, patterns, "PDF Documents", 1);
            after_file_dialog();
#endif
            if (r) load_pdfs_from_selection(r);
        }
        if (ImGui::MenuItem("Add Folder...")) {
#ifdef __APPLE__
            const char* r = scholion_select_folder("Select PDF folder");
#else
            before_file_dialog();
            const char* r = tinyfd_selectFolderDialog("Select PDF folder", nullptr);
            after_file_dialog();
#endif
            if (r) load_pdfs_from_folder(r);
        }
        ImGui::Separator();

        // App preferences
        if (ImGui::MenuItem("Settings"))
            g_settings_open = true;
        ImGui::Separator();

        // Exit
        if (ImGui::MenuItem("Quit", "Cmd+Q")) {
            g_quit_requested = true;
        }
        ImGui::EndPopup();
    }
}

// --- References tab (inside panel) ------------------------------------------

struct RefEntry { int di, pi, hi; };  // indices into g_documents[di].pages[pi].annots.highlights[hi]

static void draw_references_tab() {
    namespace fs = std::filesystem;
    static AnnotHighlight* s_prev_editing_hl = nullptr;
    static std::unordered_set<int> s_expanded_refs;  // reference indices shown fully expanded

    // Build a flat sorted list of all text highlights across all documents.
    // Sorted by document order then page order (stable presentation).
    std::vector<RefEntry> entries;
    for (int di = 0; di < (int)g_documents.size(); ++di)
        for (int pi = 0; pi < (int)g_documents[di].pages.size(); ++pi)
            for (int hi = 0; hi < (int)g_documents[di].pages[pi].annots.highlights.size(); ++hi)
                if (!g_documents[di].pages[pi].annots.highlights[hi].text.empty())
                    entries.push_back({di, pi, hi});

    // Drop a stale note-editing pointer if its highlight is gone (e.g., its document was
    // removed mid-edit). Runs before any use of s_editing_ref_hl this frame, so the pointer
    // is never dereferenced dangling.
    if (s_editing_ref_hl) {
        bool live = false;
        for (const auto& e : entries)
            if (&g_documents[e.di].pages[e.pi].annots.highlights[e.hi] == s_editing_ref_hl) { live = true; break; }
        if (!live) s_editing_ref_hl = nullptr;
    }

    // Right-aligned export button
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - 100.0f);
    if (ImGui::SmallButton("Export .html")) {
        const char* filters[] = {"*.html"};
#ifdef __APPLE__
        const char* out_path = scholion_save_file("Export References", "references.html", filters, 1);
#else
        before_file_dialog();
        const char* out_path = tinyfd_saveFileDialog(
            "Export References", "references.html", 1, filters, "HTML file");
        after_file_dialog();
#endif
        if (out_path) {
            FILE* f = fopen(out_path, "w");
            if (f) {
                // Escape < > & " for safe HTML embedding
                auto esc = [](const std::string& s) {
                    std::string r; r.reserve(s.size());
                    for (char c : s) {
                        if      (c == '&')  r += "&amp;";
                        else if (c == '<')  r += "&lt;";
                        else if (c == '>')  r += "&gt;";
                        else if (c == '"')  r += "&quot;";
                        else                r += c;
                    }
                    return r;
                };
                fprintf(f,
                    "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
                    "<meta charset=\"UTF-8\">\n"
                    "<title>Scholion References</title>\n"
                    "<style>\n"
                    "  body{font-family:Georgia,serif;max-width:740px;margin:40px auto;"
                         "padding:0 24px;background:#f9f9f7;color:#222;}\n"
                    "  h1{font-size:1.3em;font-weight:normal;color:#666;"
                         "border-bottom:1px solid #ccc;padding-bottom:8px;margin-bottom:24px;}\n"
                    "  .entry{margin:0 0 20px 0;}\n"
                    "  blockquote{margin:0 0 5px 0;padding:10px 16px;"
                         "background:#fffef0;border-left:3px solid #c8a84b;"
                         "font-style:italic;color:#333;}\n"
                    "  .source{font-size:0.80em;color:#999;margin:0 0 6px 16px;}\n"
                    "  .note{font-size:0.88em;color:#555;margin:6px 0 0 20px;"
                         "padding:7px 12px;background:#f0f0f5;border-radius:4px;"
                         "font-style:italic;white-space:pre-wrap;"
                         "border-left:2px solid #aab;}\n"
                    "  hr{border:none;border-top:1px solid #ddd;margin:20px 0;}\n"
                    "  @media print{body{background:#fff;}}\n"
                    "</style>\n</head>\n<body>\n"
                    "<h1>Scholion References</h1>\n");
                for (int gi = 0; gi < (int)entries.size(); ++gi) {
                    const auto& e = entries[gi];
                    const Document& doc = g_documents[e.di];
                    const Page&     pg  = doc.pages[e.pi];
                    const auto&     hl  = pg.annots.highlights[e.hi];
                    std::string fname = esc(fs::path(doc.path).filename().string());
                    std::string text  = esc(hl.text);
                    fprintf(f, "<div class=\"entry\">\n");
                    fprintf(f, "  <blockquote>&#8220;%s&#8221;</blockquote>\n", text.c_str());
                    fprintf(f, "  <div class=\"source\">%s &mdash; p.%d</div>\n",
                            fname.c_str(), pg.page_index + 1);
                    if (!hl.note.empty())
                        fprintf(f, "  <div class=\"note\">%s</div>\n", esc(hl.note).c_str());
                    fprintf(f, "</div>\n");
                    if (gi + 1 < (int)entries.size()) fprintf(f, "<hr>\n");
                }
                fprintf(f, "</body>\n</html>\n");
                fclose(f);
            }
        }
    }
    ImGui::SetItemTooltip("Export all highlights and notes as an HTML file (opens in any browser)");
    ImGui::Separator();

    if (entries.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, {0.5f, 0.5f, 0.5f, 1.0f});
        ImGui::TextWrapped(
            "No text highlights yet.\n\n"
            "Select the Highlight tool, then drag across text on any page "
            "\xe2\x80\x94 it will appear here with the filename and page number.");
        ImGui::PopStyleColor();
        // NOTE: no early return — the Open Documents list below must stay reachable.
    }

    for (int gi = 0; gi < (int)entries.size(); ++gi) {
        const auto& e = entries[gi];
        const Document& doc = g_documents[e.di];
        const Page&     pg  = doc.pages[e.pi];
        const auto&     hl  = pg.annots.highlights[e.hi];

        // --- Highlight card ---
        // Disclosure arrow on the left toggles the full blurb; source line follows.
        bool expanded = s_expanded_refs.count(gi) > 0;
        char aid[24]; snprintf(aid, sizeof(aid), "##rex%d", gi);
        if (ImGui::ArrowButton(aid, expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
            if (expanded) s_expanded_refs.erase(gi); else s_expanded_refs.insert(gi);
            expanded = !expanded;
        }
        ImGui::SetItemTooltip(expanded ? "Collapse" : "Show full text");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, {doc.hue_r, doc.hue_g, doc.hue_b, 0.85f});
        std::string fname = fs::path(doc.path).filename().string();
        char source[256]; snprintf(source, sizeof(source), "%s · p.%d", fname.c_str(), pg.page_index + 1);
        ImGui::TextUnformatted(source);
        ImGui::PopStyleColor();

        auto zoom_here = [&]{
            zoom_to_rect(pg.world_pos.x, pg.world_pos.y,
                         pg.world_pos.x + pg.world_w, pg.world_pos.y + pg.world_h);
        };
        if (!expanded) {
            // Collapsed: single-line truncated preview (newlines flattened), click → zoom.
            std::string display = hl.text;
            for (char& c : display) if (c == '\n' || c == '\r') c = ' ';
            bool truncated = display.size() > 120;
            if (truncated) { display.resize(117); display += "..."; }
            char row[512];
            snprintf(row, sizeof(row), "\"%s\"##hl%d", display.c_str(), gi);
            if (ImGui::Selectable(row, false)) zoom_here();
            if (truncated) ImGui::SetItemTooltip("%s", hl.text.c_str());
        } else {
            // Expanded: full multiline blurb wrapped to the sidebar width, click → zoom.
            float wrap_w = ImGui::GetContentRegionAvail().x;
            std::string quoted = "\"" + hl.text + "\"";
            ImVec2 tsz = ImGui::CalcTextSize(quoted.c_str(), nullptr, false, wrap_w);
            ImVec2 tl  = ImGui::GetCursorScreenPos();
            char bid[24]; snprintf(bid, sizeof(bid), "##hlful%d", gi);
            if (ImGui::InvisibleButton(bid, {wrap_w, std::max(tsz.y, ImGui::GetTextLineHeight())}))
                zoom_here();
            ImGui::GetWindowDrawList()->AddText(
                ImGui::GetFont(), ImGui::GetFontSize(), tl,
                ImGui::GetColorU32(ImGuiCol_Text), quoted.c_str(), nullptr, wrap_w);
        }

        // --- Research note for this reference (stored on the highlight itself) ---
        AnnotHighlight& note_hl = g_documents[e.di].pages[e.pi].annots.highlights[e.hi];
        bool editing  = (s_editing_ref_hl == &note_hl);
        bool has_note = !note_hl.note.empty();

        {
            const float kXW    = 20.0f;
            const float kPad   = 5.0f;
            const float kSpc   = ImGui::GetStyle().ItemSpacing.x;
            const float avail  = ImGui::GetContentRegionAvail().x;
            const float kRound = 3.0f;
            const ImU32 kBord  = IM_COL32(80, 80, 96, 150);

            // tl/cb saved before any content is drawn for this note row.
            ImVec2 cb = ImGui::GetCursorPos();
            ImVec2 tl = ImGui::GetCursorScreenPos();

            if (editing) {
                // ---- Editing: text box inset inside border, X top-right ----
                float txt_w    = avail - kXW - kSpc - kPad * 2.0f;
                ImVec2 content_sz = ImGui::CalcTextSize(s_ref_note_buf, nullptr, false, txt_w);
                float kTextH = std::max(content_sz.y, ImGui::GetTextLineHeight())
                               + ImGui::GetStyle().FramePadding.y * 2.0f + 2.0f;
                const float box_h = kPad * 2.0f + kTextH;

                // Inset cursor so text box sits inside the border with kPad margin
                ImGui::SetCursorPos({cb.x + kPad, cb.y + kPad});
                // The note box is forced dark in both themes, so pin a light text colour
                // too — otherwise light mode uses the theme's dark ImGuiCol_Text and the
                // text is invisible on the dark box (the reported light-mode bug).
                ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.10f, 0.10f, 0.13f, 0.9f});
                ImGui::PushStyleColor(ImGuiCol_Text,    {0.90f, 0.90f, 0.94f, 1.0f});
                char edit_id[32]; snprintf(edit_id, sizeof(edit_id), "##rnedit%d", gi);
                // Auto-focus input on the first frame it opens
                if (s_editing_ref_hl != s_prev_editing_hl)
                    ImGui::SetKeyboardFocusHere();
                ImGui::InputTextMultiline(edit_id, s_ref_note_buf, sizeof(s_ref_note_buf),
                                          {txt_w, kTextH});
                ImGui::PopStyleColor(2);

                // Save and close on click outside the note box
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImVec2 mp = ImGui::GetMousePos();
                    bool in_box = mp.x >= tl.x && mp.x < tl.x + avail
                               && mp.y >= tl.y && mp.y < tl.y + box_h;
                    if (!in_box) {
                        note_hl.note = s_ref_note_buf;      // empty text simply clears the note
                        s_editing_ref_hl = nullptr;
                    }
                }

                // Border drawn after text box (outline only, doesn't obscure content)
                ImGui::GetWindowDrawList()->AddRect(
                    tl, {tl.x + avail, tl.y + box_h}, kBord, kRound);

                // X pinned to top-right corner of the border box — deletes the note
                ImGui::SetCursorScreenPos({tl.x + avail - kXW - 1.0f, tl.y + 2.0f});
                char del_id[32]; snprintf(del_id, sizeof(del_id), "X##rnd%d", gi);
                if (ImGui::SmallButton(del_id)) {
                    note_hl.note.clear();
                    s_editing_ref_hl = nullptr;
                }

                // Advance cursor past the whole box
                ImGui::SetCursorPos({cb.x, cb.y + box_h + ImGui::GetStyle().ItemSpacing.y});

            } else if (has_note) {
                // ---- Display: full-box hitbox, DrawList text, X top-right ----
                const char* note_text = note_hl.note.c_str();
                // Wrap width: avail minus X button, border padding, and extra indent
                const float wrap_w = avail - kXW - kSpc - kPad * 2.0f - 8.0f;
                ImVec2 text_sz = ImGui::CalcTextSize(note_text, nullptr, false, wrap_w);
                const float box_h = kPad * 2.0f + text_sz.y;

                // InvisibleButton covers the whole box — becomes the re-edit hit target.
                // Allow overlap so the X SmallButton drawn on top of it still wins the click.
                char bg_id[32]; snprintf(bg_id, sizeof(bg_id), "##rna_bg%d", gi);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton(bg_id, {avail, box_h});
                bool bg_clicked = ImGui::IsItemHovered()
                               && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                // Border
                ImGui::GetWindowDrawList()->AddRect(
                    tl, {tl.x + avail, tl.y + box_h}, kBord, kRound);

                // Left accent bar — visually marks the block as a note
                ImGui::GetWindowDrawList()->AddLine(
                    {tl.x + 3.5f, tl.y + 4.0f},
                    {tl.x + 3.5f, tl.y + box_h - 4.0f},
                    IM_COL32(130, 130, 178, 180), 2.0f);

                // Note text: indented; colour follows the theme so it stays readable in
                // light mode (a light lavender washes out on the light window background).
                ImU32 note_col = g_settings.dark_mode ? IM_COL32(185, 185, 205, 240)
                                                      : IM_COL32( 55,  55,  75, 255);
                ImGui::GetWindowDrawList()->AddText(
                    ImGui::GetFont(), ImGui::GetFontSize(),
                    {tl.x + kPad + 8.0f, tl.y + kPad},
                    note_col,
                    note_text, nullptr, wrap_w);

                // X pinned top-right — drawn after InvisibleButton so it wins the hit test
                ImGui::SetCursorScreenPos({tl.x + avail - kXW - 1.0f, tl.y + 2.0f});
                char del_id[32]; snprintf(del_id, sizeof(del_id), "X##rnd%d", gi);
                bool x_clicked = ImGui::SmallButton(del_id);

                // Restore cursor to after the box
                ImGui::SetCursorPos({cb.x, cb.y + box_h + ImGui::GetStyle().ItemSpacing.y});

                if (x_clicked) {
                    note_hl.note.clear();
                } else if (bg_clicked) {
                    s_editing_ref_hl = &note_hl;
                    strncpy(s_ref_note_buf, note_text, sizeof(s_ref_note_buf) - 1);
                    s_ref_note_buf[sizeof(s_ref_note_buf) - 1] = '\0';
                }

            } else {
                // ---- No note: bordered placeholder, full box is the hit target ----
                const float box_h = ImGui::GetFrameHeight() + kPad * 2.0f;

                char bg_id[32]; snprintf(bg_id, sizeof(bg_id), "##rna_bg%d", gi);
                ImGui::InvisibleButton(bg_id, {avail, box_h});
                bool activated = ImGui::IsItemHovered()
                              && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                // Subtle hover fill
                if (ImGui::IsItemHovered())
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        tl, {tl.x + avail, tl.y + box_h},
                        IM_COL32(80, 80, 100, 22), kRound);

                // Border
                ImGui::GetWindowDrawList()->AddRect(
                    tl, {tl.x + avail, tl.y + box_h}, kBord, kRound);

                // Centered placeholder text (theme-aware so it reads in light mode too)
                const char* ph = "Attach Note to Reference";
                ImVec2 ts = ImGui::CalcTextSize(ph);
                ImU32 ph_col = g_settings.dark_mode ? IM_COL32(90, 90, 108, 200)
                                                    : IM_COL32(120, 120, 135, 220);
                ImGui::GetWindowDrawList()->AddText(
                    {tl.x + (avail - ts.x) * 0.5f, tl.y + (box_h - ts.y) * 0.5f},
                    ph_col, ph);

                if (activated) {
                    s_editing_ref_hl = &note_hl;
                    s_ref_note_buf[0] = '\0';
                }
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, {0.45f, 0.45f, 0.52f, 0.85f});
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    s_prev_editing_hl = s_editing_ref_hl;

    // ---- Open Documents --------------------------------------------------------
    if (g_documents.empty()) return;

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, {0.55f, 0.55f, 0.62f, 1.0f});
    ImGui::SeparatorText("Open Documents");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    float avail_d = ImGui::GetContentRegionAvail().x;
    for (int di = 0; di < (int)g_documents.size(); ++di) {
        Document& doc = g_documents[di];
        bool offline  = doc.missing;

        ImGui::PushID(di);

        // Filename row — selectable, toggles show_threads
        ImVec4 name_col = offline
            ? ImVec4(0.80f, 0.22f, 0.22f, 1.0f)
            : ImVec4(doc.hue_r, doc.hue_g, doc.hue_b, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, name_col);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
            ImVec4(doc.hue_r * 0.25f, doc.hue_g * 0.25f, doc.hue_b * 0.25f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_Header,
            ImVec4(doc.hue_r * 0.25f, doc.hue_g * 0.25f, doc.hue_b * 0.25f, 0.35f));

        std::string fname = fs::path(doc.path).filename().string();
        ImGui::Selectable(fname.c_str(), doc.show_threads,
                          ImGuiSelectableFlags_AllowDoubleClick, {avail_d, 0.0f});
        if (ImGui::IsItemClicked() && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            doc.show_threads = !doc.show_threads;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (offline) {
                const char* pats[] = {"*.pdf", "*.PDF"};
#ifdef __APPLE__
                const char* picked = scholion_open_file("Locate the missing PDF", pats, 2, 0);
#else
                before_file_dialog();
                const char* picked = tinyfd_openFileDialog(
                    "Locate the missing PDF", doc.path.c_str(), 2, pats, "PDF Documents", 0);
                after_file_dialog();
#endif
                if (picked) {
                    relink_document(di, picked);
                    ImGui::PopStyleColor(3);
                    ImGui::PopID();
                    break;  // doc reference is now invalid
                }
            } else {
                reveal_in_file_manager(doc.path);
            }
        }
        {
            const char* tt_line2 = offline
                ? "Double-click to relink this document"
                : "Double-click to reveal in Finder";
            const char* tt_line1 = doc.show_threads
                ? "Click to hide connection threads"
                : "Click to show connection threads";
            char tt[128];
            snprintf(tt, sizeof(tt), "%s\n%s", tt_line1, tt_line2);
            ImGui::SetItemTooltip("%s", tt);
        }

        ImGui::PopStyleColor(3);

        // Filepath: abbreviated to last two directory components + filename
        {
            namespace fs2 = std::filesystem;
            fs2::path p(doc.path);
            fs2::path par  = p.parent_path();
            fs2::path gpar = par.parent_path();
            std::string short_path;
            if (!gpar.empty() && !gpar.filename().empty())
                short_path = ".../" + par.parent_path().filename().string()
                           + "/" + par.filename().string() + "/" + p.filename().string();
            else
                short_path = doc.path;

            ImGui::PushStyleColor(ImGuiCol_Text,
                offline ? ImVec4(0.70f, 0.28f, 0.28f, 0.80f)
                        : ImVec4(0.45f, 0.45f, 0.50f, 0.85f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(short_path.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::PopID();
        ImGui::Spacing();
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
    s_last_panel_doc = doc_idx;  // remember for edge-tab re-open
    ImVec2 vp = ImGui::GetMainViewport()->Size;

    ImGui::SetNextWindowPos({vp.x - g_panel_w, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({g_panel_w, vp.y}, ImGuiCond_Always);
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

    // Track the "active" page for nav and copy (updated on scroll-to events)
    int scroll_to_peek = g_input.panel_scroll_page();  // read before clear
    if (ImGui::IsWindowAppearing()) s_panel_nav_page = std::max(0, scroll_to_peek);
    else if (scroll_to_peek >= 0)  s_panel_nav_page = scroll_to_peek;

    // Tab bar — "Viewer" / "References" — blue-themed, pinned at the top of the panel.
    // On first appearance, honour g_panel_open_to_refs to select the right tab.
    namespace fs = std::filesystem;
    bool just_appeared = ImGui::IsWindowAppearing();
    ImGuiTabItemFlags viewer_flags = (just_appeared && !g_panel_open_to_refs)
                                     ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags ref_flags    = (just_appeared &&  g_panel_open_to_refs)
                                     ? ImGuiTabItemFlags_SetSelected : 0;
    if (just_appeared) g_panel_open_to_refs = false;

    ImGui::PushStyleColor(ImGuiCol_Tab,                 ImVec4(0.14f, 0.33f, 0.65f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,          ImVec4(0.20f, 0.42f, 0.78f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,         ImVec4(0.27f, 0.51f, 0.88f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.50f, 0.75f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed,           ImVec4(0.10f, 0.24f, 0.50f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,   ImVec4(0.18f, 0.40f, 0.72f, 0.90f));
    ImGui::BeginTabBar("##panel_tabs");
    ImGui::PopStyleColor(6);

    if (ImGui::BeginTabItem("Viewer", nullptr, viewer_flags)) {
        // Document name below tab bar
        std::string fname = fs::path(doc.path).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, {doc.hue_r, doc.hue_g, doc.hue_b, 1.0f});
        ImGui::TextUnformatted(fname.c_str());
        ImGui::PopStyleColor();

        // Page navigation  < p.N/Total >
        {
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

    // Child window fills the remaining panel height and is the only scrollable region.
    ImGui::BeginChild("##panel_scroll", {0.0f, 0.0f}, false, ImGuiWindowFlags_None);
    ImGui::Spacing();

    float avail_w  = ImGui::GetContentRegionAvail().x;
    int   scroll_to = g_input.panel_scroll_page();

    for (int pi = 0; pi < (int)doc.pages.size(); ++pi) {
        Page& page = doc.pages[pi];

        // Panel uses Low tier (150 DPI); sufficient for the panel's ~360px display width.
        // High-tier tiles are managed by stream_lod for canvas use only.
        if (page.needs_lod(LodTier::Low) && g_loaders[doc_idx])
            enqueue_rast(doc.path, g_loaders[doc_idx], page.page_index, LodTier::Low);
        uint32_t tex = page.tex_for_lod(LodTier::Low);

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

        // Draw orange search hit rects (text-level, from g_search_highlight)
        if (g_search_highlight.active() &&
            g_search_highlight.doc_idx == doc_idx &&
            g_search_highlight.page_idx == pi) {
            for (const auto& r : g_search_highlight.rects) {
                ImVec2 tl = {img_pos.x + r[0] * img_w, img_pos.y + r[1] * img_h};
                ImVec2 br = {img_pos.x + r[2] * img_w, img_pos.y + r[3] * img_h};
                dl->AddRectFilled(tl, br, IM_COL32(255, 128, 0, 130));
            }
        }

        // Annotation tool interaction in panel
        {
            ImVec2 mouse  = ImGui::GetMousePos();
            float  nx     = std::clamp((mouse.x - img_pos.x) / img_w, 0.0f, 1.0f);
            float  ny     = std::clamp((mouse.y - img_pos.y) / img_h, 0.0f, 1.0f);
            Vec2   pnorm  = {nx, ny};
            bool   hov    = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

            if (g_annot_tool == AnnotTool::Note && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                int snap = g_next_note_idx;
                page.annots.notes.push_back({note_label(g_next_note_idx++)});
                UndoRecord r; r.type = UndoRecord::Type::Note;
                r.doc_idx = doc_idx; r.page_idx = pi; r.note_idx_before = snap;
                push_undo(r);
            }

            if ((g_annot_tool == AnnotTool::Highlight || g_annot_tool == AnnotTool::Pen)
                    && !g_ann_drawing && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_ann_drawing      = true;
                g_ann_doc_idx      = doc_idx;
                g_ann_page_idx     = pi;
                s_panel_ann_active = true;
                if (g_annot_tool == AnnotTool::Pen ||
                    (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode)) {
                    g_ann_cur_stroke = {};
                    if (g_annot_tool == AnnotTool::Pen) {
                        g_ann_cur_stroke.r = g_pen_r; g_ann_cur_stroke.g = g_pen_g; g_ann_cur_stroke.b = g_pen_b;
                    } else {
                        g_ann_cur_stroke.r = g_hl_r; g_ann_cur_stroke.g = g_hl_g; g_ann_cur_stroke.b = g_hl_b;
                        g_ann_cur_stroke.width = MARKER_HALF_W; g_ann_cur_stroke.alpha = MARKER_ALPHA;
                    }
                    g_ann_cur_stroke.pts.push_back(pnorm);
                }
                g_ann_hl_start = pnorm;
                g_ann_cur_norm = pnorm;
            }

            if (g_ann_drawing && s_panel_ann_active &&
                    g_ann_doc_idx == doc_idx && g_ann_page_idx == pi) {
                if ((g_annot_tool == AnnotTool::Pen ||
                     (g_annot_tool == AnnotTool::Highlight && !g_hl_box_mode))
                        && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    stroke_add_point(page, pnorm, ImGui::GetIO().KeyShift);  // Shift = ortho-lock
                    const auto& pts = g_ann_cur_stroke.pts;
                    bool  hlt   = (g_annot_tool == AnnotTool::Highlight);
                    ImU32 col   = hlt
                        ? IM_COL32((int)(g_hl_r*255),(int)(g_hl_g*255),(int)(g_hl_b*255),(int)(MARKER_ALPHA*255))
                        : IM_COL32((int)(g_pen_r*255),(int)(g_pen_g*255),(int)(g_pen_b*255),220);
                    float thick = hlt ? MARKER_HALF_W * 2.0f : 2.0f;
                    for (int si = 1; si < (int)pts.size(); ++si) {
                        ImVec2 a = {img_pos.x + pts[si-1].x * img_w, img_pos.y + pts[si-1].y * img_h};
                        ImVec2 b = {img_pos.x + pts[si  ].x * img_w, img_pos.y + pts[si  ].y * img_h};
                        dl->AddLine(a, b, col, thick);
                    }
                } else if (g_annot_tool == AnnotTool::Highlight && g_hl_box_mode) {
                    // Box mode — track the rectangle and preview it in yellow.
                    g_ann_cur_norm = pnorm;
                    float x0 = std::min(g_ann_hl_start.x, pnorm.x), y0 = std::min(g_ann_hl_start.y, pnorm.y);
                    float x1 = std::max(g_ann_hl_start.x, pnorm.x), y1 = std::max(g_ann_hl_start.y, pnorm.y);
                    dl->AddRectFilled(
                        {img_pos.x + x0 * img_w, img_pos.y + y0 * img_h},
                        {img_pos.x + x1 * img_w, img_pos.y + y1 * img_h},
                        IM_COL32(255, 224, 0, 80));
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    s_panel_ann_active = false;
                    // finalize_annotation() is called by the main draw loop
            }

            if (g_annot_tool == AnnotTool::Eraser && hov && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                constexpr float ER = 0.025f;
                for (int hi = (int)page.annots.highlights.size() - 1; hi >= 0; --hi) {
                    const auto& hl = page.annots.highlights[hi];
                    float cx = std::clamp(nx, hl.x0, hl.x1), cy = std::clamp(ny, hl.y0, hl.y1);
                    if ((nx-cx)*(nx-cx) + (ny-cy)*(ny-cy) <= ER*ER) {
                        UndoRecord r; r.type = UndoRecord::Type::ErasedHighlight;
                        r.doc_idx = doc_idx; r.page_idx = pi; r.erased_highlight = hl;
                        push_undo(r);
                        page.annots.highlights.erase(page.annots.highlights.begin() + hi);
                    }
                }
                for (int si = (int)page.annots.strokes.size() - 1; si >= 0; --si) {
                    bool hit = false;
                    for (const auto& pt : page.annots.strokes[si].pts) {
                        float dx = nx - pt.x, dy = ny - pt.y;
                        if (dx*dx + dy*dy <= ER*ER) { hit = true; break; }
                    }
                    if (hit) {
                        UndoRecord r; r.type = UndoRecord::Type::ErasedStroke;
                        r.doc_idx = doc_idx; r.page_idx = pi;
                        r.erased_stroke = page.annots.strokes[si];
                        push_undo(r);
                        page.annots.strokes.erase(page.annots.strokes.begin() + si);
                    }
                }
            }
        }

        // Draw saved annotations on top of page
        for (const auto& hl : page.annots.highlights) {
            ImVec2 tl = {img_pos.x + hl.x0 * img_w, img_pos.y + hl.y0 * img_h};
            ImVec2 br = {img_pos.x + hl.x1 * img_w, img_pos.y + hl.y1 * img_h};
            dl->AddRectFilled(tl, br, IM_COL32(255, 224, 0, 80));
        }
        for (const auto& stroke : page.annots.strokes) {
            int sa = (int)(stroke.alpha * 255.0f);
            for (int si = 1; si < (int)stroke.pts.size(); ++si) {
                ImVec2 a = {img_pos.x + stroke.pts[si-1].x * img_w,
                            img_pos.y + stroke.pts[si-1].y * img_h};
                ImVec2 b = {img_pos.x + stroke.pts[si  ].x * img_w,
                            img_pos.y + stroke.pts[si  ].y * img_h};
                dl->AddLine(a, b,
                    IM_COL32((int)(stroke.r*255), (int)(stroke.g*255), (int)(stroke.b*255), sa),
                    stroke.width * 2.0f);
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
    } // Viewer tab

    if (ImGui::BeginTabItem("References", nullptr, ref_flags)) {
        ImGui::BeginChild("##panel_ref_scroll", {0.0f, 0.0f}, false, ImGuiWindowFlags_None);
        draw_references_tab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

// --- Canvas (world-space) pen strokes -----------------------------------------
// Drawn via BackgroundDrawList (above the GL page layer, below ImGui windows) so canvas
// marks read as "in front" of pages while staying beneath the sidebar. Includes the
// in-progress stroke as a live preview.
static void draw_canvas_strokes() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto draw_one = [&](const AnnotStroke& s) {
        if (s.pts.size() < 2) return;
        ImU32 col = IM_COL32((int)(s.r*255), (int)(s.g*255), (int)(s.b*255), (int)(s.alpha*255));
        float th = s.width * 2.0f;
        for (size_t i = 1; i < s.pts.size(); ++i) {
            Vec2 a = g_canvas.world_to_screen(s.pts[i-1]);
            Vec2 b = g_canvas.world_to_screen(s.pts[i]);
            dl->AddLine({a.x, a.y}, {b.x, b.y}, col, th);
        }
    };
    for (const auto& s : g_canvas_strokes) draw_one(s);
    if (g_ann_canvas && g_ann_drawing) draw_one(g_ann_cur_stroke);  // live preview
}

// Eraser over the canvas removes canvas strokes whose path passes near the cursor (world
// distance). Runs each frame while the eraser is held; each removed stroke is undoable.
static void erase_canvas_strokes_at_cursor() {
    if (g_annot_tool != AnnotTool::Eraser) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (ImGui::GetIO().WantCaptureMouse) return;
    ImVec2 m = ImGui::GetMousePos();
    Vec2 world = g_canvas.screen_to_world({m.x, m.y});
    float zoom = g_canvas.get_zoom();
    float er  = (zoom > 1e-4f) ? (12.0f / zoom) : 12.0f;   // ~12 screen px in world units
    float er2 = er * er;
    for (int i = (int)g_canvas_strokes.size() - 1; i >= 0; --i) {
        bool hit = false;
        for (const auto& p : g_canvas_strokes[i].pts) {
            float dx = world.x - p.x, dy = world.y - p.y;
            if (dx*dx + dy*dy <= er2) { hit = true; break; }
        }
        if (hit) {
            UndoRecord r; r.type = UndoRecord::Type::ErasedCanvasStroke;
            r.canvas_idx = i; r.erased_stroke = g_canvas_strokes[i];
            push_undo(r);
            g_canvas_strokes.erase(g_canvas_strokes.begin() + i);
        }
    }
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
    constexpr float STRIP_W = 12.0f;

    ImGui::SetNextWindowPos({vp.x - g_panel_w - STRIP_W * 0.5f, 0.0f}, ImGuiCond_Always);
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

    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::InvisibleButton("##drag", {STRIP_W, vp.y});
    bool is_active  = ImGui::IsItemActive();
    bool is_hovered = ImGui::IsItemHovered();

    if (is_active) {
        g_panel_w -= ImGui::GetIO().MouseDelta.x;
        g_panel_w  = std::clamp(g_panel_w, 180.0f, vp.x - 30.0f);
    } else if (ImGui::IsItemDeactivated()) {
        g_settings.panel_w = g_panel_w;
        save_prefs();
    }
    if (is_hovered || is_active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // Blue divider line — brighter when hovered/dragged
    float cx = wp.x + STRIP_W * 0.5f;
    ImU32 line_col = is_active  ? IM_COL32(110, 165, 255, 230)
                   : is_hovered ? IM_COL32(80, 140, 230, 200)
                   :              IM_COL32(55, 108, 190, 150);
    ImGui::GetWindowDrawList()->AddLine({cx, wp.y}, {cx, wp.y + vp.y}, line_col, 2.5f);

    ImGui::End();
}

// --- Panel edge tabs (shown on right edge when sidebar is closed) -----------
// Two vertical tab handles sitting flush on the right edge — characters are
// rendered stacked top-to-bottom so the label reads downward without rotation.
// They disappear once the sidebar is open.

// Render text rotated 90° CCW, centered on 'center', using glyph quads so
// the result is crisp at any size.  Reading direction: bottom → top.
static void draw_text_ccw(ImDrawList* dl, ImVec2 center, const char* text, ImU32 col)
{
    ImFont*  font  = ImGui::GetFont();
    float    scale = ImGui::GetFontSize() / font->FontSize;
    ImVec2   tsz   = ImGui::CalcTextSize(text);
    float    cx    = tsz.x * 0.5f;   // half text-width  → vertical offset
    float    cy    = tsz.y * 0.5f;   // half text-height → horizontal offset

    float cur_x = 0.0f;
    for (const char* s = text; *s; ++s) {
        const ImFontGlyph* g = font->FindGlyph((ImWchar)(unsigned char)*s);
        if (!g) continue;
        if (g->Visible) {
            float x0 = cur_x + g->X0 * scale,  y0 = g->Y0 * scale;
            float x1 = cur_x + g->X1 * scale,  y1 = g->Y1 * scale;
            // 90° CCW in screen-space: (lx,ly) → (+ly - cy, cx - lx) + center
            ImVec2 p1 = { center.x + y0 - cy, center.y + cx - x0 };
            ImVec2 p2 = { center.x + y0 - cy, center.y + cx - x1 };
            ImVec2 p3 = { center.x + y1 - cy, center.y + cx - x1 };
            ImVec2 p4 = { center.x + y1 - cy, center.y + cx - x0 };
            dl->AddImageQuad(ImGui::GetIO().Fonts->TexID,
                p1, p2, p3, p4,
                { g->U0, g->V0 }, { g->U1, g->V0 },
                { g->U1, g->V1 }, { g->U0, g->V1 },
                col);
        }
        cur_x += g->AdvanceX * scale;
    }
}

static void draw_panel_edge_tabs() {
    if (g_input.panel_open()) return;
    if (g_documents.empty()) return;

    ImVec2 vp      = ImGui::GetMainViewport()->Size;
    float  lh      = ImGui::GetTextLineHeight();
    // When rotated 90°, text width becomes the tab height and text height
    // becomes the tab width.  Add padding in both axes.
    const float PAD_H   = 10.0f;   // left/right padding → adds to STRIP_W
    const float PAD_V   = 16.0f;   // top/bottom padding → adds to tab height
    const float TAB_GAP =  6.0f;
    float STRIP_W  = lh + PAD_H * 2.0f;
    float viewer_h = ImGui::CalcTextSize("Viewer").x     + PAD_V * 2.0f;
    float refs_h   = ImGui::CalcTextSize("References").x + PAD_V * 2.0f;
    float total_h  = viewer_h + TAB_GAP + refs_h;
    float origin_y = s_toolbar_bottom + 4.0f;

    ImGui::SetNextWindowPos({vp.x - STRIP_W, origin_y}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({STRIP_W, total_h}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0f, 1.0f});
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar          |
        ImGuiWindowFlags_NoResize            |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoScrollbar         |
        ImGuiWindowFlags_NoSavedSettings     |
        ImGuiWindowFlags_NoBackground        |
        ImGuiWindowFlags_NoFocusOnAppearing  |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##edge_tabs", nullptr, kFlags);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();

    // --- "Viewer" tab ---
    ImGui::InvisibleButton("##etab_viewer", {STRIP_W, viewer_h});
    bool v_hov = ImGui::IsItemHovered();
    bool v_act = ImGui::IsItemActive();
    if (ImGui::IsItemClicked()) {
        int di = (s_last_panel_doc >= 0 && s_last_panel_doc < (int)g_documents.size())
                 ? s_last_panel_doc : 0;
        g_panel_open_to_refs = false;
        g_input.open_panel(di, -1);
    }
    ImU32 v_bg = v_act  ? IM_COL32(70, 130, 225, 240)
               : v_hov  ? IM_COL32(50, 108, 200, 220)
               :          IM_COL32(35,  85, 165, 190);
    dl->AddRectFilled({wp.x, wp.y}, {wp.x + STRIP_W, wp.y + viewer_h},
                      v_bg, 5.0f, ImDrawFlags_RoundCornersLeft);
    draw_text_ccw(dl, {wp.x + STRIP_W * 0.5f, wp.y + viewer_h * 0.5f},
                  "Viewer", IM_COL32(210, 225, 255, 245));

    // Gap between tabs
    ImGui::Dummy({STRIP_W, TAB_GAP});

    // --- "References" tab ---
    float refs_y = wp.y + viewer_h + TAB_GAP;
    ImGui::InvisibleButton("##etab_refs", {STRIP_W, refs_h});
    bool r_hov = ImGui::IsItemHovered();
    bool r_act = ImGui::IsItemActive();
    if (ImGui::IsItemClicked()) {
        int di = (s_last_panel_doc >= 0 && s_last_panel_doc < (int)g_documents.size())
                 ? s_last_panel_doc : 0;
        g_panel_open_to_refs = true;
        g_input.open_panel(di, -1);
    }
    ImU32 r_bg = r_act  ? IM_COL32(70, 130, 225, 240)
               : r_hov  ? IM_COL32(50, 108, 200, 220)
               :          IM_COL32(35,  85, 165, 190);
    dl->AddRectFilled({wp.x, refs_y}, {wp.x + STRIP_W, refs_y + refs_h},
                      r_bg, 5.0f, ImDrawFlags_RoundCornersLeft);
    draw_text_ccw(dl, {wp.x + STRIP_W * 0.5f, refs_y + refs_h * 0.5f},
                  "References", IM_COL32(210, 225, 255, 245));

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

    // Only show when the hovered page belongs to the current selection.
    if (!g_input.selection().count(hov_page->id)) return;

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
    std::string q(g_search_buf);
    if (q.empty()) return;
    if (g_search_running) return;  // previous search still in flight

    // Clear previous results immediately so the UI shows "Searching..."
    {
        std::lock_guard<std::mutex> lk(g_search_results_mutex);
        g_search_results.clear();
    }
    g_highlighted_search_result = -1;
    g_search_running = true;

    // Snapshot doc paths so the thread doesn't touch g_documents.
    namespace fs = std::filesystem;
    struct DocSnap { int idx; std::string path; std::string name; };
    std::vector<DocSnap> snap;
    snap.reserve(g_documents.size());
    for (int di = 0; di < (int)g_documents.size(); ++di)
        snap.push_back({di, g_documents[di].path,
                        fs::path(g_documents[di].path).filename().string()});

    if (g_search_thread.joinable()) g_search_thread.join();
    g_search_thread = std::thread([q, snap = std::move(snap)]() {
        std::vector<SearchResult> results;
        for (const auto& d : snap) {
            PdfLoader tmp;
            Document  dummy;
            if (!tmp.load(d.path, dummy)) continue;
            auto hits = tmp.search_text(q, 50);
            for (auto& h : hits)
                results.push_back({d.idx, h.page_index, std::move(h.excerpt), d.name,
                                   std::move(h.hit_rects)});
        }
        {
            std::lock_guard<std::mutex> lk(g_search_results_mutex);
            g_search_results = std::move(results);
        }
        g_search_running = false;
        glfwPostEmptyEvent();  // wake the main loop so results appear immediately
    });
}

static void draw_search_panel() {
    if (!g_search_open) {
        if (g_search_highlight.active()) g_search_highlight = {};
        return;
    }

    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos({vp.x * 0.5f - 260.0f, 50.0f}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({520.0f, 440.0f}, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.97f);

    if (!ImGui::Begin("Search##search_panel", &g_search_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();

    // Snapshot before any ImGui calls so Begin/EndDisabled are always paired.
    // run_search() sets g_search_running = true mid-frame, so reading it twice
    // would call EndDisabled without a matching BeginDisabled and crash ImGui.
    bool was_running = g_search_running.load();
    if (was_running) ImGui::BeginDisabled();
    bool enter = ImGui::InputText("##sq", g_search_buf, sizeof(g_search_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    if (g_last_search_term != std::string(g_search_buf)) {
        g_highlighted_search_result = -1;
        g_last_search_term = std::string(g_search_buf);
        g_search_highlight = {};
    }
    ImGui::SameLine();
    if (ImGui::Button("Search") || enter) run_search();
    if (was_running) ImGui::EndDisabled();

    // Result count / loading indicator
    ImGui::SameLine();
    if (g_search_running) {
        // Animated ellipsis as a simple "working" indicator
        double t = ImGui::GetTime();
        int    dots = (int)(t * 2.0) % 4;
        static const char* spinner[] = {"Searching", "Searching.", "Searching..", "Searching..."};
        ImGui::TextDisabled("%s", spinner[dots]);
    } else {
        std::lock_guard<std::mutex> lk(g_search_results_mutex);
        ImGui::TextDisabled("%d result%s", (int)g_search_results.size(),
                            g_search_results.size() == 1 ? "" : "s");
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !g_search_running) {
        g_search_open = false;
        g_highlighted_search_result = -1;
        ImGui::End();
        return;
    }

    ImGui::Separator();
    ImGui::BeginChild("##search_results", {0.0f, 0.0f}, false);

    if (g_search_running) {
        // Centred "searching" message with animated dots — blocks result interaction
        ImGui::Spacing();
        ImGui::Spacing();
        float avail = ImGui::GetContentRegionAvail().x;
        double t  = ImGui::GetTime();
        int    dots = (int)(t * 2.0) % 4;
        static const char* msgs[] = {"◌", "◎", "◉", "●"};
        const char* icon = msgs[dots % 4];
        float tw = ImGui::CalcTextSize(icon).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(icon);
        const char* label = "Searching documents...";
        tw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw) * 0.5f);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    } else {
        // Take a snapshot of results under the lock so we don't hold it while rendering
        std::vector<SearchResult> results_snap;
        {
            std::lock_guard<std::mutex> lk(g_search_results_mutex);
            results_snap = g_search_results;
        }

        for (int i = 0; i < (int)results_snap.size(); ++i) {
            auto& hit = results_snap[i];
            ImGui::PushID(i);

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
                Page& pg = doc.pages[std::min(hit.page_idx, (int)doc.pages.size() - 1)];
                zoom_to_rect(pg.world_pos.x, pg.world_pos.y,
                             pg.world_pos.x + pg.world_w,
                             pg.world_pos.y + pg.world_h);
                // Navigate within panel if already open — never force-open it.
                if (g_input.panel_open())
                    g_input.open_panel(hit.doc_idx, hit.page_idx);
                g_highlighted_search_result = i;
                g_search_highlight = {hit.doc_idx, hit.page_idx, hit.hit_rects};
            }

            ImGui::Separator();
            ImGui::PopID();
        }
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

    if (ImGui::Button("+")) {
        g_text_tool = false; g_editing_box = -1;
        g_annot_tool = AnnotTool::None; g_ann_drawing = false;
        ImGui::OpenPopup("add_popup");
    }
    ImGui::SetItemTooltip("Add PDFs from folder, file, or URL");
    ImGui::SameLine();
    bool text_was_active = g_text_tool;
    if (text_was_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
    if (ImGui::Button("T")) {
        bool was_text  = g_text_tool;
        g_text_tool    = !g_text_tool;
        g_editing_box  = -1;
        if (!was_text) { g_annot_tool = AnnotTool::None; g_ann_drawing = false; }
    }
    if (text_was_active) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Insert a floating text box (T)");
    ImGui::SameLine();

    // Annotation tools
    ImGui::Spacing(); ImGui::SameLine();
    {
        bool pen_was    = (g_annot_tool == AnnotTool::Pen);
        bool hl_was     = (g_annot_tool == AnnotTool::Highlight);
        bool note_was   = (g_annot_tool == AnnotTool::Note);
        bool eraser_was = (g_annot_tool == AnnotTool::Eraser);

        if (pen_was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.08f, 0.08f, 1.0f));
        if (ImGui::Button("Pen")) {
            g_annot_tool = pen_was ? AnnotTool::None : AnnotTool::Pen;
            g_ann_drawing = false;
            if (!pen_was) { g_text_tool = false; g_editing_box = -1; }
        }
        if (pen_was) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Freehand pen: draw on any page (P)");
        ImGui::SameLine();

        if (hl_was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.55f, 0.0f, 1.0f));
        if (ImGui::Button("HL")) {
            g_annot_tool = hl_was ? AnnotTool::None : AnnotTool::Highlight;
            g_ann_drawing = false;
            if (!hl_was) { g_text_tool = false; g_editing_box = -1; }
        }
        if (hl_was) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Highlight text: drag to select (H)");
        ImGui::SameLine();

        if (note_was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.40f, 0.70f, 1.0f));
        if (ImGui::Button("Flag")) {
            g_annot_tool = note_was ? AnnotTool::None : AnnotTool::Note;
            g_ann_drawing = false;
            if (!note_was) { g_text_tool = false; g_editing_box = -1; }
        }
        if (note_was) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Place a note marker on any page (F)");
        ImGui::SameLine();

        if (eraser_was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        if (ImGui::Button("Erase")) {
            g_annot_tool = eraser_was ? AnnotTool::None : AnnotTool::Eraser;
            g_ann_drawing = false;
            if (!eraser_was) { g_text_tool = false; g_editing_box = -1; }
        }
        if (eraser_was) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Erase annotations by dragging over them");
    }

    // Tool-property strip — separator then context-sensitive controls, shown to the
    // right of all tool buttons when a tool with configurable properties is active.
    {
        bool show_pen_props  = (g_annot_tool == AnnotTool::Pen);
        bool show_hl_props   = (g_annot_tool == AnnotTool::Highlight);
        bool show_text_props = (g_text_tool || g_selected_box >= 0);
        if (show_pen_props || show_hl_props || show_text_props) {
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::TextDisabled("|");
            ImGui::SameLine(0.0f, 10.0f);
        }
        if (show_pen_props) {
            float pcol[3] = {g_pen_r, g_pen_g, g_pen_b};
            if (ImGui::ColorEdit3("##pencolor", pcol,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                { g_pen_r = pcol[0]; g_pen_g = pcol[1]; g_pen_b = pcol[2]; }
            ImGui::SetItemTooltip("Pen color");
        }
        if (show_hl_props) {
            // Mode: Box (rectangle drag — most reliable glyph capture) vs Freehand (swipe marker).
            if (ImGui::RadioButton("Box", g_hl_box_mode)) g_hl_box_mode = true;
            ImGui::SetItemTooltip("Drag a rectangle over text to capture it; over non-text leaves a yellow highlight");
            ImGui::SameLine();
            if (ImGui::RadioButton("Freehand", !g_hl_box_mode)) g_hl_box_mode = false;
            ImGui::SetItemTooltip("Swipe like a marker; captures text it covers, leaves a mark on non-text");
            // Colour applies to freehand marks only (box/text highlights are always yellow).
            if (!g_hl_box_mode) {
                ImGui::SameLine();
                float hcol[3] = {g_hl_r, g_hl_g, g_hl_b};
                if (ImGui::ColorEdit3("##hlcolor", hcol,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                    { g_hl_r = hcol[0]; g_hl_g = hcol[1]; g_hl_b = hcol[2]; }
                ImGui::SetItemTooltip("Freehand marker color");
            }
        }
        if (show_text_props) {
            float tcol[3] = {g_tbox_r, g_tbox_g, g_tbox_b};
            if (ImGui::ColorEdit3("##tboxcolor", tcol,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                g_tbox_r = tcol[0]; g_tbox_g = tcol[1]; g_tbox_b = tcol[2];
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
            ImGui::SetItemTooltip("Font size");
            ImGui::SameLine();
            if (ImGui::Checkbox("Scale", &g_tbox_zoom_scaled)) {
                for (auto& b : g_text_boxes)
                    if (b.id == g_selected_box) { b.zoom_scaled = g_tbox_zoom_scaled; break; }
            }
            ImGui::SetItemTooltip("Scale text with zoom so it stays proportional to the page");
        }
    }

    static bool open_url_modal = false;

    if (ImGui::BeginPopup("add_popup")) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f, 6.0f});

        if (ImGui::MenuItem("Add from folder...")) {
#ifdef __APPLE__
            const char* path = scholion_select_folder("Select PDF folder");
#else
            before_file_dialog();
            const char* path = tinyfd_selectFolderDialog("Select PDF folder", nullptr);
            after_file_dialog();
#endif
            if (path) load_pdfs_from_folder(path);
        }
        if (ImGui::MenuItem("Add from file...")) {
            const char* patterns[] = {"*.pdf", "*.PDF"};
#ifdef __APPLE__
            const char* result = scholion_open_file("Select PDF files", patterns, 2, 1);
#else
            before_file_dialog();
            const char* result = tinyfd_openFileDialog(
                "Select PDF files", nullptr,
                2, patterns, "PDF Documents", 1);
            after_file_dialog();
#endif
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

    s_toolbar_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
}

// ---------------------------------------------------------------------------

// Returns true when the app should keep redrawing at the interactive frame rate.
// When it returns false, the main loop blocks in glfwWaitEventsTimeout() for a
// long idle interval instead, dropping idle CPU/GPU to near zero (PureRef-style).
// Any GLFW input event, or a glfwPostEmptyEvent() from a worker thread, wakes the
// loop immediately, so responsiveness is unaffected. Conservative by design: it
// prefers a false-positive (a wasted redraw) over freezing a live animation.
static bool app_wants_animation() {
    if (rast_pending())                                    return true;  // tiles streaming / shimmer
    if (g_save_feedback_type != SaveFeedbackType::None)    return true;  // "Saved!" fade
    if (g_dl_state.load(std::memory_order_acquire) == 1)   return true;  // download spinner
    if (g_search_running.load(std::memory_order_acquire))  return true;  // search spinner
    if (g_startup_chooser)                                 return true;  // modal with hover states
    if (!g_group_remove_flashes.empty())                   return true;  // group-removal flash fading

    if (ImGui::IsAnyMouseDown())        return true;  // drag / pan / draw in progress
    if (ImGui::GetIO().WantTextInput)   return true;  // caret blink while editing text
    if (ImGui::IsAnyItemHovered())      return true;  // tooltip / widget hover animation
    return false;
}

int main(int argc, char* argv[]) {
    // Headless self-test: runs the save/load round-trip and exits. No window/GL needed.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0)
            return run_selftest();

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
#ifdef _WIN32
    // Register after glfwInit so glfwTerminate() is safe to call from the filter.
    SetUnhandledExceptionFilter(scholion_seh_filter);
#endif
#ifdef __APPLE__
    scholion_register_file_handler();  // re-register after glfwInit to override NSApp's default handler
#endif

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    g_window = glfwCreateWindow(1280, 800, "Scholion " SCHOLION_VERSION, nullptr, nullptr);
    GLFWwindow* window = g_window;
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);

    {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(window, &sx, &sy);
        g_content_scale = sx;
    }

#ifdef __APPLE__
    scholion_prewarm_dialogs();  // pre-init NSOpenPanel before first user action
#endif

#ifndef __APPLE__
    // Load all OpenGL 3.3 core function pointers via GLAD (Windows and Linux).
    // Must happen after a GL context is made current; before any GL calls.
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialise GLAD OpenGL loader\n");
        glfwTerminate();
        return 1;
    }
#endif

#ifdef _WIN32
    {
        HWND hwnd = glfwGetWin32Window(window);

        // Dark title bar — paints Windows 10/11 chrome to match the app's dark theme.
        BOOL use_dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));

        // Set the window icon from the embedded .exe resource (GLFW does not do this).
        // ID must match the IDI_ICON1 define in scholion.rc.
        HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL),
                                       MAKEINTRESOURCE(1),
                                       IMAGE_ICON, 0, 0,
                                       LR_DEFAULTSIZE | LR_SHARED);
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }
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

    // Bundled UI font (DejaVu Sans, embedded) replaces the default bitmap font: real Unicode
    // (ellipsis, dashes, ·, °, Greek) and crisp scaling for the Larger-UI option. Ranges are
    // static so the pointer stays valid until the atlas is built. See include/font_data.h.
    {
        static const ImWchar font_ranges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement (·, °, accented letters)
            0x2000, 0x206F,   // General Punctuation (… ellipsis, – — dashes, curly quotes)
            0x0370, 0x03FF,   // Greek
            0,
        };
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH  = true;
        ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(
            dejavu_sans_compressed_data, dejavu_sans_compressed_size, 15.0f, &cfg, font_ranges);
    }

    try { load_prefs(); } catch (...) {}   // guard against filesystem exceptions on second run
    apply_appearance();   // theme + UI scale (restores the persisted Larger-UI choice)
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

    // App logo (embedded RGBA) — used in the startup chooser and settings footer.
    {
        glGenTextures(1, &g_logo_tex);
        glBindTexture(GL_TEXTURE_2D, g_logo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LOGO_W, LOGO_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, LOGO_RGBA);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    PerformanceOverlay overlay;
    double last_frame_time = glfwGetTime();

    // Event-driven render pacing. `next_wait` is how long the loop blocks for the
    // next event; recomputed each frame (short when animating/interacting, long
    // when idle). `sampling_fps` records whether the previous frame was a
    // sustained-active frame, so the overlay ignores the huge delta of the first
    // frame waking from idle. (Windows uses PollEvents+Sleep and ignores these.)
    double next_wait    = 0.016;
    bool   sampling_fps = false;

    printf("Scholion " SCHOLION_VERSION "\n");
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
    g_panel_w = g_settings.panel_w;
    rast_init();

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
            if (!g_project_path.empty() && !g_documents.empty() && g_load_ok) {
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
        // Idle when nothing is animating: block up to 0.5 s so CPU/GPU drop to
        // near zero. A GLFW event or a worker's glfwPostEmptyEvent() returns
        // immediately, so this adds no latency. next_wait is set at the end of
        // the previous iteration by app_wants_animation().
        glfwWaitEventsTimeout(next_wait);
#endif

        // Per-frame timing for the performance overlay
        double now_t   = glfwGetTime();
        float  delta_t = static_cast<float>(now_t - last_frame_time);
        last_frame_time = now_t;
        overlay.update(delta_t, sampling_fps);

        if (g_input.consume_overlay_toggle()) overlay.toggle();

        g_input.update(window);
        bool rast_uploaded = drain_rast_results();
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
        stream_lod(rast_uploaded);

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
                    r.page_moves.push_back({g_drag_snap_page->id, g_drag_snap_pos});
                    push_undo(r);
                }
                g_drag_snap_page = nullptr;
            }
            g_was_page_dragging = (cur_dragged != nullptr);

            // Multi-page drag: snapshot all selected pages when drag begins
            if (!g_was_multi_drag && cur_multi) {
                g_multi_drag_snaps.clear();
                for (Page* p : selected_pages())
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
                        r.page_moves.push_back({snap.page->id, snap.old_pos});
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
        // ESC: tool deactivation always fires regardless of ImGui keyboard focus.
        // Only the editing-box branch stays gated so ImGui's InputText ESC handling
        // (revert buffer) can run first.
        if (!g_search_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (g_editing_box >= 0 && !ImGui::GetIO().WantCaptureKeyboard) {
                g_editing_box = -1;
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

        // Canvas layers — all on the BackgroundDrawList, so every ImGui window (menus,
        // panel, dialogs, tooltips) renders on top of them. Painter order (bottom → top):
        // locked labels → group frames → in-progress annotation → canvas strokes → note
        // badges → text boxes → cursor icon. Text boxes and the cursor icon are drawn last
        // (further below) so they stay above the other canvas marks while remaining under
        // the UI. (g_hovered_box, set in draw_canvas_text_boxes, is then one frame stale
        // for the double-click guard — harmless, since hover persists across a double-click.)
        draw_locked_doc_labels();
        update_group_drag_add();
        draw_page_groups();
        draw_group_remove_flashes();
        update_canvas_annotations();
        erase_canvas_strokes_at_cursor();
        draw_canvas_strokes();

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
                                if (g_input.selection().count(sel_doc->pages[pi].id)) {
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
        draw_canvas_text_boxes();  // above the other canvas marks, still below ImGui windows
        draw_cursor_tool_icon();   // topmost canvas layer, still below the ImGui windows
        draw_toolbar_ui();
        draw_context_menu();
        draw_canvas_context_menu();
        draw_settings_popup();
        draw_quit_dialog();
        draw_startup_chooser();
        draw_panel_ui();
        draw_panel_resize_handle();   // rendered after panel so it sits on top
        draw_panel_edge_tabs();       // visible only when panel is closed
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
        hints.draw_time      = (float)glfwGetTime();
        hints.tile_cache     = &g_tile_cache;
        hints.content_scale  = g_content_scale;
        if (g_search_highlight.active()) {
            hints.search_hit_doc   = g_search_highlight.doc_idx;
            hints.search_hit_page  = g_search_highlight.page_idx;
            hints.search_hit_rects = &g_search_highlight.rects;
        }
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

        // Standing save-status indicator (top-left, below the toolbar): always tells the user
        // whether their work is safe. Flashes "Saved!" on each save (manual or auto), then rests
        // at the last-saved clock time; shows "Editing…" when there are unsaved edits, "Unsaved"
        // before the first save, and a warning when autosave is suppressed (suspect load, #D).
        if (!g_documents.empty() || !g_project_path.empty()) {
            auto now = std::chrono::steady_clock::now();
            long since_ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - g_save_feedback_time).count();
            bool flashing = (g_save_feedback_type != SaveFeedbackType::None && since_ms < 1200);
            if (g_save_feedback_type != SaveFeedbackType::None && since_ms >= 1200)
                g_save_feedback_type = SaveFeedbackType::None;   // flash finished

            char   status[64];
            ImVec4 col;
            float  scale = 1.0f;
            if (flashing) {
                float a = 1.0f - (float)since_ms / 1200.0f;
                snprintf(status, sizeof(status), "Saved!");
                col = ImVec4(0.85f, 0.92f, 1.0f, 0.55f + 0.45f * a);
                scale = 1.2f;
            } else if (!g_load_ok) {
                snprintf(status, sizeof(status), "Autosave paused - save manually");
                col = ImVec4(0.95f, 0.74f, 0.34f, 0.90f);
            } else if (g_project_path.empty() || g_last_save_wall == 0) {
                snprintf(status, sizeof(status), "Unsaved");
                col = ImVec4(0.62f, 0.62f, 0.68f, 0.65f);
            } else if (g_dirty) {
                snprintf(status, sizeof(status), "Editing...");
                col = ImVec4(0.82f, 0.76f, 0.60f, 0.75f);
            } else {
                char clk[32]; std::time_t t = g_last_save_wall;
                std::strftime(clk, sizeof(clk), "%I:%M %p", std::localtime(&t));
                const char* c = (clk[0] == '0') ? clk + 1 : clk;   // trim leading-zero hour
                snprintf(status, sizeof(status), "Saved %s", c);
                col = ImVec4(0.62f, 0.62f, 0.68f, 0.65f);
            }

            ImGui::SetNextWindowPos({14.0f, 54.0f}, ImGuiCond_Always, {0.0f, 0.0f});
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f, 5.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
            ImGui::Begin("##save_status", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::SetWindowFontScale(scale);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(status);
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
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
        // g_load_ok gate: after a suspect load (corrupt/truncated/newer-format), don't let
        // autosave overwrite the on-disk file — the user must explicitly save first.
        if (!g_project_path.empty() && !g_documents.empty() && g_load_ok) {
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - g_last_save_time).count();
            if (elapsed >= 60) {
                // Serialize on main thread now (fast); disk write happens in background.
                g_save_feedback_type = SaveFeedbackType::Auto;
                g_save_feedback_time = std::chrono::steady_clock::now();
                start_autosave(g_project_path);
                g_last_save_time = now;
            }
        }

        glfwSwapBuffers(window);

        // Decide how long to block for the next event. ImGui interaction queries
        // (mouse/hover/text-input) are valid here — they reflect the frame just
        // rendered. When active, keep the interactive cap; when idle, block long.
        // sampling_fps carries this frame's active-ness to the next iteration so
        // the overlay ignores the oversized delta of the first frame after idle.
        {
            bool active   = app_wants_animation();
            next_wait     = active ? (g_settings.compat_mode ? 0.033 : 0.016) : 0.5;
            sampling_fps  = active;
        }

        // Exit the main loop if quit was confirmed
        if (g_quit_state == QuitState::Confirmed) {
            break;
        }
    }

    // Final save on clean exit (red button / Cmd+Q) so the last edits since the
    // previous autosave aren't lost. Only when a project path is set AND the load was clean
    // (g_load_ok) — a suspect-loaded file is never auto-overwritten, even on quit.
    if (!g_project_path.empty() && !g_documents.empty() && g_load_ok) {
        // Wait up to 3 s for any in-flight background autosave before writing
        // the exit save. The timeout prevents hanging if the background thread
        // is blocked on a slow disk or network-backed path (e.g. Dropbox).
        {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (autosave_running() &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (save_to_path(g_project_path)) {
            printf("Saved on exit: %s\n", g_project_path.c_str());
            // Clean quit → the working file is known-good; drop the crash-recovery backup.
            std::remove((g_project_path + ".bak").c_str());
        }
    }

    // Stop background workers before cleaning up shared state.
    if (g_dl_state.load() == 1) cancel_download();
    if (g_dl_thread.joinable()) g_dl_thread.join();
    if (g_search_thread.joinable()) g_search_thread.join();

    rast_cancel_all();   // drain queue so rast_shutdown() join returns immediately
    rast_shutdown();

    if (g_vignette_tex) { glDeleteTextures(1, &g_vignette_tex); g_vignette_tex = 0; }
    if (g_logo_tex)     { glDeleteTextures(1, &g_logo_tex);     g_logo_tex = 0; }
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
