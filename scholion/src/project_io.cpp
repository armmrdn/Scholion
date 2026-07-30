#include "project_io.h"
#include "app_state.h"
// Vendored JSON library (BSD-2, header-only) — Phase 2 will migrate the hand-rolled parser
// onto it. Included now (unused) to verify it compiles in our toolchain. INT64 support so page
// ids parse exactly rather than through double.
#define PICOJSON_USE_INT64
#include "picojson.h"
#include "version.h"
#include "canvas_annot.h"
#include "save_feedback.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
extern "C" void        scholion_activate_app(void);
extern "C" const char* scholion_open_file(const char* title,
                                          const char** ext_patterns, int n_ext,
                                          int allow_multi);
extern "C" const char* scholion_save_file(const char* title, const char* default_name,
                                          const char** ext_patterns, int n_ext);
extern "C" const char* scholion_select_folder(const char* title);
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#endif

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>       // _commit, _fileno
#else
#include <unistd.h>   // fsync, fileno
#endif

#include "imgui.h"
#include "tinyfiledialogs.h"

// Forward declarations for main.cpp functions called during project load
void load_pdf(const std::string& path);
void clear_documents();

// --- Owned globals -----------------------------------------------------------

std::string              g_project_path;
std::chrono::steady_clock::time_point g_last_save_time;
std::vector<std::string> g_recents;

bool        g_load_ok        = true;
bool        g_dirty          = false;
std::time_t g_last_save_wall = 0;

// On-disk schema version. Bump ONLY when the layout changes incompatibly. Files without a
// "format" field are treated as this legacy baseline. Kept separate from the app version.
static constexpr int SCHOLION_FORMAT = 1;

// 64-bit FNV-1a over a byte range — a cheap integrity hash of the file body (not the PDFs).
static uint64_t fnv1a64(const char* data, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)data[i]; h *= 1099511628211ULL; }
    return h;
}
static std::string hex64(uint64_t h) {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf, 16);
}

static constexpr int RECENTS_MAX = 10;
static std::atomic<bool> g_autosave_running{false};

bool autosave_running() { return g_autosave_running.load(); }

// --- JSON string helpers -----------------------------------------------------

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

// --- Recent files ------------------------------------------------------------

static std::string recents_file_path() {
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
        return dir + "\\recents";
    }
    return "";
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.scholion_recents" : "";
#endif
}

void save_recents() {
    std::string p = recents_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    for (const auto& r : g_recents) fprintf(f, "%s\n", r.c_str());
    fclose(f);
}

void load_recents() {
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

void add_to_recents(const std::string& path) {
    g_recents.erase(std::remove(g_recents.begin(), g_recents.end(), path), g_recents.end());
    g_recents.insert(g_recents.begin(), path);
    if ((int)g_recents.size() > RECENTS_MAX) g_recents.resize(RECENTS_MAX);
    save_recents();
}

// --- Application preferences -------------------------------------------------

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

void save_prefs() {
    std::string p = prefs_file_path();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "dark_mode=%d\n",   g_settings.dark_mode   ? 1 : 0);
    fprintf(f, "grid_mode=%d\n",   (int)g_settings.grid_mode);
    fprintf(f, "compat_mode=%d\n", g_settings.compat_mode ? 1 : 0);
    fprintf(f, "vignette_on=%d\n", g_settings.vignette_on ? 1 : 0);
    fprintf(f, "panel_w=%.1f\n",   g_settings.panel_w);
    fprintf(f, "large_ui=%d\n",    g_settings.large_ui   ? 1 : 0);
    fclose(f);
}

void load_prefs() {
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
        else if (!strcmp(key, "large_ui"))    g_settings.large_ui    = ival;
    }
    fclose(f);
}

// Larger-UI scale factor. Applied to fonts (io.FontGlobalScale) and widget metrics
// (ImGuiStyle::ScaleAllSizes). 1.0 = normal.
static constexpr float SCHOLION_UI_SCALE_LARGE = 1.4f;

