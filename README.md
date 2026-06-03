# Scholion — PDF Canvas Workspace

A minimalist, high-performance PDF viewer and infinite-canvas workspace built with C++17, OpenGL, and MuPDF.

**Current Status:** macOS fully featured, Windows port in progress, Linux planned.

## Features

- **Infinite Canvas**: Pan and zoom pages freely in 2D space
- **Multi-Document Support**: Open multiple PDFs and arrange them on the canvas
- **Text Annotations**: Highlight, underline, strikethrough, and add notes
- **Full-Text Search**: Search across all loaded PDFs with instant navigation
- **Project Persistence**: Save and load projects with all page positions and annotations
- **Keyboard Navigation**: Complete keyboard control (Tab, Arrow keys, Space, Enter)
- **Smart LOD System**: Automatic Level-of-Detail rendering (72/150/300 DPI tiers) with VRAM budget enforcement

## Building

### macOS

```bash
cd scholion
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DSCHOLION_WITH_MUPDF=ON
make -j$(sysctl -n hw.ncpu)
./Scholion.app/Contents/MacOS/Scholion
```

### Windows

```bash
# From Visual Studio Developer Command Prompt (x64)
cd scholion-win
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DSCHOLION_WITH_MUPDF=ON
cmake --build . --config Release
Release\Scholion.exe
```

### Linux (coming soon)

## Dependencies

### macOS
- CMake 3.20+
- GLFW3 (`brew install glfw3`)
- libcurl (`brew install curl`)
- MuPDF development files
- Clang/LLVM (included with Xcode)

### Windows
- CMake 3.20+
- Visual Studio 2022 Community (or MSYS2/MinGW-w64)
- GLFW3 (via vcpkg or package manager)
- libcurl (via vcpkg or package manager)
- GLAD (OpenGL 3.3 Core loader — generate from https://glad.dav1d.de/)
- MuPDF static libraries (.lib files)

### Linux
- Same as macOS but via native package manager (apt, pacman, etc.)

## Project Structure

```
scholion/
  ├── src/           # Shared cross-platform source (C++17)
  ├── include/       # Headers
  ├── third_party/   # Dear ImGui, MuPDF, tinyfiledialogs, etc.
  ├── docs/          # Architecture, TODO, keymap docs
  ├── CMakeLists.txt # macOS build config
  └── build/         # Build artifacts (ignored by git)

scholion-win/
  ├── src/           # Windows-specific stubs
  ├── resources/     # Windows .rc, icons
  ├── third_party/   # GLAD, Windows MuPDF .libs
  ├── CMakeLists.txt # Windows build config
  └── build/         # Build artifacts (ignored by git)

scholion-linux/     # Linux port (coming soon)
  └── ...
```

## Keyboard Shortcuts

| Action | macOS | Windows |
|--------|-------|---------|
| Search | Cmd+F | Ctrl+F |
| Save | Cmd+S | Ctrl+S |
| Undo | Cmd+Z | Ctrl+Z |
| Redo | Cmd+Shift+Z | Ctrl+Shift+Z |
| Select All Pages | Cmd+A | Ctrl+A |
| Pan Canvas | Space + Drag | Space + Drag |
| Multi-Select | Cmd+Click | Ctrl+Click |
| Quit | Cmd+Q | Alt+F4 |
| Right-Click Menu | Right-Click | Right-Click |

## CI/CD

This repository includes GitHub Actions workflows that automatically build:
- **macOS**: DMG files for both x86_64 and arm64 architectures
- **Windows**: Standalone Scholion.exe with all dependencies

Builds are triggered on every push to `main` or `develop` branches and on pull requests. Artifacts are available in the Actions tab; tagged releases will automatically create GitHub Releases with binaries.

## Architecture

See `scholion/docs/ARCHITECTURE.md` for detailed information on:
- Coordinate systems (screen, world, page-normalized)
- Threading model (main thread vs. worker threads)
- Rasterization pipeline and LOD system
- Project file format (.scholion JSON schema)
- Undo/redo stack design
- Selection model

## Performance Notes

Scholion is optimized for modern hardware with:
- **VRAM Budget**: 350 MB enforced by automatic texture eviction
- **Rasterization**: Asynchronous background worker threads with MuPDF
- **Rendering**: Batched OpenGL draw calls with LOD-based texture selection
- **UI**: Dear ImGui with minimal overhead

## License

© 2026 ARMMRDN. All rights reserved.

## Contributing

This is a personal project. For feature requests or bug reports, please open an issue on GitHub.

---

**Next Steps:**
- [ ] Windows build tested and verified
- [ ] Linux port implementation
- [ ] Automated releases to GitHub
