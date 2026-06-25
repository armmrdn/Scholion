#include "rast_pipeline.h"
#include "app_state.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

// --- Internal pipeline state (not exposed) -----------------------------------

struct RastTask {
    std::shared_ptr<PdfLoader> loader;
    std::string doc_path;
    int         page_index       = 0;
    LodTier     tier             = LodTier::Thumb;
    float       viewport_dist_sq = 0.0f;
    int         tile_col         = -1;
    int         tile_row         = -1;
};
struct RastResult {
    std::string             doc_path;
    int                     page_index = 0;
    LodTier                 tier       = LodTier::Thumb;
    PdfLoader::RasterBuffer buf;
    int                     tile_col   = -1;
    int                     tile_row   = -1;
};

static std::mutex              g_rast_mutex;
static std::condition_variable g_rast_cv;
static std::vector<RastTask>   g_rast_tasks;
static std::vector<RastResult> g_rast_ready;
static std::unordered_set<std::string> g_rast_inflight;
static std::atomic<bool>       g_rast_stop{false};
static std::thread             g_rast_thread;

// Backpressure cap: worker pauses once this many completed tiles are queued.
// 16 tiles * 256 KB = 4 MB max in the ready queue; uploading that per frame
// is negligible on modern GL drivers and cuts fill time by ~8x vs the old cap of 2.
static constexpr int RAST_READY_MAX = 16;

static constexpr size_t VRAM_BUDGET = 350ULL * 1024 * 1024;
static constexpr size_t TILE_BUDGET = 200ULL * 1024 * 1024;

// --- Internal helpers --------------------------------------------------------

static std::string rast_key(const std::string& path, int pg, LodTier tier,
                             int tc = -1, int tr = -1) {
    std::string k = path + '\0' + std::to_string(pg) + '\0' + std::to_string((int)tier);
    if (tc >= 0) { k += '\0'; k += std::to_string(tc); k += '\0'; k += std::to_string(tr); }
    return k;
}

static void rast_worker() {
    while (true) {
        RastTask task;
        {
            std::unique_lock<std::mutex> lk(g_rast_mutex);
            g_rast_cv.wait(lk, []{
                return g_rast_stop ||
                       (!g_rast_tasks.empty() && (int)g_rast_ready.size() < RAST_READY_MAX);
            });
            if (g_rast_stop && g_rast_tasks.empty()) break;
            task = std::move(g_rast_tasks.front());
            g_rast_tasks.erase(g_rast_tasks.begin());
            g_rast_inflight.erase(rast_key(task.doc_path, task.page_index, task.tier,
                                           task.tile_col, task.tile_row));
        }
        auto buf = task.loader->rasterize_to_buffer(task.page_index, task.tier,
                                                     task.tile_col, task.tile_row);
        {
            std::lock_guard<std::mutex> lk(g_rast_mutex);
            g_rast_ready.push_back({task.doc_path, task.page_index, task.tier, std::move(buf),
                                    task.tile_col, task.tile_row});
        }
        glfwPostEmptyEvent();
    }
}

static void enqueue_rast_cancelling_stale(const std::string& doc_path,
                                          std::shared_ptr<PdfLoader> loader,
                                          int page_index, LodTier tier,
                                          float viewport_dist_sq = 0.0f) {
    std::string new_key = rast_key(doc_path, page_index, tier);
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    auto it = std::remove_if(g_rast_tasks.begin(), g_rast_tasks.end(),
        [&](const RastTask& t) {
            if (t.doc_path != doc_path || t.page_index != page_index ||
                t.tier == tier || t.tile_col >= 0)
                return false;
            g_rast_inflight.erase(rast_key(t.doc_path, t.page_index, t.tier));
            return true;
        });
    g_rast_tasks.erase(it, g_rast_tasks.end());
    if (g_rast_inflight.count(new_key)) return;
    g_rast_inflight.insert(new_key);
    g_rast_tasks.push_back({loader, doc_path, page_index, tier, viewport_dist_sq});
    g_rast_cv.notify_one();
}