void apply_theme(bool dark) {
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

// Apply the theme AND the UI scale together. apply_theme resets all size fields to
// defaults (via StyleColorsDark/Light), so ScaleAllSizes must run right after it to
// avoid compounding across calls. Use this everywhere theme or scale changes.
void apply_appearance() {
    apply_theme(g_settings.dark_mode);
    float s = g_settings.large_ui ? SCHOLION_UI_SCALE_LARGE : 1.0f;
    ImGui::GetIO().FontGlobalScale = s;
    if (s != 1.0f) ImGui::GetStyle().ScaleAllSizes(s);
}

// --- Window title ------------------------------------------------------------

void update_window_title() {
    if (!g_window) return;
    if (g_project_path.empty()) {
        glfwSetWindowTitle(g_window, "Scholion " SCHOLION_VERSION);
    } else {
        std::string name = std::filesystem::path(g_project_path).stem().string();
        glfwSetWindowTitle(g_window, ("Scholion " SCHOLION_VERSION " \xe2\x80\x94 " + name).c_str());
    }
}

// --- Serialization -----------------------------------------------------------

static std::string build_project_json() {
    std::string out;
    out.reserve(128 * 1024);
    char b[512];

    Vec2 offset = g_canvas.get_offset();
    out += "{\n";
    // Header: schema version, writing-app provenance, and a placeholder integrity hash
    // (filled in at the end, computed over the whole document body).
    snprintf(b, sizeof(b), "  \"format\": %d,\n  \"app\": \"%s\",\n  \"hash\": \"0000000000000000\",\n",
             SCHOLION_FORMAT, SCHOLION_VERSION);
    out += b;
    snprintf(b, sizeof(b), "  \"viewport\": { \"x\": %.4f, \"y\": %.4f, \"zoom\": %.6f },\n",
             offset.x, offset.y, g_canvas.get_zoom());
    out += b;
    snprintf(b, sizeof(b), "  \"note_idx\": %d,\n", g_next_note_idx);
    out += b;

    out += "  \"documents\": [\n";
    for (int di = 0; di < (int)g_documents.size(); ++di) {
        const Document& doc = g_documents[di];
        out += "    {\n";
        out += "      \"path\": \""; out += doc.path; out += "\",\n";
        // Portable link: path relative to the .scholion's folder (forward slashes). Loader
        // tries this first so a moved/shared project keeps its PDFs; absolute "path" is the
        // fallback. Empty project path → rel == absolute.
        {
            std::string rel = doc.path;
            std::error_code ec;
            if (!g_project_path.empty()) {
                auto r = std::filesystem::relative(
                    doc.path, std::filesystem::path(g_project_path).parent_path(), ec);
                if (!ec && !r.empty()) rel = r.generic_string();
            }
            out += "      \"rel\": \""; out += rel; out += "\",\n";
        }
        snprintf(b, sizeof(b), "      \"stack_origin\": [%.4f, %.4f],\n",
                 doc.stack_origin.x, doc.stack_origin.y);
        out += b;
        out += "      \"pages\": [\n";
        for (int pi = 0; pi < (int)doc.pages.size(); ++pi) {
            const Page& page = doc.pages[pi];
            snprintf(b, sizeof(b),
                     "        { \"index\": %d, \"x\": %.4f, \"y\": %.4f, \"w\": %.2f, \"h\": %.2f, \"rot\": %d, \"grp\": %d, \"id\": %llu }%s\n",
                     page.page_index, page.world_pos.x, page.world_pos.y,
                     page.world_w, page.world_h, page.rotation, page.group_id,
                     (unsigned long long)page.id,
                     pi + 1 < (int)doc.pages.size() ? "," : "");
            out += b;
        }
        out += "      ]\n";
        snprintf(b, sizeof(b), "    }%s\n", di + 1 < (int)g_documents.size() ? "," : "");
        out += b;
    }
    out += "  ],\n";

    out += "  \"text_boxes\": [\n";
    for (int i = 0; i < (int)g_text_boxes.size(); ++i) {
        const auto& box = g_text_boxes[i];
        snprintf(b, sizeof(b),
                 "    { \"id\": %d, \"x\": %.4f, \"y\": %.4f, \"r\": %.3f, \"g\": %.3f, \"b\": %.3f, \"fs\": %.1f, \"w\": %.2f, \"h\": %.2f, \"zs\": %d, \"text\": \"",
                 box.id, box.world_pos.x, box.world_pos.y, box.r, box.g, box.b, box.font_size,
                 box.w, box.h, box.zoom_scaled ? 1 : 0);
        out += b;
        out += json_escape(box.text);
        snprintf(b, sizeof(b), "\" }%s\n", i + 1 < (int)g_text_boxes.size() ? "," : "");
        out += b;
    }
    out += "  ],\n";

    out += "  \"annots\": [\n";
    bool first_annot = true;
    auto sep = [&]{ if (!first_annot) out += ",\n"; first_annot = false; };

    for (int di = 0; di < (int)g_documents.size(); ++di) {
        for (int pi = 0; pi < (int)g_documents[di].pages.size(); ++pi) {
            const PageAnnotations& an = g_documents[di].pages[pi].annots;
            for (const auto& hl : an.highlights) {
                sep();
                if (hl.text.empty()) {
                    snprintf(b, sizeof(b),
                             "    { \"doc\": %d, \"page\": %d, \"hl\": [%.6f, %.6f, %.6f, %.6f] }",
                             di, pi, hl.x0, hl.y0, hl.x1, hl.y1);
                    out += b;
                } else {
                    snprintf(b, sizeof(b),
                             "    { \"doc\": %d, \"page\": %d, \"hl\": [%.6f, %.6f, %.6f, %.6f], \"ht\": \"",
                             di, pi, hl.x0, hl.y0, hl.x1, hl.y1);
                    out += b;
                    out += json_escape(hl.text.c_str());
                    out += "\"";
                    if (!hl.note.empty()) {   // "rn" = research note attached to this reference
                        out += ", \"rn\": \"";
                        out += json_escape(hl.note.c_str());
                        out += "\"";
                    }
                    out += " }";
                }
            }
            for (const auto& note : an.notes) {
                sep();
                snprintf(b, sizeof(b),
                         "    { \"doc\": %d, \"page\": %d, \"note\": \"%s\" }",
                         di, pi, note.label.c_str());
                out += b;
            }
            for (const auto& stroke : an.strokes) {
                sep();
                snprintf(b, sizeof(b),
                         "    { \"doc\": %d, \"page\": %d, \"sr\": %.5f, \"sg\": %.5f, \"sb\": %.5f, \"sw\": %.3f, \"sa\": %.4f }",
                         di, pi, stroke.r, stroke.g, stroke.b, stroke.width, stroke.alpha);
                out += b;
                for (const auto& pt : stroke.pts) {
                    snprintf(b, sizeof(b), ",\n    { \"p\": [%.6f, %.6f] }", pt.x, pt.y);
                    out += b;
                }
            }
        }
    }
    // Canvas (world-space) pen strokes: stored in the annots array with doc=-1 as the
    // sentinel (points are world coords, not page-normalized).
    for (const auto& stroke : g_canvas_strokes) {
        sep();
        snprintf(b, sizeof(b),
                 "    { \"doc\": -1, \"page\": -1, \"sr\": %.5f, \"sg\": %.5f, \"sb\": %.5f, \"sw\": %.3f, \"sa\": %.4f }",
                 stroke.r, stroke.g, stroke.b, stroke.width, stroke.alpha);
        out += b;
        for (const auto& pt : stroke.pts) {
            snprintf(b, sizeof(b), ",\n    { \"p\": [%.3f, %.3f] }", pt.x, pt.y);
            out += b;
        }
    }
    if (!first_annot) out += "\n";
    out += "  ],\n";
    // Reference notes now live on each highlight ("rn" field above) — no separate section.
    // Pre-v1.4 files still carry a "ref_notes" array, which the loader migrates onto the
    // matching highlights.

    // Page groups (ad-hoc cross-document clusters). Only groups with live members
    // are written — an empty group carries no information.
    out += "  \"groups\": [\n";
    {
        std::vector<const PageGroup*> live;
        for (const auto& grp : g_groups) {
            bool has = false;
            for (const auto& doc : g_documents) {
                for (const auto& pg : doc.pages) if (pg.group_id == grp.id) { has = true; break; }
                if (has) break;
            }
            if (has) live.push_back(&grp);
        }
        for (size_t i = 0; i < live.size(); ++i) {
            const PageGroup* grp = live[i];
            snprintf(b, sizeof(b),
                     "    { \"group\": %d, \"r\": %.3f, \"g\": %.3f, \"b\": %.3f, \"name\": \"",
                     grp->id, grp->col_r, grp->col_g, grp->col_b);
            out += b;
            out += json_escape(grp->name.c_str());
            snprintf(b, sizeof(b), "\" }%s\n", i + 1 < live.size() ? "," : "");
            out += b;
        }
    }
    out += "  ]\n}\n";
    // Integrity hash: FNV-1a over the whole document (with the placeholder still in place),
    // then patch the placeholder. The loader blanks the field back to zeros and recomputes.
    {
        uint64_t h = fnv1a64(out.data(), out.size());
        size_t hp = out.find("\"hash\": \"");
        if (hp != std::string::npos) out.replace(hp + 9, 16, hex64(h));
    }
    return out;
}

static bool write_json_file(const std::string& json, const std::string& path) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { fprintf(stderr, "save: cannot open %s\n", tmp.c_str()); return false; }
    bool ok = fwrite(json.data(), 1, json.size(), f) == json.size();
    if (ok) {
        // Flush to physical media before the rename so a power-loss can't leave a torn file.
        fflush(f);
#ifdef _WIN32
        _commit(_fileno(f));
#else
        fsync(fileno(f));
#endif
    }
    fclose(f);
    if (!ok) { remove(tmp.c_str()); return false; }
    // One-deep backup: rotate the current good file to ".bak" before swapping in the new one,
    // so the previous saved state is recoverable (crash / bad save). Removed on clean quit.
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::string bak = path + ".bak";
        remove(bak.c_str());
        rename(path.c_str(), bak.c_str());   // best-effort; if it fails we still save below
    }
