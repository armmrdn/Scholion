# Scholion

A canvas-style PDF workspace. Open multiple PDFs, arrange their pages freely on an infinite canvas, annotate, highlight, and save your layout as a project file.

Designed and built by @ARMMRDN (2026).

---

## Download

Pre-built binaries are on the [Releases](../../releases) page.

| Platform | Download |
|----------|----------|
| macOS (Apple Silicon + Intel universal) | `Scholion.dmg` |
| Windows (x64) | `Scholion-Windows.zip` |

---

## Platform Status

| Platform | Status |
|----------|--------|
| macOS | Fully functional — universal binary (arm64 + x86_64) |
| Windows | Fully functional — MinGW64 build, bundled DLLs |
| Linux | Not started |

---

## Building from Source

### macOS

```bash
brew install glfw

cmake -S scholion -B scholion/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSCHOLION_WITH_MUPDF=ON
cmake --build scholion/build -j$(sysctl -n hw.ncpu)
open scholion/build/Scholion.app
```

### Windows

```bash
# From MSYS2 MinGW 64-bit shell
cmake -S scholion-win -B scholion-win/build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSCHOLION_WITH_MUPDF=ON
cmake --build scholion-win/build
```

MuPDF static libs must be placed in `scholion-win/third_party/mupdf/lib/` before building. See `scholion-win/CLAUDE.md` for the full setup guide.

---

## Project Structure

```
scholion/           macOS build + all shared cross-platform source
scholion-win/       Windows CMake overlay + platform stubs
.github/workflows/  CI/CD
RELEASE_NOTES.md    Release notes — embedded into each release by CI
```

All application logic lives in `scholion/src/` and `scholion/include/`. Both platforms compile from the same files; only platform entry points and file-dialog calls differ via `#ifdef`.

---

## CI/CD

A single workflow (`.github/workflows/build.yml`) handles both platforms.

**On push to `main` or `develop`:** both builds run as a compile check — no release is created.

**On a version tag (`v*`):** builds run sequentially. macOS finishes first and creates a draft release with the DMG and the contents of `RELEASE_NOTES.md` attached. Windows uploads its zip to that draft. A final publish job makes the release live. No artifact storage is used.

To cut a release:
1. Update `RELEASE_NOTES.md` and push to `main`
2. Create the version tag via the GitHub API (tags cannot be pushed directly due to branch protection):
```bash
gh api repos/armmrdn/Scholion/git/refs \
  --method POST \
  --field ref="refs/tags/vX.Y" \
  --field sha="$(gh api repos/armmrdn/Scholion/git/refs/heads/main --jq '.object.sha')"
```

> **Important:** once a tag name has been used with a published release, GitHub locks it permanently — even after deleting both the release and the tag ref. Always use a new tag name for each release.

---

## License

© 2026 ARMMRDN. All rights reserved. Closed binary distribution only.

MuPDF is AGPL licensed. For commercial closed-source distribution, replace with PDFium.
