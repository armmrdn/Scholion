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
#include "undo.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

static int s_fail = 0;

#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); ++s_fail; } } while (0)

static bool approx(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// --- Undo coverage (headless) ----------------------------------------------
// Drives the REAL push_undo/undo_last on placeholder documents (no GL/MuPDF), covering the
// data-model operations that the 1.5 stable-ID refactor will migrate off raw Page*. Each
// case pushes one record then undoes it, so the shared undo stack stays balanced.

static void reset_docs_for_undo() {
    g_documents.clear();
    g_loaders.clear();
    g_text_boxes.clear();
    g_groups.clear();
    g_canvas_strokes.clear();
    g_next_box_id   = 0;
    g_next_group_id = 1;
    auto add = [&](const char* path, int n) {
        Document d; d.path = path;
        for (int i = 0; i < n; ++i) {
            Page p; p.id = g_next_page_id++;   // undo records reference pages by id
            p.page_index = i;
            p.world_pos = {100.0f * i, 50.0f * i};
            p.world_w = 600.0f; p.world_h = 800.0f;
            d.pages.push_back(p);
        }
        g_documents.push_back(std::move(d));
        g_loaders.push_back(nullptr);   // keep g_loaders in lockstep with g_documents
    };
    add("/nonexistent/undo_a.pdf", 2);
    add("/nonexistent/undo_b.pdf", 1);
    g_input.set_documents(&g_documents);
}

static void test_undo() {
    printf("Scholion self-test — undo of data-model operations\n");

    reset_docs_for_undo();
    { // PageMove
        Page* p = &g_documents[0].pages[0];
        Vec2 old = p->world_pos;
        UndoRecord r; r.type = UndoRecord::Type::PageMove; r.page_moves.push_back({p->id, old});
        push_undo(r);
        p->world_pos = {999.0f, 999.0f};
        undo_last();
        CHECK(approx(p->world_pos.x, old.x, 0.001f) && approx(p->world_pos.y, old.y, 0.001f),
              "undo PageMove did not restore position");
    }

    reset_docs_for_undo();
    { // PageRotate (with the odd-turn w/h swap)
        Page* p = &g_documents[0].pages[0];
        int old_rot = p->rotation; float ow = p->world_w, oh = p->world_h;
        UndoRecord r; r.type = UndoRecord::Type::PageRotate;
        r.page_rots.push_back({p->id, old_rot, ow, oh});
        push_undo(r);
        p->rotation = (p->rotation + 90) % 360; std::swap(p->world_w, p->world_h);
        undo_last();
        CHECK(p->rotation == old_rot && approx(p->world_w, ow, 0.01f) && approx(p->world_h, oh, 0.01f),
              "undo PageRotate did not restore rotation/size");
    }

    reset_docs_for_undo();
    { // DocScale
        auto& pages = g_documents[0].pages;
        UndoRecord r; r.type = UndoRecord::Type::DocScale;
        for (auto& pg : pages) r.page_moves.push_back({pg.id, pg.world_pos, pg.world_w, pg.world_h});
        push_undo(r);
        for (auto& pg : pages) { pg.world_pos.x *= 2; pg.world_w *= 2; pg.world_h *= 2; }
        undo_last();
        CHECK(approx(pages[0].world_w, 600.0f, 0.01f), "undo DocScale did not restore size");
    }

    reset_docs_for_undo();
    { // Group / Ungroup / GroupJoin
        Page* p = &g_documents[0].pages[0];
        UndoRecord g; g.type = UndoRecord::Type::Group;
        PageGroup row; row.id = 1; row.name = "T"; g.group_row = row;
        g.group_members.push_back({p->id, p->group_id});
        p->group_id = 1; g_groups.push_back(row);
        push_undo(g); undo_last();
        CHECK(p->group_id == 0 && g_groups.empty(), "undo Group did not revert membership + row");

        PageGroup row2; row2.id = 2; row2.name = "T2";
        p->group_id = 2; g_groups.push_back(row2);
        UndoRecord u; u.type = UndoRecord::Type::Ungroup; u.group_row = row2;
        u.group_members.push_back({p->id, 2});
        p->group_id = 0; g_groups.pop_back();
        push_undo(u); undo_last();
        CHECK(p->group_id == 2 && !g_groups.empty(), "undo Ungroup did not restore membership + row");

        int before = p->group_id;
        UndoRecord j; j.type = UndoRecord::Type::GroupJoin;
        j.group_members.push_back({p->id, before});
        p->group_id = 99;
        push_undo(j); undo_last();
        CHECK(p->group_id == before, "undo GroupJoin did not revert membership");
    }

    reset_docs_for_undo();
    { // TextBoxCreate / TextBoxDelete
        CanvasTextBox b; b.id = g_next_box_id++; b.world_pos = {10, 10};
        snprintf(b.text, sizeof(b.text), "x");
        g_text_boxes.push_back(b);
        UndoRecord c; c.type = UndoRecord::Type::TextBoxCreate; c.box_id = b.id;
        push_undo(c); undo_last();
        CHECK(g_text_boxes.empty(), "undo TextBoxCreate did not remove the box");

        CanvasTextBox b2; b2.id = g_next_box_id++; snprintf(b2.text, sizeof(b2.text), "y");
        UndoRecord d; d.type = UndoRecord::Type::TextBoxDelete; d.deleted_box = b2;
        push_undo(d); undo_last();
        CHECK(g_text_boxes.size() == 1 && std::strcmp(g_text_boxes[0].text, "y") == 0,
              "undo TextBoxDelete did not restore the box");
    }

    reset_docs_for_undo();
    { // Annotation (Note flag)
        Page* p = &g_documents[0].pages[0];
        p->annots.notes.push_back({"A"});
        UndoRecord r; r.type = UndoRecord::Type::Note;
        r.doc_idx = 0; r.page_idx = p->page_index; r.note_idx_before = g_next_note_idx;
        push_undo(r); undo_last();
        CHECK(p->annots.notes.empty(), "undo Note did not remove the flag");
    }

    reset_docs_for_undo();
    { // DocumentRemove: a pushed record referencing the removed doc's page must be scrubbed,
      // and a later undo must not crash (the exact raw-Page* fragility Phase 1 replaces).
        Page* p = &g_documents[1].pages[0];
        UndoRecord r; r.type = UndoRecord::Type::PageMove; r.page_moves.push_back({p->id, p->world_pos});
        push_undo(r);
        size_t before = g_documents.size();
        remove_document(1);
        CHECK(g_documents.size() == before - 1, "remove_document did not drop the document");
        undo_last();   // must be safe: the dangling record was scrubbed
        CHECK(g_documents.size() == before - 1, "undo after remove_document corrupted the doc set");
    }

    // Leave globals clean for anything after.
    g_documents.clear(); g_loaders.clear();
    g_text_boxes.clear(); g_groups.clear();
    g_input.set_documents(nullptr);
}

// --- Malformed / corrupt project-file hardening ------------------------------
// Guards the failure modes a hand-edited, truncated, or corrupted .scholion can trigger:
//   1. Duplicate page ids — page_by_id() returns the FIRST match, so a duplicate would silently
//      aim undo records, selection, and the renderer at the wrong page. Load must make ids unique.
//   2. Non-finite numbers — picojson's value(double) THROWS std::overflow_error on Inf/NaN, both
//      when PARSING a file containing one (e.g. "1e999" overflows via strtod) and when
//      SERIALIZING one out of memory. Neither may abort the app.
//   3. A failed parse must not destroy the currently-open project.
static void test_project_validation() {
    printf("Scholion self-test — malformed project-file hardening\n");
    namespace fs = std::filesystem;

    auto write_tmp = [](const char* name, const std::string& json) {
        std::string p = (fs::temp_directory_path() / name).string();
        FILE* f = fopen(p.c_str(), "w");
        if (f) { fwrite(json.data(), 1, json.size(), f); fclose(f); }
        return p;
    };

    // ----- Duplicate page ids must be reassigned to unique ones -----
    {
        std::string json =
            "{\n  \"format\": 1,\n"
            "  \"viewport\": { \"x\": 0.0, \"y\": 0.0, \"zoom\": 0.6 },\n"
            "  \"documents\": [\n"
            "    { \"path\": \"/nonexistent/scholion_selftest_dup.pdf\", \"stack_origin\": [0.0, 0.0],\n"
            "      \"pages\": [\n"
            "        { \"index\": 0, \"x\": 0.0,  \"y\": 0.0, \"w\": 100.0, \"h\": 100.0, \"rot\": 0, \"grp\": 0, \"id\": 42 },\n"
            "        { \"index\": 1, \"x\": 10.0, \"y\": 0.0, \"w\": 100.0, \"h\": 100.0, \"rot\": 0, \"grp\": 0, \"id\": 42 }\n"
            "      ] }\n"
            "  ],\n  \"text_boxes\": [\n  ],\n  \"annots\": [\n  ],\n  \"groups\": [\n  ]\n}\n";
        std::string tmp = write_tmp("scholion_selftest_dupid.scholion", json);
        load_project_from_path(tmp);
        if (g_documents.size() == 1 && g_documents[0].pages.size() == 2) {
            uint64_t a = g_documents[0].pages[0].id, b = g_documents[0].pages[1].id;
            CHECK(a != b, "duplicate page ids not made unique (both %llu)", (unsigned long long)a);
            CHECK(a != 0 && b != 0, "page id left as 0 after load");
        } else {
            CHECK(false, "dup-id: unexpected structure after load (%zu docs)", g_documents.size());
        }
        fs::remove(tmp);
    }

    // ----- A file containing Inf must be rejected, not abort — and must not wipe the open project -----
    {
        std::string json =
            "{\n  \"format\": 1,\n"
            "  \"viewport\": { \"x\": 0.0, \"y\": 0.0, \"zoom\": 0.6 },\n"
            "  \"documents\": [\n"
            "    { \"path\": \"/nonexistent/scholion_selftest_inf.pdf\", \"stack_origin\": [0.0, 0.0],\n"
            "      \"pages\": [ { \"index\": 0, \"x\": 1e999, \"y\": 0.0, \"w\": 100.0, \"h\": 100.0, \"rot\": 0, \"grp\": 0 } ] }\n"
            "  ],\n  \"text_boxes\": [\n  ],\n  \"annots\": [\n  ],\n  \"groups\": [\n  ]\n}\n";
        std::string tmp = write_tmp("scholion_selftest_inf.scholion", json);
        size_t docs_before = g_documents.size();
        load_project_from_path(tmp);   // must not throw/abort
        CHECK(g_documents.size() == docs_before,
              "failed parse wiped the open project (%zu docs, expected %zu)",
              g_documents.size(), docs_before);
        fs::remove(tmp);
    }

    // ----- A non-finite value in memory must not abort the save -----
    if (!g_documents.empty() && !g_documents[0].pages.empty()) {
        g_documents[0].pages[0].world_pos.x = INFINITY;
        g_documents[0].pages[0].world_pos.y = NAN;
        std::string tmp = (fs::temp_directory_path() / "scholion_selftest_nonfinite.scholion").string();
        CHECK(save_to_path(tmp), "save with non-finite coords failed");   // must not throw
        load_project_from_path(tmp);
        if (!g_documents.empty() && !g_documents[0].pages.empty()) {
            const Page& p = g_documents[0].pages[0];
            CHECK(std::isfinite(p.world_pos.x) && std::isfinite(p.world_pos.y),
                  "non-finite coords survived the save/load clamp");
            CHECK(std::isfinite(p.world_w) && p.world_w > 0.0f &&
                  std::isfinite(p.world_h) && p.world_h > 0.0f, "page size not sane after load");
        }
        fs::remove(tmp);
    }
}

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

    uint64_t next_id = 1001;
    auto make_doc = [&](const char* path, int npages, float ox, float oy) {
        Document d; d.path = path;
        for (int i = 0; i < npages; ++i) {
            Page p;
            p.id         = next_id++;   // explicit stable ids to verify they round-trip
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
        CHECK(p00.id == 1001 && g_documents[0].pages[1].id == 1002 && g_documents[1].pages[0].id == 1003,
              "page ids not restored (%llu, %llu, %llu)",
              (unsigned long long)p00.id,
              (unsigned long long)g_documents[0].pages[1].id,
              (unsigned long long)g_documents[1].pages[0].id);
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

    // ----- Malformed / corrupt project-file hardening -----
    test_project_validation();

    // ----- Undo of data-model operations -----
    test_undo();

    if (s_fail == 0) { printf("Self-test PASSED\n"); return 0; }
    printf("Self-test FAILED — %d check(s)\n", s_fail);
    return 1;
}
