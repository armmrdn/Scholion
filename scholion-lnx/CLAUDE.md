# Scholion — Linux Port

> Full archive of every plan, decision, removal, and idea lives in `../DEVLOG.md` (local-only, never committed).

## Overview

Linux build of Scholion. The entire application logic lives in the shared
source tree at `../scholion/src/` and `../scholion/include/`. This directory
contains only Linux-specific files: the CMakeLists.txt, platform source stub,
and Linux-only third-party libraries.

**Platform strategy:**
- `../scholion/`      — macOS build (canonical source, contains all shared `.cpp/.h`)
- `../scholion-win/`  — Windows build
- `../scholion-lnx/`  — Linux build (this directory)

All source-level bug fixes and features land in `../scholion/src/`. Platform
conditionals (`#ifdef __APPLE__` / `#ifdef _WIN32` / `#else`) inside those files
make them compile correctly on all three platforms without changes to this directory.

**GLAD** is shared with the Windows build tree at `../scholion-win/third_party/glad/`.
No separate copy is needed — the CMakeLists.txt references that path directly.

---

## Directory Layout

```
scholion-lnx/
  CMakeLists.txt           — Linux build system
  src/
    platform_linux.cpp     — Linux-specific init (currently a stub)
  third_party/
    mupdf/lib/             — Linux .a files (MISSING: see instructions there)
  build/                   — CMake build output (created by cmake)
```

---

## Prerequisites (one-time Linux setup)

### 1. Build tools
```
sudo apt install build-essential cmake ninja-build
```

### 2. System libraries
```
sudo apt install libgl1-mesa-dev xorg-dev libcurl4-openssl-dev
```

### 3. GLFW
Either build from source (see build instructions) or install via apt:
```
sudo apt install libglfw3-dev
```
Building from source is recommended for CI to ensure a static lib.

### 4. MuPDF Linux static libs
See `third_party/mupdf/lib/PLACE_LINUX_LIBS_HERE.txt`.

---

## Build Instructions

```bash
# From repo root — after installing prerequisites
cmake -S scholion-lnx -B scholion-lnx/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSCHOLION_WITH_MUPDF=ON
cmake --build scholion-lnx/build -j$(nproc)

./scholion-lnx/build/Scholion
```

---

## Sync Policy

When modifying the application:
1. **Bug fixes / new features** → edit `../scholion/src/*.cpp` or `../scholion/include/*.h`.
   The Linux build picks them up automatically (it compiles from that path).
2. **New source file** → add its bare filename to `../scholion/cmake/sources.cmake` **only**.
   All three build trees `include()` that one list and prepend their own path prefix, so a new
   shared `.cpp` is a ONE-LINE change in ONE file. Do **not** edit the three CMakeLists separately
   (that was the pre-1.5 workflow).
3. **Linux-specific code** → use `#else` in the shared source file (after the
   `__APPLE__` and `_WIN32` guards), or add to `src/platform_linux.cpp` if large.
4. **macOS-specific code** → use `#ifdef __APPLE__`.
5. **Windows-specific code** → use `#ifdef _WIN32`.

---

## CI Build

The Linux build runs in CI via `.github/workflows/build.yml` on `ubuntu-22.04`.
MuPDF and GLFW are both built from source during CI (cached by `actions/cache`).
The Linux job runs in parallel with `build-windows` after `build-macos` completes.
The final artifact is `Scholion-Linux.zip` containing the `Scholion` binary.

Runtime dependencies on the user's system:
- `libcurl4` (dynamically linked)
- `libGL` / Mesa (`libgl1-mesa-glx`)
- GLFW is statically linked — no runtime dep

---

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
3. Check CMakeLists.txt for the dependency

If you skip verification, prefix the code with a comment:
`// UNVERIFIED: I have not confirmed this symbol exists`
