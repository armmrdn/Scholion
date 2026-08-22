#include "references_panel.h"
#include "app_state.h"
#include "canvas_annot.h"
#include "actions.h"
#include "project_io.h"   // before_file_dialog / after_file_dialog
#include "imgui.h"
#include "tinyfiledialogs.h"   // non-Apple file pickers (tinyfd_*)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

// ===== Note-edit state (private; reached from main via the hooks at the bottom) =====
// The highlight whose research note is being edited in the panel (nullptr = none). A stable
// pointer (not a list index) so editing can never target the wrong reference. Safe for the
// edit session: highlights aren't added/removed while a note field has focus, and it is reset
// on project load/new.
static AnnotHighlight* s_editing_ref_hl = nullptr;
static char s_ref_note_buf[2048] = {};

struct RefEntry { int di, pi, hi; };  // indices into g_documents[di].pages[pi].annots.highlights[hi]

void draw_references_tab() {
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


// ===== Hooks for main.cpp (key_callback input-suppression + new_project reset) =====
bool references_note_editing() { return s_editing_ref_hl != nullptr; }

void references_commit_note() {
    if (s_editing_ref_hl) {
        s_editing_ref_hl->note = s_ref_note_buf;   // empty buffer clears the note
        s_editing_ref_hl = nullptr;
    }
}

void references_reset() {
    s_editing_ref_hl  = nullptr;
    s_ref_note_buf[0] = '\0';
}
