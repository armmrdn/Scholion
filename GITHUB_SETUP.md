# GitHub Setup for Scholion

This guide walks you through setting up the Scholion repository on GitHub with automated CI/CD builds.

## Step 1: Remove clangd cache from git (one-time cleanup)

The clangd cache was accidentally included in the initial commit. Remove it:

```bash
cd /Users/armmrdn/Dropbox/Scholion
git rm -r --cached scholion/.cache/
git commit -m "Remove clangd cache from tracking

Clangd index cache should not be version-controlled.
It's now properly excluded via .gitignore.

Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>"
```

## Step 2: Create a new GitHub repository

1. Go to https://github.com/new
2. Create repository with:
   - **Repository name**: `Scholion` (or similar)
   - **Description**: "Minimalist PDF canvas workspace — macOS, Windows, Linux"
   - **Visibility**: Public (or Private if you prefer)
   - **DO NOT initialize with README** — we already have one
   - **DO NOT initialize with .gitignore** — we have one
   - Click "Create repository"

3. Copy the HTTPS URL (e.g., `https://github.com/yourusername/Scholion.git`)

## Step 3: Push to GitHub

```bash
cd /Users/armmrdn/Dropbox/Scholion
git remote add origin https://github.com/yourusername/Scholion.git
git branch -M main
git push -u origin main
```

You'll be prompted for your GitHub credentials (or use a Personal Access Token if you have 2FA enabled).

## Step 4: Verify CI/CD Workflows

Once pushed:

1. Go to your GitHub repo: https://github.com/yourusername/Scholion
2. Click **Actions** tab
3. Workflows should appear: `build-macos.yml` and `build-windows.yml`
4. They may show as pending or running on the first push

Watch the workflows in the Actions tab to see:
- **macOS builds**: Compile for x86_64 and arm64, create DMG artifacts
- **Windows builds**: Compile for x64, create EXE artifact

## Step 5: Setting up automatic releases (optional)

To make GitHub automatically create releases when you tag commits:

```bash
# After making improvements, create a version tag
git tag -a v0.1.0 -m "Initial release — full-featured macOS + Windows ports"
git push origin v0.1.0
```

The workflows detect tags matching `refs/tags/` and automatically create GitHub Releases with:
- Compiled `Scholion.dmg` (macOS)
- Compiled `Scholion.exe` (Windows)

## Troubleshooting CI/CD

### macOS workflow fails: "GLAD not found"
The GLAD setup is already in place at `scholion-win/third_party/glad/`. If needed, regenerate from https://glad.dav1d.de/ (OpenGL 3.3 Core, with loader).

### Windows workflow fails: "MuPDF libs not found"
Windows builds require MuPDF static libraries. See `scholion-win/third_party/mupdf/lib/PLACE_WINDOWS_LIBS_HERE.txt` for setup instructions.

Place `.lib` files in:
- `scholion-win/third_party/mupdf/lib/mupdf.lib`
- `scholion-win/third_party/mupdf/lib/mupdf-third.lib`

Then recommit and push.

### DMG creation fails on macOS
If `hdiutil` fails, ensure `dist/` directory exists and contains `Scholion.app`. The workflow creates this automatically — if it fails, check macOS build step output.

## Future workflow: pushing updates

From now on, workflow is simple:

1. Make code changes locally
2. Test locally: `cd scholion/build && make -j$(sysctl -n hw.ncpu)`
3. Commit: `git commit -m "Description of changes"`
4. Push: `git push origin main`
5. GitHub Actions automatically builds macOS + Windows
6. Check Actions tab for build status
7. When ready to release, tag: `git tag -a v0.2.0 && git push origin v0.2.0`

## Restore point

Before moving to the Linux refactor session, save the current state:

```bash
cd /Users/armmrdn/Dropbox
tar --exclude='.DS_Store' --exclude='scholion/.cache' --exclude='scholion-win/build' \
  -czf scholion-github-ready.tar.gz Scholion/
```

This tarball contains the clean, GitHub-ready source tree.

---

Once GitHub is set up and working, you're ready to begin the Linux refactoring session in the next Claude session.
