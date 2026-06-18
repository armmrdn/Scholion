#pragma once
#include "canvas.h"
#include "canvas_text_box.h"
#include "document.h"
#include "pdf_loader.h"
#include <memory>
#include <string>
#include <vector>

struct UndoRecord {
    enum class Type {
        PenStroke, Highlight, Note,
        PageMove, PageResize,
        TextBoxCreate, TextBoxMove, TextBoxDelete,
        TextBoxEdit, TextBoxStyle,
        ErasedStroke, ErasedHighlight,
        ErasedNote,
        DocumentRemove
    };
    Type type = Type::PenStroke;

    int doc_idx         = -1;
    int page_idx        = -1;
    int note_idx_before = -1;

    struct PagePos { Page* page; Vec2 old_pos; float old_w = 0.0f; float old_h = 0.0f; };
    std::vector<PagePos> page_moves;

    int           box_id      = -1;
    Vec2          old_box_pos = {};
    CanvasTextBox deleted_box = {};
    std::string   prev_text;
    float old_r = 0, old_g = 0, old_b = 0, old_fs = 0;

    AnnotStroke    erased_stroke    = {};
    AnnotHighlight erased_highlight = {};
    AnnotNote      erased_note      = {};
    int            erased_note_at   = -1;

    int                        removed_doc_idx = -1;
    Document                   removed_doc     = {};
    std::shared_ptr<PdfLoader> removed_loader;
};

void push_undo(UndoRecord r);
