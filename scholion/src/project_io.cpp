#include "project_io.h"
#include "app_state.h"
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
                     "        { \"index\": %d, \"x\": %.4f, \"y\": %.4f, \"w\": %.2f, \"h\": %.2f, \"rot\": %d, \"grp\": %d }%s\n",
                     page.page_index, page.world_pos.x, page.world_pos.y,
                     page.world_w, page.world_h, page.rotation, page.group_id,
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

    struct SavedPage { int idx; float x, y; float w = 0.0f, h = 0.0f; int rot = 0; int grp = 0; };
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
    int   cur = -1;

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
    size_t tb_key  = content.find("\"text_boxes\":");
    size_t an_key  = content.find("\"annots\":");
    size_t rn_key  = content.find("\"ref_notes\":");
    size_t gr_key  = content.find("\"groups\":");
    auto section_at = [&](size_t at) -> Section {
        Section sec = Section::Other;
        if (doc_key != npos && at > doc_key) sec = Section::Docs;
        if (tb_key  != npos && at > tb_key)  sec = Section::TextBoxes;
        if (an_key  != npos && at > an_key)  sec = Section::Annots;
        if (rn_key  != npos && at > rn_key)  sec = Section::RefNotes;
        if (gr_key  != npos && at > gr_key)  sec = Section::Groups;
        return sec;
    };

    enum Tok { T_VIEWPORT, T_NOTE_IDX, T_PATH, T_REL, T_STACK, T_INDEX, T_ID, T_DOC, T_POINT, T_AFTER, T_GROUP };
    struct TokDef { const char* s; size_t len; Tok t; };
    static const TokDef toks[] = {
        {"\"viewport\":",     11, T_VIEWPORT},
        {"\"note_idx\":",     11, T_NOTE_IDX},
        {"\"path\":",          7, T_PATH},
        {"\"rel\":",           6, T_REL},
        {"\"stack_origin\":", 15, T_STACK},
        {"\"index\":",         8, T_INDEX},
        {"\"id\":",            5, T_ID},
        {"\"doc\":",           6, T_DOC},
        {"\"p\":",             4, T_POINT},
        {"\"after\":",         8, T_AFTER},
        {"\"group\":",         8, T_GROUP},
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
                if (sec == Section::Docs && sscanf(at, "\"path\": \"%4095[^\"]\"", s) == 1) {
                    saved.push_back({s, "", 0.0f, 0.0f, {}});
                    cur = (int)saved.size() - 1;
                }
                break;
            case T_REL:
                if (sec == Section::Docs && cur >= 0 && sscanf(at, "\"rel\": \"%4095[^\"]\"", s) == 1)
                    saved[cur].rel = s;
                break;
            case T_STACK:
                if (sec == Section::Docs && cur >= 0 &&
                    sscanf(at, "\"stack_origin\": [%f, %f]", &a, &b) == 2) {
                    saved[cur].sox = a; saved[cur].soy = b;
                }
                break;
            case T_INDEX:
                if (sec == Section::Docs && cur >= 0) {
                    float pw = 0.0f, ph = 0.0f; int prot = 0, pgrp = 0;
                    int np = sscanf(at, "\"index\": %d, \"x\": %f, \"y\": %f, \"w\": %f, \"h\": %f, \"rot\": %d, \"grp\": %d",
                                    &n, &a, &b, &pw, &ph, &prot, &pgrp);
                    if (np >= 3) saved[cur].pages.push_back({n, a, b, pw, ph, prot, pgrp});
                }
                break;
            case T_GROUP:
                if (sec == Section::Groups) {
                    int gid; float gr = 0.63f, gg = 0.32f, gb = 0.75f; char gname[512] = "";
                    int np = sscanf(at, "\"group\": %d, \"r\": %f, \"g\": %f, \"b\": %f",
                                    &gid, &gr, &gg, &gb);
                    if (np >= 1) {
                        // Name is optional and parsed separately (may contain spaces/escapes).
                        const char* nm = strstr(at, "\"name\": \"");
                        if (nm) sscanf(nm, "\"name\": \"%511[^\"]\"", gname);
                        saved_groups.push_back({gid, gr, gg, gb, gname});
                    }
                }
                break;
            case T_ID:
                if (sec == Section::TextBoxes) {
                    int id; float x, y, tr = 0.82f, tg = 0.06f, tb2 = 0.06f, tfs = 16.0f, tw = 0.0f, th = 0.0f;
                    int tzs = 0;
                    int np = sscanf(at,
                        "\"id\": %d, \"x\": %f, \"y\": %f, \"r\": %f, \"g\": %f, \"b\": %f, \"fs\": %f, \"w\": %f, \"h\": %f, \"zs\": %d",
                        &id, &x, &y, &tr, &tg, &tb2, &tfs, &tw, &th, &tzs);
                    if (np >= 3) {
                        CanvasTextBox tb; tb.id = id; tb.world_pos = {x, y}; tb.text[0] = '\0';
                        if (np >= 7) { tb.r = tr; tb.g = tg; tb.b = tb2; tb.font_size = tfs; }
                        if (np >= 9) { tb.w = tw; tb.h = th; }
                        if (np >= 10) tb.zoom_scaled = (tzs != 0);  // older files omit "zs" → false
                        extract_json_text(at, tb.text, sizeof(tb.text));
                        saved_boxes.push_back(tb);
                    }
                }
                break;
            case T_DOC:
                if (sec == Section::Annots) {
                    int di, pi; float sw, sa; int npx;
                    if (sscanf(at, "\"doc\": %d, \"page\": %d, \"hl\": [%f, %f, %f, %f]",
                               &di, &pi, &a, &b, &c, &d) == 6) {
                        flush_stroke();
                        AnnotHighlight parsed_hl{a, b, c, d, {}};
                        // Decode a JSON-escaped string field (\n \" \\) starting just after
                        // the opening quote into `dst`.
                        auto decode_str = [](const char* p, std::string& dst) {
                            while (*p && *p != '"') {
                                if (*p == '\\' && *(p + 1)) {
                                    ++p;
                                    switch (*p) {
                                        case 'n':  dst += '\n'; break;
                                        case '"':  dst += '"';  break;
                                        case '\\': dst += '\\'; break;
                                        default:   dst += *p;   break;
                                    }
                                } else { dst += *p; }
                                ++p;
                            }
                        };
                        // Bound field search to THIS record (before the next annot record)
                        // so a highlight lacking a field can't grab a later record's one.
                        const char* next_rec = strstr(at + 6, "\"doc\":");
                        auto within = [&](const char* p){ return p && (!next_rec || p < next_rec); };
                        const char* ht = strstr(at, "\"ht\": \"");
                        if (within(ht)) decode_str(ht + 7, parsed_hl.text);
                        const char* rn = strstr(at, "\"rn\": \"");   // per-highlight research note
                        if (within(rn)) decode_str(rn + 7, parsed_hl.note);
                        saved_hls.push_back({di, pi, std::move(parsed_hl)});
                    } else if (sscanf(at, "\"doc\": %d, \"page\": %d, \"note\": \"%4095[^\"]\"",
                                      &di, &pi, s) == 3) {
                        flush_stroke(); saved_notes.push_back({di, pi, s});
                    } else if ((npx = sscanf(at,
                                   "\"doc\": %d, \"page\": %d, \"sr\": %f, \"sg\": %f, \"sb\": %f, \"sw\": %f, \"sa\": %f",
                                   &di, &pi, &a, &b, &c, &sw, &sa)) >= 5) {
                        flush_stroke();
                        stroke_doc = di; stroke_page = pi;
                        building_stroke = {};  // struct defaults width=1.2, alpha=0.88
                        building_stroke.r = a; building_stroke.g = b; building_stroke.b = c;
                        if (npx >= 6) building_stroke.width = sw;  // older files omit sw/sa → defaults
                        if (npx >= 7) building_stroke.alpha = sa;
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
            case T_AFTER:
                if (sec == Section::RefNotes && sscanf(at, "\"after\": %d", &n) == 1) {
                    RefNote rn; rn.after_idx = n;
                    const char* tp = strstr(at, "\"text\": \"");
                    if (tp) {
                        tp += 9;
                        while (*tp && *tp != '"') {
                            if (*tp == '\\' && *(tp+1)) {
                                ++tp;
                                switch (*tp) {
                                    case 'n':  rn.text += '\n'; break;
                                    case '"':  rn.text += '"';  break;
                                    case '\\': rn.text += '\\'; break;
                                    default:   rn.text += *tp;  break;
                                }
                            } else { rn.text += *tp; }
                            ++tp;
                        }
                    }
                    saved_ref_notes.push_back(std::move(rn));
                }
                break;
        }
        scan = best + bt->len;
    }
    flush_stroke();

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
