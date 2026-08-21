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
#include "prefs.h"
#include "rast_pipeline.h"
#include "renderer.h"
#include "overlay.h"
#include "pdf_loader.h"
#include "save_feedback.h"
#include "texture_cache.h"
#include "undo.h"
#include "groups.h"
#include "settings.h"
#include "toolbar.h"
#include "actions.h"
#include "text_boxes.h"
#include "references_panel.h"
#include "search.h"
#include "side_panel.h"
#include "dialogs.h"
#include "input_glue.h"

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
bool  g_settings_open = false;   // non-static: read by extracted modules (groups.cpp)

static GLuint g_vignette_tex = 0;  // elliptical gradient texture, created once after GL init
GLuint g_logo_tex     = 0;  // app logo (embedded); non-static: read by settings.cpp

// --- Quit confirmation -------------------------------------------------------

// --- Panel width (shared between panel and resize handle) -------------------
float g_panel_w = 360.0f;   // also exposed as extern in app_state.h
float g_toolbar_bottom     = 51.0f; // measured each frame by draw_toolbar_ui()

// --- Canvas text boxes -------------------------------------------------------
// w/h are the box's on-screen size in pixels (0 = auto-size to text, used by
// bare-click and legacy boxes). Text wraps to w; h is a floor that grows to fit.
std::vector<CanvasTextBox> g_text_boxes;
int  g_next_box_id    = 0;
int  g_selected_box   = -1;  // id, -1 = none
int  g_editing_box    = -1;  // id, -1 = not editing
bool  g_text_tool      = false;   // non-static: shared tool state (canvas_annot.h)
// Definitions for the extern defaults declared in canvas_text_box.h (shared with the toolbar).
float g_tbox_r = 0.82f, g_tbox_g = 0.06f, g_tbox_b = 0.06f;  // default red, like the pen
float g_tbox_font_size = 16.0f;
bool  g_tbox_zoom_scaled = false;  // template for new boxes: scale text with zoom


bool g_just_created      = false; // set on create; consumed by edit-session tracking

// Entity clipboard for Cmd+C / Cmd+V duplication of selected boxes

// Undo session tracking — one record per editing session (text) and per
// selection session (style). Snapshots taken on begin, compared on end.
int   g_prev_editing_box  = -1;
bool  g_edit_was_new      = false;
int   g_prev_selected_box = -1;

