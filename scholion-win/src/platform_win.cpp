// platform_win.cpp — Windows-specific initialisation and hooks.
//
// Most platform logic (reveal_in_file_manager, recents_file_path, etc.) lives
// directly in ../scholion/src/main.cpp behind #ifdef _WIN32 guards so it stays
// close to the call sites and is easy to keep in sync across platforms.
//
// This file is the right place for anything that:
//   - requires Windows-specific linker symbols (DllMain, WinMain override, etc.)
//   - needs to run before main() via global constructors
//   - is too bulky to embed inline in main.cpp

// Currently a stub — expand as needed for the Windows port.
