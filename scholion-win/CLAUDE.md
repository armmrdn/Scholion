# Scholion — Windows Port

## Overview

Windows build of Scholion. The entire application logic lives in the shared
source tree at `../scholion/src/` and `../scholion/include/`. This directory
contains only Windows-specific files: the CMakeLists.txt, platform source stub,
resources, and Windows-only third-party libraries.

**Platform strategy:**
- `../scholion/`       — macOS build (canonical source, contains all shared `.cpp/.h`)
- `../scholion-win/`   — Windows build (this directory)
- `../scholion-lnx/`   — Linux build

All source-level bug fixes and features land in `../scholion/src/`. Platform
conditionals (`#ifdef __APPLE__` / `#ifdef _WIN32` / `#else`) inside those files
make them compile correctly on all three platforms without any changes to this
directory.

---

## Directory Layout

```
scholion-win/
  CMakeLists.txt           — Windows build system
  src/
    platform_win.cpp       — Windows-specific init (currently a stub)
  resources/
    scholion.rc            — Windows resource file (icon + version info)
    AppIcon.ico            — App icon (MISSING: see README_icon.md)
    README_icon.md         — How to generate AppIcon.ico from macOS assets
  third_party/
    mupdf/lib/             — Windows .lib files (MISSING: see instructions there)
    glad/                  — OpenGL 3.3 loader (MISSING: see SETUP_GLAD.md)
  build/                   — CMake build output (created by cmake/make)
```

---

## Prerequisites (one-time Windows setup)

### 1. Build tools
Install one of:
- **Visual Studio 2022 Community** with "Desktop development with C++" workload
- **MSYS2/MinGW-w64**: download from https://www.msys2.org/ — then:
  ```
  pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake
  ```

### 2. CMake 3.20+
Either bundled with Visual Studio, or download from https://cmake.org/download/

### 3. GLFW
- **vcpkg**: `vcpkg install glfw3:x64-windows`
- **MSYS2**: `pacman -S mingw-w64-x86_64-glfw`
- Or download prebuilt from https://www.glfw.org/download.html

### 4. libcurl
- **vcpkg**: `vcpkg install curl:x64-windows`
- **MSYS2**: `pacman -S mingw-w64-x86_64-curl`

### 5. GLAD (OpenGL loader)
See `third_party/glad/SETUP_GLAD.md` — generate from https://glad.dav1d.de/
(OpenGL 3.3, Core profile, with loader).

### 6. MuPDF Windows static libs
See `third_party/mupdf/lib/PLACE_WINDOWS_LIBS_HERE.txt`.
Build from source or use a prebuilt release matching the bundled headers.

### 7. App icon
See `resources/README_icon.md`. Convert the macOS `AppIcon.icns` to `AppIcon.ico`
and place it in `resources/`.

---

## Build Instructions

### With MSYS2/MinGW (recommended for first port attempt — closer to Clang)
```bash
# From MSYS2 MinGW 64-bit shell
cd /path/to/Scholion/scholion-win
mkdir build && cd build
cmake .. -G "MinGW Makefiles" \
         -DCMAKE_BUILD_TYPE=Release \
         -DSCHOLION_WITH_MUPDF=ON
mingw32-make -j$(nproc)
```

### With Visual Studio 2022
```bash
# From Developer Command Prompt (x64)
cd C:\path\to\Scholion\scholion-win
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 \
         -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
         -DSCHOLION_WITH_MUPDF=ON
cmake --build . --config Release
```

The output is `build/Release/Scholion.exe` (MSVC) or `build/Scholion.exe` (MinGW).

---

## Sync Policy

When modifying the application:
1. **Bug fixes / new features** → edit `../scholion/src/*.cpp` or `../scholion/include/*.h`.
   The Windows build picks them up automatically (it compiles from that path).
2. **New source file** → add its bare filename to `../scholion/cmake/sources.cmake` **only**.
   All three build trees `include()` that one list and prepend their own path prefix, so a new
   shared `.cpp` is a ONE-LINE change in ONE file. Do **not** edit the three CMakeLists separately
   (that was the pre-1.5 workflow).
3. **Windows-specific code** → use `#ifdef _WIN32` in the shared source file, or
   add to `src/platform_win.cpp` if it's large enough to warrant separation.
4. **macOS-specific code** → use `#ifdef __APPLE__`.
5. **Linux code** → use `#else` (the fall-through after Apple/Windows guards), or add
   to `../scholion-lnx/src/platform_linux.cpp`.

---

## Known Gaps / TODO

- [x] **AppIcon.ico** — generated from macOS ICNS (16/32/48/64/128/256 px, all in one file)
- [x] **GLAD** — generated for OpenGL 3.3 Core; source in `third_party/glad/`
- [x] **DPI awareness** — `Scholion.manifest` with `PerMonitorV2`; embedded via `scholion.rc`
- [x] **MuPDF Windows libs** — built from source in CI; cached by `actions/cache`
- [x] **Distribution** — `Scholion-Windows.zip` with `Scholion.exe` + all MinGW DLLs, produced by CI on every `v*` tag
- [ ] **File association** — register `.scholion` in HKEY_CLASSES_ROOT via installer
      (`argv[1]` path handling already works; association just makes double-click work in Explorer)

---

## CI Build

The Windows build runs in CI via `.github/workflows/build.yml` using an MSYS2/MinGW64 environment on `windows-2022`. MuPDF is built from source during CI (cached by `actions/cache`). GLFW is installed via MSYS2 packages. The final artifact is `Scholion-Windows.zip` — `Scholion.exe` bundled with all required MinGW DLLs.

`build-windows` runs in parallel with `build-linux` after `build-macos` completes, so the draft release already exists when this job uploads its zip. The `publish` job waits for both Windows and Linux before making the release live.

## History

The project is on GitHub (`armmrdn/Scholion`, private). Use `git log` and `git checkout` for history.

Full archive of every plan, decision, removal, and idea lives in `../DEVLOG.md` (local-only, never committed).

## Honesty rules (read every turn)

Before claiming a function, class, or import exists, verify it by reading
the file or running a grep. Never fabricate symbols.

If you cannot verify something, say "I haven't verified this" explicitly.
Do not write code that depends on the unverified claim.

If a task asks you to use a library you've never seen referenced in this
project, ask before adding it.

If a task involved tests or builds, do not claim success unless you
actually ran the test or build command in this session.

Never invent error messages, API responses, or stack traces. If you
didn't see them, say so.

When you genuinely don't know, the correct answer is "I don't know" or
"I need to check first." Both are better than a confident guess.

## Verification protocol

Before writing or editing code that uses a symbol (function, class, type,
constant), do one of:

1. Read the file where it's defined and confirm the signature
2. Run `grep -r "symbolName" .` or use the Glob tool to find it
3. Check package.json, requirements.txt, Cargo.toml, or equivalent for
   the dependency

If you skip verification, prefix the code with a comment:
`// UNVERIFIED: I have not confirmed this symbol exists`

Plan-then-execute mode is preferred for any task touching more than one
file. Use Shift+Tab to enter plan mode before starting.
