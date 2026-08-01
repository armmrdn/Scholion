#pragma once
// Full-text search state shared between the search subsystem (main.cpp) and the panel Viewer's
// search-hit rendering (side_panel.cpp). Only the read-side types + result state live here; the
// worker thread/mutex/query buffer stay private to main.cpp.
#include <array>
#include <string>
#include <vector>

struct SearchResult {
    int         doc_idx;
    int         page_idx;
    std::string excerpt;
    std::string doc_name;
    std::vector<std::array<float,4>> hit_rects;  // normalized [0,1] quads from MuPDF
};

// Transient canvas highlight for the currently selected search result.
struct SearchHighlight {
    int doc_idx  = -1;
    int page_idx = -1;
    std::vector<std::array<float,4>> rects;
    bool active() const { return doc_idx >= 0 && !rects.empty(); }
};

extern std::vector<SearchResult> g_search_results;
extern int                       g_highlighted_search_result;
extern SearchHighlight           g_search_highlight;
