#pragma once
// Page groups — ad-hoc, nondestructive clusters of pages (possibly across
// documents) that share a colored boundary frame and can be moved as a unit.
// Membership lives on Page::group_id; g_groups (app_state.h) holds each group's
// color/label. Implementation in src/groups.cpp.
#include <vector>

struct Page;

// Selection / creation / dissolution (context-menu + keyboard driven).
std::vector<Page*> group_pages(int gid);      // all live pages with group_id == gid
void create_group_from_selection();           // group the selected pages (>=1)
void ungroup_group(int gid);                  // dissolve a group (nondestructive)
void remove_page_from_group(Page* p);         // drop one page from its group

// Hit-testing (mouse handling).
int  group_handle_at(float sx, float sy);     // group whose frame/label is under a point; 0 = none

// Per-frame drawing + interaction (render loop).
void draw_page_groups();                       // boundary rings + label tabs
void update_group_drag_add();                  // dwell-to-join a dragged page into a group
void draw_group_remove_flashes();              // fading outline as a page leaves a group

// Lifecycle / animation hooks.
void groups_reset();       // clear transient group UI state on new/loaded project
bool groups_animating();   // true while a remove-flash is still fading (drives redraw)
