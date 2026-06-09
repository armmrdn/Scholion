# Scholion

A canvas-style PDF review utility. Open multiple PDFs, arrange their pages freely on an infinite canvas, annotate, and save your layout as a project file.

Designed and built by @ARMMRDN (2026).

---

## Platform Status

| Platform | Status |
|----------|--------|
| macOS | Fully functional |
| Windows | Build system ready; CI validation pending |
| Linux | Not started |

---

## Building

### macOS

```bash
# Dependencies
brew install glfw

# Build
cd scholion
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DSCHOLION_WITH_MUPDF=ON
make -j$(sysctl -n hw.ncpu)
open Scholion.app
```

### Windows

The `scholion-win/` directory contains the Windows CMake configuration. It references the shared source in `scholion/src/` and uses Windows-specific stubs from `scholion-win/src/platform_win.cpp`. MuPDF static `.lib` files must be placed in `scholion-win/third_party/mupdf/lib/` before building.

CI/CD workflow: `.github/workflows/build-windows.yml`

---

## Project Structure

```
scholion/          macOS build + shared cross-platform source
scholion-win/      Windows CMake overlay + platform stubs
.github/workflows/ CI/CD (Windows build automation)
```

Shared C++ source lives in `scholion/src/` and `scholion/include/`. Both platforms compile from the same files; only platform-specific entry points differ.

---

## CI/CD

`build-windows.yml` triggers on push to `main` and builds `Scholion.exe`. A separate manual workflow (`build-mupdf-windows.yml`) rebuilds the MuPDF Windows static libraries when needed.

---

## License

© 2026 ARMMRDN. All rights reserved. For closed binary distribution.

MuPDF is AGPL licensed. For closed binary distribution, swap to PDFium.
