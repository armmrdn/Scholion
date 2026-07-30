# Cross-platform shared source files (bare names, relative to scholion/src/).
# Each build tree includes this and prepends its own path prefix via list(TRANSFORM).
# Add a new shared .cpp HERE ONCE — not in all three CMakeLists.
set(SCHOLION_SHARED_SOURCES
    main.cpp
    rast_pipeline.cpp
    canvas_annot.cpp
    project_io.cpp
    canvas.cpp
    input.cpp
    renderer.cpp
    overlay.cpp
    pdf_loader.cpp
    texture_cache.cpp
    selftest.cpp
)
