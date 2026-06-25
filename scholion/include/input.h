#pragma once

#include "canvas.h"
#include "document.h"
#include <unordered_set>
#include <vector>

struct GLFWwindow;

class InputHandler {
public:
    InputHandler(Canvas& canvas);

    void on_mouse_button(GLFWwindow* window, int button, int action, int mods);
    void on_cursor_move(GLFWwindow* window, double xpos, double ypos);
    void on_scroll(GLFWwindow* window, double xoffset, double yoffset);
    void on_key(GLFWwindow* window, int key, int scancode, int action, int mods);
    void update(GLFWwindow* window);

    void clear_hover() { m_hovered_doc = nullptr; m_hovered_page = nullptr; }

    // Called when the window loses focus so key-up events that were missed (e.g.
    // space released while cmd-tabbing away) don't leave m_space_held stuck true.
    void clear_held_keys() {
        m_space_held        = false;
        m_space_dragged     = false;
        m_panning           = false;
        m_dragged_page      = nullptr;
        m_drag_active       = false;
        m_drag_pending_page = nullptr;
        m_drag_pending_doc  = nullptr;
        m_multi_drag_active = false;
        m_drag_origins.clear();
        m_box_selecting     = false;
    }

    bool is_panning()       const { return m_panning; }
    bool is_multi_dragging() const { return m_multi_drag_active; }
    Vec2 multi_drag_grab_world() const { return m_multi_drag_grab_world; }
    void start_multi_drag(Vec2 grab_screen);

    const Document* selected_doc() const { return m_selected_doc; }
    const Document* hovered_doc()  const { return m_hovered_doc; }
    const Page*     hovered_page() const { return m_hovered_page; }
    const Page*     dragged_page()        const { return m_dragged_page; }
    const Page*     pending_drag_page()   const { return m_drag_pending_page; }

    // Multi-page selection + text-box selection (unified model)
    const std::unordered_set<Page*>& selection() const { return m_selection; }
    const std::unordered_set<int>&   selected_text_boxes() const { return m_selected_text_boxes; }
    void clear_selection() {
        m_selection.clear();
        m_selected_text_boxes.clear();
        m_multi_drag_active = false;
        m_drag_origins.clear();
        m_box_selecting = false;
    }

    // Rubber-band box state (for renderer)
    bool box_selecting()   const { return m_box_selecting; }
    Vec2 box_start_world() const { return m_box_start_world; }
    Vec2 box_cur_world()   const { return m_box_cur_world; }

    // Panel viewer
    bool panel_open()        const { return m_panel_open; }
    int  panel_doc_index()   const { return m_panel_doc_index; }
    int  panel_scroll_page() const { return m_panel_scroll_page; }
    void clear_panel_scroll()      { m_panel_scroll_page = -1; }
    void close_panel()             { m_panel_open = false; m_panel_doc_index = -1; }
    void open_panel(int doc_idx, int scroll_page = -1) {
        m_panel_open        = true;
        m_panel_doc_index   = doc_idx;
        m_panel_scroll_page = scroll_page;
    }

    // Page right-click context menu
    bool      ctx_pending() const { return m_ctx_pending; }
    Document* ctx_doc()     const { return m_ctx_doc; }
    Page*     ctx_page()    const { return m_ctx_page; }
    Vec2      ctx_pos()     const { return m_ctx_pos; }
    void      consume_ctx()       { m_ctx_pending = false; }

    // Canvas (empty space) right-click context menu
    bool canvas_ctx_pending() const { return m_canvas_ctx_pending; }
    Vec2 canvas_ctx_pos()     const { return m_canvas_ctx_pos; }
    void consume_canvas_ctx()       { m_canvas_ctx_pending = false; }

    // F3 — performance overlay toggle. Latched on key press; main consumes it.
    bool consume_overlay_toggle() {
        bool v = m_overlay_toggle_pending;
        m_overlay_toggle_pending = false;
        return v;
    }