static void enqueue_tile_rast(const std::string& doc_path,
                               const std::shared_ptr<PdfLoader>& loader,
                               int page_index, int tile_col, int tile_row,
                               float viewport_dist_sq = 0.0f) {
    std::string key = rast_key(doc_path, page_index, LodTier::High, tile_col, tile_row);
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    if (g_rast_inflight.count(key)) return;
    g_rast_inflight.insert(key);
    g_rast_tasks.push_back({loader, doc_path, page_index, LodTier::High,
                             viewport_dist_sq, tile_col, tile_row});
    g_rast_cv.notify_one();
}

static void enqueue_visible_high_tiles(const Document& doc,
                                        const std::shared_ptr<PdfLoader>& loader,
                                        const Page& page, Vec2 vp_center) {
    // Tile col/row indices are in original PDF pixel space; they don't map
    // correctly to a rotated quad, so fall back to Low/Thumb for rotated pages.
    if (page.rotation != 0) return;

    float px_per_wu = dpi_for_lod(LodTier::High) * g_content_scale / 72.0f;
    float tile_wu   = static_cast<float>(TILE_PX) / px_per_wu;
    int   n_cols    = static_cast<int>(std::ceil(page.world_w / tile_wu));
    int   n_rows    = static_cast<int>(std::ceil(page.world_h / tile_wu));

    for (int row = 0; row < n_rows; ++row) {
        for (int col = 0; col < n_cols; ++col) {
            if (g_tile_cache.count(tile_cache_key(doc.path, page.page_index, col, row)))
                continue;

            float tx = page.world_pos.x + col * tile_wu;
            float ty = page.world_pos.y + row * tile_wu;
            float tw = (tx + tile_wu < page.world_pos.x + page.world_w)
                           ? tile_wu : (page.world_pos.x + page.world_w - tx);
            float th = (ty + tile_wu < page.world_pos.y + page.world_h)
                           ? tile_wu : (page.world_pos.y + page.world_h - ty);
            // Prefetch 2 tiles beyond the visible edge so the user can pan without
            // hitting a blank tile. At TILE_PX=256 this adds a modest ring of tiles.
            constexpr float PREFETCH = 2.0f;
            if (!is_world_rect_visible(tx - PREFETCH * tile_wu, ty - PREFETCH * tile_wu,
                                       tw + 2.0f * PREFETCH * tile_wu,
                                       th + 2.0f * PREFETCH * tile_wu)) continue;

            float tcx = tx + tw * 0.5f - vp_center.x;
            float tcy = ty + th * 0.5f - vp_center.y;
            enqueue_tile_rast(doc.path, loader, page.page_index, col, row,
                              tcx * tcx + tcy * tcy);
        }
    }
}

// --- Public API --------------------------------------------------------------

void rast_init() {
    g_rast_stop.store(false);
    g_rast_thread = std::thread(rast_worker);
}

void rast_shutdown() {
    g_rast_stop.store(true);
    g_rast_cv.notify_one();
    if (g_rast_thread.joinable()) g_rast_thread.join();
}

void enqueue_rast(const std::string& doc_path,
                  std::shared_ptr<PdfLoader> loader,
                  int page_index, LodTier tier) {
    std::string key = rast_key(doc_path, page_index, tier);
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    if (g_rast_inflight.count(key)) return;
    g_rast_inflight.insert(key);
    g_rast_tasks.push_back({loader, doc_path, page_index, tier});
    g_rast_cv.notify_one();
}

