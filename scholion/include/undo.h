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
        PageMove, PageResize, PageRotate, DocScale,
        TextBoxCreate, TextBoxMove, TextBoxDelete,
        TextBoxEdit, TextBoxStyle,
        ErasedStroke, ErasedHighlight,
        ErasedNote,
        DocumentRemove,
        CanvasStroke, ErasedCanvasStroke,
        Group, Ungroup, GroupJoin
    };
    Type type = Type::PenStroke;

    int doc_idx         = -1;
    int page_idx        = -1;
    int note_idx_before = -1;

    struct PagePos { Page* page; Vec2 old_pos; float old_w = 0.0f; float old_h = 0.0f; };
    std::vector<PagePos> page_moves;

    struct PageRot { Page* page; int old_rot; float old_w; float old_h; };
    std::vector<PageRot> page_rots;

    int           box_id      = -1;
    Vec2          old_box_pos = {};
    CanvasTextBox deleted_box = {};
    std::string   prev_text;
    float old_r = 0, old_g = 0, old_b = 0, old_fs = 0;

    AnnotStroke    erased_stroke    = {};
    int            canvas_idx       = -1;   // index for ErasedCanvasStroke restore
    AnnotHighlight erased_highlight = {};
    AnnotNote      erased_note      = {};
    int            erased_note_at   = -1;

    int                        removed_doc_idx = -1;
    Document                   removed_doc     = {};
    std::shared_ptr<PdfLoader> removed_loader;

    // Page grouping (Group / Ungroup). group_members records each affected page and
    // its group_id before the change; group_row is the group added (Group) or removed
    // (Ungroup) from g_groups. Undo reverts the members and reverses the table change.
    struct GroupMember { Page* page; int old_group; };
    std::vector<GroupMember> group_members;
    PageGroup                group_row = {};
};

void push_undo(UndoRecord r);