#ifdef _WIN32
    _unlink(path.c_str());
#endif
    if (rename(tmp.c_str(), path.c_str()) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

void start_autosave(const std::string& path) {
    if (g_autosave_running.exchange(true)) return;
    std::string json = build_project_json();
    // The snapshot was taken on the main thread → mark the in-memory state clean now.
    g_dirty = false;
    g_last_save_wall = std::time(nullptr);
    std::thread([json = std::move(json), path]() mutable {
        write_json_file(json, path);
        g_autosave_running.store(false);
    }).detach();
}

bool save_to_path(const std::string& path) {
    std::string json = build_project_json();
    if (!write_json_file(json, path)) {
        fprintf(stderr, "save_project: cannot write %s\n", path.c_str());
        return false;
    }
    if (g_debug) {
        int s_hl = 0, s_note = 0, s_stroke = 0;
        for (const auto& doc : g_documents)
            for (const auto& pg : doc.pages) {
                s_hl     += (int)pg.annots.highlights.size();
                s_note   += (int)pg.annots.notes.size();
                s_stroke += (int)pg.annots.strokes.size();
            }
        printf("Project saved: %s — %d text boxes, %d highlights, %d notes, %d strokes\n",
               path.c_str(), (int)g_text_boxes.size(), s_hl, s_note, s_stroke);
    }
    if (g_save_feedback_type == SaveFeedbackType::None)
        g_save_feedback_type = SaveFeedbackType::Manual;
    g_save_feedback_time = std::chrono::steady_clock::now();
    g_dirty = false;
    g_last_save_wall = std::time(nullptr);
    g_load_ok = true;   // an explicit save makes this file the current good state again
    return true;
}

// --- File dialog helpers -----------------------------------------------------

void before_file_dialog() {
#ifdef __APPLE__
    scholion_activate_app();
#elif defined(_WIN32)
    if (g_window) EnableWindow(glfwGetWin32Window(g_window), FALSE);
#endif
}

void after_file_dialog() {
#ifdef _WIN32
    if (g_window) {
        HWND hwnd = glfwGetWin32Window(g_window);
        EnableWindow(hwnd, TRUE);
        SetForegroundWindow(hwnd);
    }
#endif
}

// --- Save project ------------------------------------------------------------

void save_project() {
    before_file_dialog();
    const char* filter_patterns[] = {"*.scholion"};
#ifdef __APPLE__
    const char* picked = scholion_save_file("Save Project", "project.scholion",
                                             filter_patterns, 1);
#else
    const char* picked = tinyfd_saveFileDialog(
        "Save Project", "project.scholion", 1, filter_patterns, "Scholion Project");
    after_file_dialog();
#endif
    if (!picked) return;

    if (save_to_path(picked)) {
        g_project_path   = picked;
        g_last_save_time = std::chrono::steady_clock::now();
        add_to_recents(picked);
        update_window_title();
        printf("Project saved: %s\n", picked);
    }
}

void save_project_current() {
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

// --- Load project ------------------------------------------------------------

void load_project_from_path(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { fprintf(stderr, "load_project: cannot open %s\n", path.c_str()); return; }

    struct SavedPage { int idx; float x, y; float w = 0.0f, h = 0.0f; int rot = 0; int grp = 0; uint64_t id = 0; };
    struct SavedDoc  { std::string path; std::string rel; float sox, soy; std::vector<SavedPage> pages; };
    struct SavedGroup { int id; float r, g, b; std::string name; };
    std::vector<SavedGroup> saved_groups;

    struct SavedHL     { int doc, page; AnnotHighlight hl; };
    struct SavedNote   { int doc, page; std::string label; };
    struct SavedStroke { int doc, page; AnnotStroke stroke; };

    std::vector<SavedDoc>      saved;
    std::vector<SavedHL>       saved_hls;
    std::vector<SavedNote>     saved_notes;
    std::vector<SavedStroke>   saved_strokes;
    std::vector<CanvasTextBox> saved_boxes;
    std::vector<RefNote>       saved_ref_notes;
    std::vector<AnnotStroke>   saved_canvas_strokes;   // doc=-1 sentinel strokes

    float vx = 0.0f, vy = 0.0f, vz = 0.6f;

    enum class Section { Docs, TextBoxes, Annots, RefNotes, Groups, Other };

    int   stroke_doc = -1, stroke_page = -1;
    AnnotStroke building_stroke;
    bool  building = false;

    auto flush_stroke = [&]() {
        if (building && !building_stroke.pts.empty()) {
            if (stroke_doc == -1)   // canvas (world-space) stroke sentinel
                saved_canvas_strokes.push_back(std::move(building_stroke));
            else
                saved_strokes.push_back({stroke_doc, stroke_page, std::move(building_stroke)});
        }
        building_stroke = {};
        building = false;
    };

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

    const size_t npos = std::string::npos;

    // --- Integrity + version header (batch #3) --------------------------------
    g_load_ok = true;
    int file_format = 1;                 // legacy files (no "format") = baseline
    { size_t fp = content.find("\"format\":"); int fv;
      if (fp != npos && sscanf(content.c_str() + fp, "\"format\": %d", &fv) == 1) file_format = fv; }
    { char av[64] = "";
      size_t ap = content.find("\"app\":");
      if (ap != npos) sscanf(content.c_str() + ap, "\"app\": \"%63[^\"]\"", av);
      printf("load: %s (format %d, app %s)\n", path.c_str(), file_format, av[0] ? av : "legacy"); }
    bool have_hash = false, hash_ok = false;
    { size_t hp = content.find("\"hash\": \"");
      if (hp != npos && hp + 9 + 16 <= content.size()) {
          have_hash = true;
          std::string stored = content.substr(hp + 9, 16);
          std::string check  = content;
          check.replace(hp + 9, 16, "0000000000000000");
          hash_ok = (hex64(fnv1a64(check.data(), check.size())) == stored);
      }
    }
    if (file_format > SCHOLION_FORMAT) {
        g_load_ok = false;
        fprintf(stderr, "load: file format %d is newer than this build (%d) — autosave suppressed\n",
                file_format, SCHOLION_FORMAT);
    }
    if (have_hash && !hash_ok) {
        g_load_ok = false;
        fprintf(stderr, "load: integrity hash mismatch — file may be corrupt; autosave suppressed\n");
    }

    size_t doc_key = content.find("\"documents\":");
    // Parse with a real JSON library (retires the substring/sscanf scanner). Fills the same
    // intermediate structures the apply half below consumes; the on-disk layout is unchanged
    // so v1.0–v1.4 files load identically. (The hash/format/heuristic checks above still run
    // on the raw text, since the hash is computed over the file bytes.)
    bool note_idx_loaded = false;
    picojson::value root_v;
    {
        std::string perr = picojson::parse(root_v, content);
        if (!perr.empty() || !root_v.is<picojson::object>()) {
            g_load_ok = false;
            fprintf(stderr, "load: JSON parse error: %s\n", perr.empty() ? "root is not an object" : perr.c_str());
        }
    }
    if (root_v.is<picojson::object>()) {
        const picojson::object& root = root_v.get<picojson::object>();
        auto num = [](const picojson::object& o, const char* k, double def) -> double {
            auto it = o.find(k);
            if (it == o.end()) return def;
            if (it->second.is<int64_t>()) return (double)it->second.get<int64_t>();
            if (it->second.is<double>())  return it->second.get<double>();
            return def;
        };
        auto u64 = [](const picojson::object& o, const char* k) -> uint64_t {
            auto it = o.find(k);
            if (it == o.end()) return 0;
            if (it->second.is<int64_t>()) return (uint64_t)it->second.get<int64_t>();
            if (it->second.is<double>())  return (uint64_t)it->second.get<double>();
            return 0;
        };
        auto sstr = [](const picojson::object& o, const char* k) -> std::string {
            auto it = o.find(k);
            return (it != o.end() && it->second.is<std::string>()) ? it->second.get<std::string>() : std::string();
        };
        auto obj_at = [](const picojson::value& v) -> const picojson::object* {
            return v.is<picojson::object>() ? &v.get<picojson::object>() : nullptr;
        };
        auto arr_at = [](const picojson::object& o, const char* k) -> const picojson::array* {
            auto it = o.find(k);
            return (it != o.end() && it->second.is<picojson::array>()) ? &it->second.get<picojson::array>() : nullptr;
        };
        auto anum = [](const picojson::array& a, size_t i) -> double {
            if (i >= a.size()) return 0.0;
            if (a[i].is<int64_t>()) return (double)a[i].get<int64_t>();
            if (a[i].is<double>())  return a[i].get<double>();
            return 0.0;
        };

        // Viewport + note counter
        if (auto it = root.find("viewport"); it != root.end() && it->second.is<picojson::object>()) {
            const auto& vp = it->second.get<picojson::object>();
            vx = (float)num(vp, "x", 0.0); vy = (float)num(vp, "y", 0.0); vz = (float)num(vp, "zoom", 0.6);
        }
        if (auto it = root.find("note_idx");
            it != root.end() && (it->second.is<int64_t>() || it->second.is<double>())) {
            g_next_note_idx = (int)num(root, "note_idx", 0);
            note_idx_loaded = true;
        }

        // Documents + pages
        if (const picojson::array* docs = arr_at(root, "documents")) {
            for (const auto& dv : *docs) {
                const picojson::object* d = obj_at(dv); if (!d) continue;
                SavedDoc sd;
                sd.path = sstr(*d, "path");
                sd.rel  = sstr(*d, "rel");
                sd.sox = sd.soy = 0.0f;
                if (const picojson::array* so = arr_at(*d, "stack_origin"); so && so->size() == 2) {
                    sd.sox = (float)anum(*so, 0); sd.soy = (float)anum(*so, 1);
                }
                if (const picojson::array* pgs = arr_at(*d, "pages")) {
                    for (const auto& pv : *pgs) {
                        const picojson::object* p = obj_at(pv); if (!p) continue;
                        SavedPage sp;
                        sp.idx = (int)num(*p, "index", 0);
                        sp.x = (float)num(*p, "x", 0.0); sp.y = (float)num(*p, "y", 0.0);
                        sp.w = (float)num(*p, "w", 0.0); sp.h = (float)num(*p, "h", 0.0);
                        sp.rot = (int)num(*p, "rot", 0); sp.grp = (int)num(*p, "grp", 0);
                        sp.id  = u64(*p, "id");
                        sd.pages.push_back(sp);
                    }
                }
                saved.push_back(std::move(sd));
            }
        }

        // Text boxes
        if (const picojson::array* tbs = arr_at(root, "text_boxes")) {
            for (const auto& tv : *tbs) {
                const picojson::object* t = obj_at(tv); if (!t) continue;
                CanvasTextBox tb;
                tb.id = (int)num(*t, "id", 0);
                tb.world_pos = { (float)num(*t, "x", 0.0), (float)num(*t, "y", 0.0) };
                tb.r = (float)num(*t, "r", 0.82); tb.g = (float)num(*t, "g", 0.06); tb.b = (float)num(*t, "b", 0.06);
                tb.font_size = (float)num(*t, "fs", 16.0);
                tb.w = (float)num(*t, "w", 0.0); tb.h = (float)num(*t, "h", 0.0);
                tb.zoom_scaled = (num(*t, "zs", 0.0) != 0.0);   // serialized as 0/1 integer
                std::string txt = sstr(*t, "text");
                strncpy(tb.text, txt.c_str(), sizeof(tb.text) - 1);
                tb.text[sizeof(tb.text) - 1] = '\0';
                saved_boxes.push_back(tb);
            }
        }

        // Annotations — a flat array in order: highlights, note flags, and strokes (a stroke
        // header object followed by its "p" point objects). Mirror the streaming stroke build.
        if (const picojson::array* an = arr_at(root, "annots")) {
            for (const auto& av : *an) {
                const picojson::object* o = obj_at(av); if (!o) continue;
                if (const picojson::array* hl = arr_at(*o, "hl"); hl && hl->size() == 4) {
                    flush_stroke();
                    AnnotHighlight h;
                    h.x0 = (float)anum(*hl, 0); h.y0 = (float)anum(*hl, 1);
                    h.x1 = (float)anum(*hl, 2); h.y1 = (float)anum(*hl, 3);
                    h.text = sstr(*o, "ht"); h.note = sstr(*o, "rn");
                    saved_hls.push_back({ (int)num(*o, "doc", 0), (int)num(*o, "page", 0), std::move(h) });
                } else if (o->count("note")) {
                    flush_stroke();
                    saved_notes.push_back({ (int)num(*o, "doc", 0), (int)num(*o, "page", 0), sstr(*o, "note") });
                } else if (o->count("sr")) {
                    flush_stroke();
                    stroke_doc = (int)num(*o, "doc", 0); stroke_page = (int)num(*o, "page", 0);
                    building_stroke = {};
                    building_stroke.r = (float)num(*o, "sr", 0.0);
                    building_stroke.g = (float)num(*o, "sg", 0.0);
                    building_stroke.b = (float)num(*o, "sb", 0.0);
                    building_stroke.width = (float)num(*o, "sw", 1.2);
                    building_stroke.alpha = (float)num(*o, "sa", 0.88);
                    building = true;
                } else if (const picojson::array* pp = arr_at(*o, "p"); pp && pp->size() == 2) {
                    if (building) building_stroke.pts.push_back({ (float)anum(*pp, 0), (float)anum(*pp, 1) });
                }
            }
            flush_stroke();
        }

        // Legacy positional reference notes (pre-v1.4; migrated onto highlights in the apply half)
        if (const picojson::array* rns = arr_at(root, "ref_notes")) {
            for (const auto& rv : *rns) {
                const picojson::object* r = obj_at(rv); if (!r) continue;
                RefNote rn; rn.after_idx = (int)num(*r, "after", 0); rn.text = sstr(*r, "text");
                saved_ref_notes.push_back(std::move(rn));
            }
        }

        // Page groups
        if (const picojson::array* grs = arr_at(root, "groups")) {
            for (const auto& gv : *grs) {
                const picojson::object* g = obj_at(gv); if (!g) continue;
                SavedGroup sg;
                sg.id = (int)num(*g, "group", 0);
                sg.r = (float)num(*g, "r", 0.63); sg.g = (float)num(*g, "g", 0.32); sg.b = (float)num(*g, "b", 0.75);
                sg.name = sstr(*g, "name");
                saved_groups.push_back(std::move(sg));
            }
        }
    }

    // Heuristic for legacy files (no integrity hash): a documents section that clearly had
    // entries but parsed to nothing signals truncation/corruption → suspect load.
    if (!have_hash && doc_key != npos &&
        content.find("\"path\":", doc_key) != npos && saved.empty()) {
        g_load_ok = false;
        fprintf(stderr, "load: documents present but none parsed — file may be truncated; autosave suppressed\n");
    }

    clear_documents();
    g_text_boxes.clear();
    g_canvas_strokes.clear();
    g_groups.clear();
    g_next_group_id = 1;
    g_next_page_id  = 1;   // load_pdf assigns fresh ids; saved ids are restored below
    g_selected_box = g_editing_box = -1;
    g_prev_selected_box = g_prev_editing_box = -1;
    g_just_created = g_edit_was_new = false;
    namespace fs = std::filesystem;

    g_text_boxes = std::move(saved_boxes);
    for (const auto& tb : g_text_boxes)
        g_next_box_id = std::max(g_next_box_id, tb.id + 1);
    g_canvas_strokes = std::move(saved_canvas_strokes);

    constexpr float PLACEHOLDER_PAGE_W = 612.0f, PLACEHOLDER_PAGE_H = 792.0f;

    std::vector<int> doc_map(saved.size(), -1);
    fs::path proj_dir = fs::path(path).parent_path();
    for (size_t si = 0; si < saved.size(); ++si) {
        const auto& sd = saved[si];
        // Resolve the PDF: (1) rel-to-project dir, (2) absolute "path", (3) basename next to
        // the .scholion; else keep the placeholder. Makes moved/shared projects keep links.
        std::string resolved;
        std::error_code rec;
        if (!sd.rel.empty()) {
            fs::path r = (proj_dir / sd.rel).lexically_normal();
            if (fs::exists(r, rec)) resolved = r.string();
        }
        if (resolved.empty() && fs::exists(sd.path, rec)) resolved = sd.path;
        if (resolved.empty()) {
            fs::path bn = proj_dir / fs::path(sd.path).filename();
            if (fs::exists(bn, rec)) resolved = bn.string();
        }
        if (resolved.empty()) {
            fprintf(stderr, "load_project: PDF not found, keeping placeholder: %s\n", sd.path.c_str());
            Document d;
            d.path         = sd.path;
            d.missing      = true;
            d.stack_origin = {sd.sox, sd.soy};
            auto& cv = DOC_PALETTE[g_documents.size() % PALETTE_SIZE];
            d.hue_r = cv[0]; d.hue_g = cv[1]; d.hue_b = cv[2];
            for (const auto& sp : sd.pages) {
                Page p;
                p.page_index = sp.idx;
                p.world_pos  = {sp.x, sp.y};
                p.world_w    = sp.w > 0.0f ? sp.w : PLACEHOLDER_PAGE_W;
                p.world_h    = sp.h > 0.0f ? sp.h : PLACEHOLDER_PAGE_H;
                p.rotation   = sp.rot;
                p.group_id   = sp.grp;
                p.id         = sp.id ? sp.id : g_next_page_id++;   // placeholder path (no load_pdf)
                d.pages.push_back(p);
            }
            doc_map[si] = (int)g_documents.size();
            g_documents.push_back(std::move(d));
            g_loaders.push_back(nullptr);
            continue;
        }
        printf("Loading: %s\n", resolved.c_str());
        size_t before = g_documents.size();
        load_pdf(resolved);
        if (g_documents.size() == before) continue;

        doc_map[si] = (int)g_documents.size() - 1;
        Document& doc = g_documents.back();
        doc.stack_origin = {sd.sox, sd.soy};
        for (const auto& sp : sd.pages) {
            for (auto& pg : doc.pages) {
                if (pg.page_index == sp.idx) {
                    pg.world_pos = {sp.x, sp.y};
                    pg.group_id  = sp.grp;
                    if (sp.id) pg.id = sp.id;   // restore saved id over load_pdf's fresh one
                    if (sp.rot != 0) {
                        pg.rotation = sp.rot;
                        pg.world_w  = sp.w;
                        pg.world_h  = sp.h;
                    }
                    break;
                }
            }
        }
    }

    // Normalize page ids: assign a fresh id to any page still unassigned (legacy files),
    // and advance the counter past every id so future imports can't collide.
    for (auto& doc : g_documents)
        for (auto& pg : doc.pages) {
            if (pg.id == 0) pg.id = g_next_page_id++;
            if (pg.id >= g_next_page_id) g_next_page_id = pg.id + 1;
        }

    // Rebuild the group table. Only keep groups that at least one loaded page
    // references, so a group whose pages all went missing doesn't linger.
    {
        std::unordered_set<int> live_gids;
        for (const auto& doc : g_documents)
            for (const auto& pg : doc.pages)
                if (pg.group_id != 0) live_gids.insert(pg.group_id);
        for (const auto& sg : saved_groups) {
            if (!live_gids.count(sg.id)) continue;
            PageGroup g; g.id = sg.id; g.name = sg.name;
            g.col_r = sg.r; g.col_g = sg.g; g.col_b = sg.b;
            g_groups.push_back(g);
        }
        // Any grouped page whose group row was missing gets a synthesized default row.
        for (int gid : live_gids) {
            bool have = false;
            for (const auto& g : g_groups) if (g.id == gid) { have = true; break; }
            if (!have) {
                PageGroup g; g.id = gid;
                const float* cv = GROUP_PALETTE[(gid - 1 + GROUP_PALETTE_SIZE) % GROUP_PALETTE_SIZE];
                g.col_r = cv[0]; g.col_g = cv[1]; g.col_b = cv[2];
                g_groups.push_back(g);
            }
            g_next_group_id = std::max(g_next_group_id, gid + 1);
        }
    }

    g_input.set_documents(&g_documents);
    auto map_doc = [&](int sd) -> int {
        return (sd >= 0 && sd < (int)doc_map.size()) ? doc_map[sd] : -1;
    };

    auto safe_page = [&](int di, int pi) -> Page* {
        if (di < 0 || di >= (int)g_documents.size()) return nullptr;
        auto& pages = g_documents[di].pages;
        auto it = std::find_if(pages.begin(), pages.end(),
                               [pi](const Page& p){ return p.page_index == pi; });
        return it != pages.end() ? &*it : nullptr;
    };

    for (const auto& sh : saved_hls)   if (auto* p = safe_page(map_doc(sh.doc), sh.page)) p->annots.highlights.push_back(sh.hl);
    for (const auto& sn : saved_notes) if (auto* p = safe_page(map_doc(sn.doc), sn.page)) p->annots.notes.push_back({sn.label});
    for (auto& ss : saved_strokes)     if (auto* p = safe_page(map_doc(ss.doc), ss.page)) p->annots.strokes.push_back(std::move(ss.stroke));

    // Migrate pre-v1.4 reference notes (keyed by position in the flat reference list, in
    // document→page→highlight order) onto the matching highlights. New files store the note
    // on the highlight directly ("rn"), so saved_ref_notes is empty and this is skipped.
    if (!saved_ref_notes.empty()) {
        std::vector<AnnotHighlight*> refs;
        for (auto& doc : g_documents)
            for (auto& pg : doc.pages)
                for (auto& hl : pg.annots.highlights)
                    if (!hl.text.empty()) refs.push_back(&hl);
        for (const auto& rn : saved_ref_notes)
            if (rn.after_idx >= 0 && rn.after_idx < (int)refs.size() && refs[rn.after_idx]->note.empty())
                refs[rn.after_idx]->note = rn.text;
    }

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
    g_dirty = false;                        // freshly loaded → matches disk
    g_last_save_wall = std::time(nullptr);  // in sync as of open (g_load_ok set by header check)
    add_to_recents(path);
    update_window_title();

    if (g_debug) {
        int n_hl = 0, n_note = 0, n_stroke = 0;
        for (const auto& doc : g_documents)
            for (const auto& pg : doc.pages) {
                n_hl     += (int)pg.annots.highlights.size();
                n_note   += (int)pg.annots.notes.size();
                n_stroke += (int)pg.annots.strokes.size();
            }
        printf("Project loaded: %s — %d docs, %d text boxes, %d highlights, %d notes, %d strokes\n",
               path.c_str(), (int)g_documents.size(), (int)g_text_boxes.size(), n_hl, n_note, n_stroke);
    }
}

void load_project() {
#ifdef __APPLE__
    const char* p = scholion_open_file("Open Project", nullptr, 0, 0);
#else
    before_file_dialog();
    const char* p = tinyfd_openFileDialog("Open Project", "", 0, nullptr, nullptr, 0);
    after_file_dialog();
#endif
    if (p) load_project_from_path(p);
}
