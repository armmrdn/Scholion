# Creating AppIcon.ico for Windows

The Windows build embeds `resources/AppIcon.ico` into the executable via `scholion.rc`.
The macOS source has `resources/AppIcon.icns` — convert it to `.ico` using one of:

## Option A — ImageMagick (fastest, any platform)
```bash
# Install: brew install imagemagick  |  winget install ImageMagick
magick convert AppIcon.icns \
    -define icon:auto-resize=256,128,64,48,32,16 \
    AppIcon.ico
```

## Option B — Online converter
Upload `AppIcon.icns` (or any PNG from the macOS source) to https://convertio.co or
https://cloudconvert.com and download the `.ico` result.

## Option C — From PNG sources
If you have the original PNG assets:
```bash
magick convert icon_256.png icon_128.png icon_64.png icon_32.png icon_16.png AppIcon.ico
```

## Recommended sizes
Windows Explorer shows 256×256 (large icons), 48×48 (medium), 32×32 (small), 16×16
(list view). Including all four in the `.ico` avoids scaling artefacts.

Once generated, place `AppIcon.ico` in this directory (`scholion-win/resources/`)
and re-run the build.
