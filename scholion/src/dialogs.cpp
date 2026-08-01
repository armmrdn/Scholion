#include "dialogs.h"
#include "app_state.h"
#include "actions.h"        // scholion pickers, load_pdfs_*
#include "project_io.h"     // before/after_file_dialog, load_project, save_project_current
#include "version.h"        // SCHOLION_VERSION
#include "imgui.h"
#include <algorithm>

// ===== Shared control flags (extern in dialogs.h; main raises/reads them) =====
bool      g_startup_chooser = false;
bool      g_quit_requested  = false;
QuitState g_quit_state      = QuitState::None;

void draw_startup_chooser() {
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

void draw_quit_dialog() {
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