bool drain_rast_results() {
    std::vector<RastResult> ready;
    {
        std::lock_guard<std::mutex> lk(g_rast_mutex);
        ready.swap(g_rast_ready);
    }
    if (!ready.empty()) g_rast_cv.notify_one();
    bool did_upload = false;
    for (auto& res : ready) {
        if (res.tile_col >= 0) {
            if (!res.buf.ok) continue;
            std::string key = tile_cache_key(res.doc_path, res.page_index,
                                             res.tile_col, res.tile_row);
            if (g_tile_cache.count(key)) continue;
            uint32_t tex = g_cache.upload_raw(res.buf.pixels.data(),
                                               res.buf.width, res.buf.height);
            g_tile_cache[key] = tex;
            did_upload = true;
            continue;
        }

        Page* target = nullptr;
        for (auto& doc : g_documents) {
            if (doc.path != res.doc_path) continue;
            for (auto& page : doc.pages)
                if (page.page_index == res.page_index) { target = &page; break; }
            break;
        }
        if (!target || !target->needs_lod(res.tier)) continue;
        if (!res.buf.ok) {
            target->rast_failed = true;
            continue;
        }
        g_cache.upload(*target, res.tier, res.buf.pixels.data(), res.buf.width, res.buf.height);
        did_upload = true;
    }
    return did_upload;
}

void evict_page_tiles(const std::string& doc_path, int page_index) {
    std::string prefix = doc_path + '\0' + std::to_string(page_index) + '\0';
    for (auto it = g_tile_cache.begin(); it != g_tile_cache.end(); ) {
        if (it->first.size() > prefix.size() &&
            it->first.compare(0, prefix.size(), prefix) == 0) {
            g_cache.free_raw(it->second);
            it = g_tile_cache.erase(it);
        } else {
            ++it;
        }
    }
}

bool page_has_tiles(const std::string& doc_path, int page_index) {
    std::string prefix = doc_path + '\0' + std::to_string(page_index) + '\0';
    for (auto& kv : g_tile_cache)
        if (kv.first.size() > prefix.size() &&
            kv.first.compare(0, prefix.size(), prefix) == 0)
            return true;
    return false;
}

void cancel_page_tile_tasks(const std::string& doc_path, int page_index) {
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    auto it = std::remove_if(g_rast_tasks.begin(), g_rast_tasks.end(),
        [&](const RastTask& t) {
            if (t.doc_path != doc_path || t.page_index != page_index || t.tile_col < 0)
                return false;
            g_rast_inflight.erase(rast_key(t.doc_path, t.page_index, t.tier,
                                           t.tile_col, t.tile_row));
            return true;
        });
    g_rast_tasks.erase(it, g_rast_tasks.end());
}

bool is_page_visible(const Page& page) {
    Vec2  tl = g_canvas.world_to_screen({page.world_pos.x,               page.world_pos.y});
    Vec2  br = g_canvas.world_to_screen({page.world_pos.x + page.world_w, page.world_pos.y + page.world_h});
    float vw = g_canvas.get_viewport_width();
    float vh = g_canvas.get_viewport_height();
    return br.x >= 0.0f && tl.x <= vw && br.y >= 0.0f && tl.y <= vh;
}

bool is_world_rect_visible(float wx, float wy, float ww, float wh) {
    Vec2  tl = g_canvas.world_to_screen({wx,      wy});
    Vec2  br = g_canvas.world_to_screen({wx + ww, wy + wh});
    float vw = g_canvas.get_viewport_width();
    float vh = g_canvas.get_viewport_height();
    return br.x >= 0.0f && tl.x <= vw && br.y >= 0.0f && tl.y <= vh;
}

void rast_cancel_doc(const std::string& doc_path) {
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    g_rast_tasks.erase(
        std::remove_if(g_rast_tasks.begin(), g_rast_tasks.end(),
            [&](const RastTask& t){ return t.doc_path == doc_path; }),
        g_rast_tasks.end());
    for (auto it = g_rast_inflight.begin(); it != g_rast_inflight.end(); )
        it = (it->rfind(doc_path + '\0', 0) == 0) ? g_rast_inflight.erase(it) : std::next(it);
}

void rast_cancel_all() {
    std::lock_guard<std::mutex> lk(g_rast_mutex);
    g_rast_tasks.clear();
    g_rast_inflight.clear();
    g_rast_ready.clear();
}

