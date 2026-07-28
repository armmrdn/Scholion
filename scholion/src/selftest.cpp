// Headless save/load round-trip self-test. Run with:  Scholion --selftest
//
// Exercises the REAL serialization (build_project_json) and parser
// (load_project_from_path) against an in-memory project, then asserts every
// persisted field survives the round-trip. Uses documents with unresolvable
// paths so they load as placeholders — no MuPDF, no GL context, no window —
// which lets this run in CI (returns exit code 0 = pass, 1 = fail).
//
// Also includes a regression test for the fixed unbounded-sscanf stack overflow
// on over-long path fields.

#include "app_state.h"
#include "project_io.h"
#include "canvas_annot.h"
#include "document.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

static int s_fail = 0;

#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); ++s_fail; } } while (0)

static bool approx(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

int run_selftest() {
    printf("Scholion self-test — save/load round-trip\n");
    namespace fs = std::filesystem;

    // ----- Build a known project entirely in memory -------------------------
    g_documents.clear();
    g_text_boxes.clear();
    g_canvas_strokes.clear();
    g_groups.clear();
    g_next_note_idx = 7;
    g_next_box_id   = 0;
    g_next_group_id = 1;

    auto make_doc = [&](const char* path, int npages, float ox, float oy) {
        Document d; d.path = path;
        for (int i = 0; i < npages; ++i) {
            Page p;
            p.page_index = i;
            p.world_pos  = { ox + i * 30.0f, oy + i * 15.0f };
            p.world_w    = 600.0f + i;
            p.world_h    = 800.0f + i;
            p.rotation   = (i == 1) ? 90 : 0;
            d.pages.push_back(p);
        }
        g_documents.push_back(std::move(d));
    };
    make_doc("/nonexistent/scholion_selftest_a.pdf", 2, 100.0f, 200.0f);
    make_doc("/nonexistent/scholion_selftest_b.pdf", 1, 900.0f, 250.0f);

    // Cross-document group: doc0.page0 + doc1.page0.
    { PageGroup g; g.id = 1; g.name = "Topic X"; g.col_r = 0.63f; g.col_g = 0.32f; g.col_b = 0.75f;
      g_groups.push_back(g); }
    g_documents[0].pages[0].group_id = 1;
    g_documents[1].pages[0].group_id = 1;

    // Annotations on doc0/page0 (stroke + text highlight with a newline + a note flag).
    {
        auto& an = g_documents[0].pages[0].annots;
        AnnotStroke s; s.pts = {{0.1f, 0.1f}, {0.2f, 0.3f}}; s.r = 0.8f; s.g = 0.1f; s.b = 0.1f;
        s.width = 1.2f; s.alpha = 0.88f;
        an.strokes.push_back(s);
        AnnotHighlight hl; hl.x0 = 0.2f; hl.y0 = 0.25f; hl.x1 = 0.6f; hl.y1 = 0.30f;
        hl.text = "quoted passage\nsecond line";
        hl.note = "my research note";   // reference note stored on the highlight
        an.highlights.push_back(hl);
        an.notes.push_back({"A"});
    }

    // Text box, reference note, canvas (world) stroke, viewport, note counter.
    { CanvasTextBox b; b.id = g_next_box_id++; b.world_pos = {150.0f, 180.0f};
      snprintf(b.text, sizeof(b.text), "note text");
      b.r = 0.82f; b.g = 0.06f; b.b = 0.06f; b.font_size = 16.0f;
      b.w = 120.0f; b.h = 40.0f; b.zoom_scaled = true;
      g_text_boxes.push_back(b); }
    { AnnotStroke cs; cs.pts = {{10.0f, 10.0f}, {50.0f, 60.0f}}; g_canvas_strokes.push_back(cs); }
    g_canvas.set_offset({123.0f, 456.0f});
    g_canvas.set_zoom(0.75f);

    // ----- Save, then load back ---------------------------------------------
    std::string tmp = (fs::temp_directory_path() / "scholion_selftest.scholion").string();
    CHECK(save_to_path(tmp), "save_to_path failed");
    load_project_from_path(tmp);

    // ----- Verify every persisted field survived ----------------------------
    CHECK(g_documents.size() == 2, "doc count %zu != 2", g_documents.size());
    if (g_documents.size() == 2) {
        CHECK(g_documents[0].pages.size() == 2, "doc0 pages %zu != 2", g_documents[0].pages.size());
        CHECK(g_documents[1].pages.size() == 1, "doc1 pages %zu != 1", g_documents[1].pages.size());
        const Page& p00 = g_documents[0].pages[0];
        CHECK(approx(p00.world_pos.x, 100.0f, 0.01f) && approx(p00.world_pos.y, 200.0f, 0.01f),
              "page0 position not restored");
        CHECK(approx(p00.world_w, 600.0f, 0.05f) && approx(p00.world_h, 800.0f, 0.05f),
              "page0 size not restored");
        CHECK(g_documents[0].pages[1].rotation == 90, "page1 rotation %d != 90",
              g_documents[0].pages[1].rotation);
        CHECK(p00.group_id == 1 && g_documents[1].pages[0].group_id == 1, "group membership lost");
        CHECK(p00.annots.strokes.size() == 1, "strokes %zu != 1", p00.annots.strokes.size());
        CHECK(p00.annots.highlights.size() == 1, "highlights %zu != 1", p00.annots.highlights.size());
        CHECK(p00.annots.notes.size() == 1, "notes %zu != 1", p00.annots.notes.size());
        if (p00.annots.highlights.size() == 1) {
            CHECK(p00.annots.highlights[0].text == "quoted passage\nsecond line",
                  "highlight text (incl. newline) not preserved: [%s]",
                  p00.annots.highlights[0].text.c_str());
            CHECK(p00.annots.highlights[0].note == "my research note",
                  "reference note (on highlight) not preserved: [%s]",
                  p00.annots.highlights[0].note.c_str());
        }
    }
    CHECK(g_text_boxes.size() == 1, "text boxes %zu != 1", g_text_boxes.size());
    if (g_text_boxes.size() == 1) {
        const CanvasTextBox& b = g_text_boxes[0];
        CHECK(std::strcmp(b.text, "note text") == 0, "text-box content [%s]", b.text);
        CHECK(b.zoom_scaled, "text-box zoom_scaled flag lost");
        CHECK(approx(b.w, 120.0f, 0.05f) && approx(b.h, 40.0f, 0.05f), "text-box size not restored");
    }
    CHECK(g_canvas_strokes.size() == 1, "canvas strokes %zu != 1", g_canvas_strokes.size());
    CHECK(g_groups.size() == 1, "groups %zu != 1", g_groups.size());
    if (g_groups.size() == 1) {
        CHECK(g_groups[0].id == 1, "group id %d != 1", g_groups[0].id);
        CHECK(g_groups[0].name == "Topic X", "group name [%s]", g_groups[0].name.c_str());
        CHECK(approx(g_groups[0].col_r, 0.63f, 0.01f), "group color not restored");
    }
    CHECK(approx(g_canvas.get_offset().x, 123.0f, 0.01f) && approx(g_canvas.get_offset().y, 456.0f, 0.01f),
          "viewport offset not restored");
    CHECK(approx(g_canvas.get_zoom(), 0.75f, 0.001f), "viewport zoom %f != 0.75", g_canvas.get_zoom());
    CHECK(g_next_note_idx == 7, "note_idx %d != 7", g_next_note_idx);
    fs::remove(tmp);

    // ----- Regression: over-long path must not overflow the parse buffer -----
    // Before the fix, an unbounded sscanf("%[^\"]") into a 4096-byte buffer would
    // smash the stack on a path longer than the buffer. It must now truncate safely.
    {
        std::string longpath(5000, 'A');
        std::string json =
            "{\n  \"format\": 1,\n"
            "  \"viewport\": { \"x\": 0.0, \"y\": 0.0, \"zoom\": 0.6 },\n"
            "  \"documents\": [\n"
            "    { \"path\": \"" + longpath + "\", \"stack_origin\": [0.0, 0.0],\n"
            "      \"pages\": [ { \"index\": 0, \"x\": 0.0, \"y\": 0.0, \"w\": 100.0, \"h\": 100.0, \"rot\": 0, \"grp\": 0 } ] }\n"
            "  ],\n  \"text_boxes\": [\n  ],\n  \"annots\": [\n  ],\n  \"ref_notes\": [\n  ],\n  \"groups\": [\n  ]\n}\n";
        std::string tmp2 = (fs::temp_directory_path() / "scholion_selftest_overflow.scholion").string();
        FILE* f = fopen(tmp2.c_str(), "w");
        if (f) { fwrite(json.data(), 1, json.size(), f); fclose(f); }
        load_project_from_path(tmp2);   // must not crash
        CHECK(g_documents.size() == 1, "over-long path: doc count %zu != 1 (placeholder expected)",
              g_documents.size());
        fs::remove(tmp2);
    }

    // ----- Migration: pre-v1.4 positional ref_notes land on the right highlight ----
    // Two references; the legacy note is keyed to the SECOND (after: 1). After load it
    // must sit on the second highlight, not the first.
    {
        std::string json =
            "{\n  \"format\": 1,\n"
            "  \"viewport\": { \"x\": 0.0, \"y\": 0.0, \"zoom\": 0.6 },\n"
            "  \"documents\": [\n"
            "    { \"path\": \"/nonexistent/scholion_selftest_mig.pdf\", \"stack_origin\": [0.0, 0.0],\n"
            "      \"pages\": [ { \"index\": 0, \"x\": 0.0, \"y\": 0.0, \"w\": 100.0, \"h\": 100.0, \"rot\": 0, \"grp\": 0 } ] }\n"
            "  ],\n  \"text_boxes\": [\n  ],\n"
            "  \"annots\": [\n"
            "    { \"doc\": 0, \"page\": 0, \"hl\": [0.1, 0.1, 0.2, 0.2], \"ht\": \"first quote\" },\n"
            "    { \"doc\": 0, \"page\": 0, \"hl\": [0.3, 0.3, 0.4, 0.4], \"ht\": \"second quote\" }\n"
            "  ],\n"
            "  \"ref_notes\": [\n    { \"after\": 1, \"text\": \"note for second\" }\n  ],\n"
            "  \"groups\": [\n  ]\n}\n";
        std::string tmp3 = (fs::temp_directory_path() / "scholion_selftest_migrate.scholion").string();
        FILE* f = fopen(tmp3.c_str(), "w");
        if (f) { fwrite(json.data(), 1, json.size(), f); fclose(f); }
        load_project_from_path(tmp3);
        if (g_documents.size() == 1 && g_documents[0].pages.size() == 1 &&
            g_documents[0].pages[0].annots.highlights.size() == 2) {
            const auto& hls = g_documents[0].pages[0].annots.highlights;
            CHECK(hls[0].note.empty(), "migration: note wrongly attached to first reference [%s]",
                  hls[0].note.c_str());
            CHECK(hls[1].note == "note for second", "migration: note not on second reference [%s]",
                  hls[1].note.c_str());
        } else {
            CHECK(false, "migration: unexpected structure after load (%zu docs)", g_documents.size());
        }
        fs::remove(tmp3);
    }

    if (s_fail == 0) { printf("Self-test PASSED\n"); return 0; }
    printf("Self-test FAILED — %d check(s)\n", s_fail);
    return 1;
}