    // Space tap (pressed and released without any pan drag): main consumes this to
    // toggle the panel for the currently selected document.
    bool consume_space_tap() {
        bool v = m_space_tap_pending;
        m_space_tap_pending = false;
        return v;
    }

    // Registering documents also clears any stale Page* in selection.
    void set_documents(std::vector<Document>* docs) {
        m_documents = docs;
        clear_selection();
    }

private:
    Canvas& m_canvas;
    std::vector<Document>* m_documents = nullptr;

    // --- Selection state machine -----------------------------------------------
    // m_selection (pages) and m_selected_text_boxes (IDs) form a unified selection.
    // They are always modified together by the public API (clear_selection, etc.).
    //
    // Drag modes are mutually exclusive:
    //   m_box_selecting    — rubber-band active; cleared by start_multi_drag so a
    //                        mousedown on an item always supersedes a band in progress.
    //   m_multi_drag_active — group page drag active; set by start_multi_drag.
    //   m_drag_pending_page — deferred single-page drag (awaiting 4 px threshold);
    //                         coexists with m_multi_drag_active = false only.
    //
    // Text-box dragging (g_box_dragging in main.cpp) is a separate system that
    // does NOT use m_multi_drag_active to avoid the glfwPollEvents/ImGui frame-
    // boundary race. It reads multi_drag_grab_world() as its grab point when a
    // page-initiated drag also needs to move text boxes.

    // Pan
    bool m_panning       = false;
    bool m_space_held    = false;
    // True if a pan drag occurred during the current space-hold; cleared on space release.
    // Used to distinguish a tap (no drag) from a hold-and-pan so we don't fire the panel
    // toggle after the user finishes panning with space+drag.
    bool m_space_dragged = false;
    bool m_space_tap_pending = false;
    Vec2 m_last_mouse    = {};
    Vec2 m_current_mouse = {};

    // Hover
    Document* m_hovered_doc  = nullptr;
    Page*     m_hovered_page = nullptr;

    // Single-page deferred drag (4 px threshold before move activates)
    Vec2      m_drag_start_screen  = {};
    bool      m_drag_active        = false;
    Page*     m_drag_pending_page  = nullptr;
    Document* m_drag_pending_doc   = nullptr;
    Page*     m_dragged_page       = nullptr;
    Vec2      m_drag_world_offset  = {};

    // Multi-page drag
    struct DragOrigin { Page* page; Vec2 offset; };  // offset = page_world_pos − grab_world
    bool                    m_multi_drag_active    = false;
    Vec2                    m_multi_drag_grab_world = {};
    std::vector<DragOrigin> m_drag_origins;

    // Rubber-band box selection
    bool m_box_selecting   = false;
    Vec2 m_box_start_world = {};
    Vec2 m_box_cur_world   = {};

    // Page selection set (raw pointers; cleared whenever documents change)
    std::unordered_set<Page*> m_selection;

    // Text-box selection set (IDs; unified with page selection)
    std::unordered_set<int> m_selected_text_boxes;

    // Multi-page + text-box drag (offset for each selected item)
    struct TextBoxDragOrigin { int id; Vec2 offset; };
    std::vector<TextBoxDragOrigin> m_text_box_drag_origins;

    // Which document's threads are shown
    Document* m_selected_doc = nullptr;

    // Panel
    bool m_panel_open        = false;
    int  m_panel_doc_index   = -1;
    int  m_panel_scroll_page = -1;

    // Page right-click
    bool      m_ctx_pending = false;
    Document* m_ctx_doc     = nullptr;
    Page*     m_ctx_page    = nullptr;
    Vec2      m_ctx_pos     = {};

    // Canvas right-click
    bool m_canvas_ctx_pending = false;
    Vec2 m_canvas_ctx_pos     = {};

    // F3 overlay toggle latch
    bool m_overlay_toggle_pending = false;

    struct HitResult { Document* doc; Page* page; };
    HitResult hit_test(Vec2 screen_pos) const;

    void finalize_box_selection();

    static constexpr float ZOOM_SENSITIVITY  = 0.1f;
    static constexpr float DRAG_THRESHOLD_SQ = 16.0f;
};
