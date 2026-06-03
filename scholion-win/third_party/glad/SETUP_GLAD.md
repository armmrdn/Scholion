# Setting Up GLAD (OpenGL 3.3 Core Function Loader)

macOS exposes all OpenGL 3.3 symbols directly through `<OpenGL/gl3.h>`.
Windows only exposes OpenGL 1.1 via `<GL/gl.h>`; modern functions (3.x+) must
be loaded at runtime through a loader. GLAD is the standard lightweight solution.

## Step 1 — Generate GLAD files

Visit: https://glad.dav1d.de/

Set the following options EXACTLY:
  - Language:         C/C++
  - Specification:    OpenGL
  - API / gl:         Version 3.3
  - Profile:          Core
  - Extensions:       (leave blank — Scholion does not use extensions)
  - Options:          ✅ Generate a loader

Click "GENERATE" and download the ZIP.

## Step 2 — Extract into this directory

The ZIP contains:
  include/
    glad/
      glad.h
    KHR/
      khrplatform.h
  src/
    glad.c

Extract so the final layout is:
  scholion-win/third_party/glad/
    include/
      glad/
        glad.h          ← place here
      KHR/
        khrplatform.h   ← place here
    src/
      glad.c            ← place here

## Step 3 — Rebuild

Run cmake and build normally. The `GLAD_SRC` variable in CMakeLists.txt points to
`third_party/glad/src/glad.c`; the build will fail with a FATAL_ERROR if the file
is missing, guiding you back to this document.

## Why GLAD and not GLEW?

Both work. GLAD is preferred because:
- It generates only the functions you ask for (smaller binary).
- It is a single C file — trivially bundled.
- It has no runtime dependency on an external DLL.
- It is the standard choice for new GLFW-based projects.

## Verification

After building, if you see "Failed to initialise GLAD OpenGL loader" in the console
on startup, your GPU driver does not support OpenGL 3.3. Update the driver (on
discrete GPUs) or ensure your Windows GPU supports OpenGL 3.3 core profile.
Intel integrated graphics from 2012+ generally support it; very old or virtual GPUs
may not.
