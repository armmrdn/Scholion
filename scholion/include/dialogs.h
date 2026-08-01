#pragma once
// App-shell modal dialogs: the bare-launch startup chooser (open project / add PDFs / add folder /
// blank) and the quit-confirmation dialog. Their control flags are shared with main.cpp (which
// raises them from the window-close/signal path and the empty-launch path, and reads g_quit_state
// in the frame loop to actually exit). Implementation in src/dialogs.cpp.
// (The "Add from URL" modal stays in main.cpp with its download subsystem.)

enum class QuitState { None, Waiting, Confirmed };

void draw_startup_chooser();   // no-op unless g_startup_chooser; call once per frame
void draw_quit_dialog();       // no-op unless g_quit_requested; call once per frame

extern bool      g_startup_chooser;   // show the bare-launch chooser modal
extern bool      g_quit_requested;    // a quit was requested (window close / signal)
extern QuitState g_quit_state;        // quit-confirmation state machine (frame loop reads Confirmed)