// --- Undo stack -------------------------------------------------------------
// UndoRecord is defined in undo.h. push_undo declared there, defined here.


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
std::vector<Page*> selected_pages() {   // non-static: also used by groups.cpp
    std::vector<Page*> out;
    out.reserve(g_input.selection().size());
    for (uint64_t id : g_input.selection())
        if (Page* p = page_by_id(id)) out.push_back(p);
    return out;
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
//  3. Text-box group drag (text_boxes.cpp): g_box_dragging + g_box_drag_states +
//     g_page_drag_states, all private to that TU. Activated when a text box is pressed.
//     g_page_drag_states moves selected pages alongside boxes, bypassing m_multi_drag_active
//     to avoid the glfwPollEvents/ImGui frame-boundary race: GLFW RELEASE fires during
//     glfwPollEvents (synchronously); ImGui IsMouseClicked fires in the next frame.
//     Setting m_multi_drag_active from an ImGui handler can arrive after its
//     RELEASE and never clear — causing pages to stick to the cursor.
//
//  When a page-initiated drag (system 2) starts and text boxes are also selected,
//  the main loop detects the !g_was_multi_drag → cur_multi transition and calls
//  textboxes_begin_page_initiated_drag() so boxes follow the pages.
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
bool g_search_open    = false;   // non-static: read by extracted modules (groups.cpp)
static std::atomic<bool> g_search_running{false};
static char g_search_buf[256] = {};
std::vector<SearchResult> g_search_results;
static std::mutex                g_search_results_mutex;
static std::thread               g_search_thread;
int g_highlighted_search_result = -1;
static std::string g_last_search_term;

SearchHighlight g_search_highlight;

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

void zoom_to_rect(float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return;
    float vw   = g_canvas.get_viewport_width();
    float vh   = g_canvas.get_viewport_height();
    float zoom = std::min(vw / (x1 - x0), vh / (y1 - y0)) * 0.88f;
    g_canvas.set_zoom(zoom);
    g_canvas.set_offset({(x0 + x1) * 0.5f, (y0 + y1) * 0.5f});
}

void zoom_to_fit() {
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
Page* next_page_in_order(Page* from, int dir) {
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
bool relink_document(int doc_idx, const std::string& new_path) {
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
void load_pdfs_from_folder(const std::string& folder) {
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

void load_pdfs_from_selection(const char* selection) {
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


// Opens a fresh per-call PdfLoader to extract text — avoids racing with the
// background rast thread which owns the shared g_loaders instances.
std::string extract_page_text(const std::string& path, int page_idx) {
    PdfLoader tmp;
    Document  dummy;
    if (!tmp.load(path, dummy)) return {};
    return tmp.extract_text(page_idx);
}


static void framebuffer_size_callback(GLFWwindow* w, int width, int height) {
    glViewport(0, 0, width, height);
    int win_w, win_h;
    glfwGetWindowSize(w, &win_w, &win_h);
    g_canvas.set_viewport_size(static_cast<float>(win_w), static_cast<float>(win_h));
}


// --- URL modal --------------------------------------------------------------

void draw_url_modal() {
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

// Global canvas concerns lifted out of draw_canvas_text_boxes so the latter is text-box-only
// (see DEVLOG 2026-07-30 finding). Called in sequence from the render loop in the original
// order: reconcile-then-delete precedes the drag reconcile, which precedes the box render.
static void reconcile_selection_and_delete() {
    if (g_settings_open || g_search_open) return;
    ImGuiIO& io = ImGui::GetIO();
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

}



// Open the system file manager and highlight the given path.
// macOS: fork/execl with /usr/bin/open -R — no shell, immune to special chars.
// Windows: ShellExecuteW with explorer /select — equivalent native approach.
void reveal_in_file_manager(const std::string& path) {
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
    references_reset();   // clear the ref-note edit state (notes live on the highlights)
    g_groups.clear();
    g_next_group_id = 1;
    g_next_page_id  = 1;
    groups_reset();   // clear transient group editing/drag/flash state
    g_load_ok = true; g_dirty = false; g_last_save_wall = 0;   // fresh, unsaved state
    g_selected_box = g_editing_box = -1;
    g_prev_selected_box = g_prev_editing_box = -1;
    g_just_created = g_edit_was_new = false;
    textboxes_reset();   // clear transient box drag/creation/hover state
    clear_undo_stack();
    g_next_note_idx = 0;
    g_next_box_id   = 0;
    g_clip_valid    = false;   // don't carry a copied box across projects
    g_project_path.clear();
    update_window_title();
}

// --- Startup chooser (bare/native launch only) ------------------------------
// Shown when the app is opened with no document. Lets the user open a saved
// project, load PDF file(s), or load a whole folder of PDFs — or start blank.



// --- Quit confirmation dialog -----------------------------------------------


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


// --- Panel viewer -----------------------------------------------------------


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
    if (groups_animating())                                return true;  // group-removal flash fading

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
                // Text boxes follow a page-initiated multi-drag (state lives in text_boxes.cpp;
                // pages move via g_multi_drag_snaps above, so page_drag_states stays empty).
                textboxes_begin_page_initiated_drag();
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
        if (!ImGui::GetIO().WantCaptureMouse && textbox_hovered() < 0
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
        reconcile_selection_and_delete();   // sync selection + delete selected pages/boxes/docs
        update_item_drag_reconcile();        // continue/finish a unified page+box drag
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