void stream_lod(bool vram_changed) {
    if (g_documents.empty()) return;

    {
        static float s_last_zoom  = -1.0f;
        static float s_last_off_x = 0.0f;
        static float s_last_off_y = 0.0f;
        float cur_zoom = g_canvas.get_zoom();
        Vec2  cur_off  = g_canvas.get_offset();
        bool view_changed = (cur_zoom != s_last_zoom ||
                             cur_off.x != s_last_off_x ||
                             cur_off.y != s_last_off_y);
        bool tasks_pending;
        { std::lock_guard<std::mutex> lk(g_rast_mutex); tasks_pending = !g_rast_tasks.empty(); }
        if (!view_changed && !vram_changed && !tasks_pending) return;
        s_last_zoom  = cur_zoom;
        s_last_off_x = cur_off.x;
        s_last_off_y = cur_off.y;
    }

    LodTier target = lod_for_zoom(g_canvas.get_zoom());
    if (g_settings.compat_mode && target == LodTier::High)
        target = LodTier::Low;

    Vec2 vp_center = g_canvas.screen_to_world(
        {g_canvas.get_viewport_width() * 0.5f, g_canvas.get_viewport_height() * 0.5f});

    for (int di = 0; di < (int)g_documents.size(); ++di) {
        auto& doc    = g_documents[di];
        auto& loader = g_loaders[di];
        if (!loader) continue;

        for (auto& page : doc.pages) {
            bool vis = is_page_visible(page);

            if (vis) {
                if (target == LodTier::High) {
                    enqueue_visible_high_tiles(doc, loader, page, vp_center);
                } else {
                    if (page.needs_lod(target)) {
                        float cx = page.world_pos.x + page.world_w * 0.5f - vp_center.x;
                        float cy = page.world_pos.y + page.world_h * 0.5f - vp_center.y;
                        enqueue_rast_cancelling_stale(doc.path, loader, page.page_index, target,
                                                      cx*cx + cy*cy);
                    }
                    evict_page_tiles(doc.path, page.page_index);
                    cancel_page_tile_tasks(doc.path, page.page_index);
                    if (page.tex_high)
                        g_cache.evict_tier(page, LodTier::High);
                }
                if (target < LodTier::Low && page.tex_low)
                    g_cache.evict_tier(page, LodTier::Low);
            } else {
                cancel_page_tile_tasks(doc.path, page.page_index);
                if (page.tex_high)
                    g_cache.evict_tier(page, LodTier::High);
                if (page.tex_low && g_cache.vram_bytes() > VRAM_BUDGET)
                    g_cache.evict_tier(page, LodTier::Low);
            }
        }
    }

    if (g_cache.vram_bytes() > TILE_BUDGET) {
        struct TilePage { float dist_sq; std::string path; int page_idx; };
        std::vector<TilePage> candidates;
        for (auto& doc : g_documents) {
            for (auto& page : doc.pages) {
                if (is_page_visible(page)) continue;
                if (!page_has_tiles(doc.path, page.page_index)) continue;
                float cx = page.world_pos.x + page.world_w * 0.5f - vp_center.x;
                float cy = page.world_pos.y + page.world_h * 0.5f - vp_center.y;
                candidates.push_back({cx*cx + cy*cy, doc.path, page.page_index});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const TilePage& a, const TilePage& b) { return a.dist_sq > b.dist_sq; });
        for (auto& c : candidates) {
            if (g_cache.vram_bytes() <= TILE_BUDGET) break;
            evict_page_tiles(c.path, c.page_idx);
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_rast_mutex);
        if (g_rast_tasks.size() > 1)
            std::stable_sort(g_rast_tasks.begin(), g_rast_tasks.end(),
                [](const RastTask& a, const RastTask& b) {
                    return a.viewport_dist_sq < b.viewport_dist_sq;
                });
    }
}
