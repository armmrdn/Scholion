#include "toolbar.h"
#include "app_state.h"       // g_text_boxes, g_selected_box/g_editing_box, g_toolbar_bottom
#include "canvas_annot.h"    // g_annot_tool, g_text_tool, g_ann_drawing, pen/hl style, g_hl_box_mode
#include "canvas_text_box.h" // g_tbox_* style defaults
#include "actions.h"         // scholion_* pickers, load_pdfs_*, draw_url_modal
#include "project_io.h"      // before_file_dialog / after_file_dialog
#include "imgui.h"
#include "tinyfiledialogs.h"   // non-Apple file pickers (tinyfd_*)

// --- Toolbar UI -------------------------------------------------------------

void draw_toolbar_ui() {
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

    g_toolbar_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
}
