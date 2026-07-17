cmake_minimum_required(VERSION 3.20)
project(Scholion VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Shared source tree (lives in ../scholion — one copy serves all platforms).
# Platform-specific files live here in scholion-lnx.
# ---------------------------------------------------------------------------
set(SHARED     "${CMAKE_SOURCE_DIR}/../scholion")
set(SHARED_SRC "${SHARED}/src")
set(SHARED_INC "${SHARED}/include")
set(SHARED_3P  "${SHARED}/third_party")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# GLFW — install via: apt install libglfw3-dev
#         Or build from source (recommended for CI — see build.yml).
find_package(glfw3 3.3 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# libcurl — apt install libcurl4-openssl-dev
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# GLAD (OpenGL 3.3 core function loader)
# Shared with the Windows build tree — no separate copy needed.
# ---------------------------------------------------------------------------
set(GLAD_DIR "${CMAKE_SOURCE_DIR}/../scholion-win/third_party/glad")
set(GLAD_INC "${GLAD_DIR}/include")
set(GLAD_SRC "${GLAD_DIR}/src/glad.c")

if(NOT EXISTS "${GLAD_SRC}")
    message(FATAL_ERROR
        "GLAD source not found at ${GLAD_SRC}.\n"
        "GLAD is shared with the Windows build tree.\n"
        "See ${GLAD_DIR}/SETUP_GLAD.md for download instructions.")
endif()

# ---------------------------------------------------------------------------
# Dear ImGui (shared from ../scholion/third_party/imgui)
# ---------------------------------------------------------------------------
set(IMGUI_DIR "${SHARED_3P}/imgui")
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${GLAD_SRC}
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GLAD_INC}
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------------------
# MuPDF (Linux static libs — built from source in CI)
# ---------------------------------------------------------------------------
option(SCHOLION_WITH_MUPDF "Link MuPDF for PDF rasterization" OFF)
option(SCHOLION_DEV        "Enable developer/diagnostic tools (benchmark, CSV logger)" OFF)

if(SCHOLION_WITH_MUPDF)
    set(MUPDF_INC "${SHARED_3P}/mupdf/include")
    set(MUPDF_LIB   "${CMAKE_SOURCE_DIR}/third_party/mupdf/lib/libmupdf.a")
    set(MUPDF_THIRD "${CMAKE_SOURCE_DIR}/third_party/mupdf/lib/libmupdf-third.a")

    if(NOT EXISTS "${MUPDF_LIB}")
        message(FATAL_ERROR
            "MuPDF lib not found at ${MUPDF_LIB}.\n"
            "CI: built automatically by the Linux CI job.\n"
            "Local: build MuPDF 1.24.11 from source and copy "
            "libmupdf.a / libmupdf-third.a into ${CMAKE_SOURCE_DIR}/third_party/mupdf/lib/")
    endif()
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(Scholion
    # ---- Shared source (cross-platform, lives in ../scholion/src/) ----------
    ${SHARED_SRC}/main.cpp
    ${SHARED_SRC}/rast_pipeline.cpp
    ${SHARED_SRC}/canvas_annot.cpp
    ${SHARED_SRC}/project_io.cpp
    ${SHARED_SRC}/canvas.cpp
    ${SHARED_SRC}/input.cpp
    ${SHARED_SRC}/renderer.cpp
    ${SHARED_SRC}/overlay.cpp
    ${SHARED_SRC}/pdf_loader.cpp
    ${SHARED_SRC}/texture_cache.cpp
    # ---- Linux-specific source ----------------------------------------------
    ${CMAKE_SOURCE_DIR}/src/platform_linux.cpp
    # ---- Shared third-party -------------------------------------------------
    ${SHARED_3P}/tinyfiledialogs/tinyfiledialogs.c
)

target_include_directories(Scholion PRIVATE
    ${SHARED_INC}
    ${SHARED_3P}/tinyfiledialogs
    ${GLAD_INC}
)

target_compile_options(Scholion PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(Scholion PRIVATE
    glfw
    OpenGL::GL
    imgui
    CURL::libcurl
    dl       # GLAD uses dlopen/dlsym via glfwGetProcAddress
    pthread  # std::thread
    m
)

if(SCHOLION_WITH_MUPDF)
    target_include_directories(Scholion PRIVATE ${MUPDF_INC})
    target_link_libraries(Scholion PRIVATE "${MUPDF_LIB}" "${MUPDF_THIRD}")
    target_compile_definitions(Scholion PRIVATE SCHOLION_HAVE_MUPDF)
endif()

if(SCHOLION_DEV)
    target_compile_definitions(Scholion PRIVATE SCHOLION_DEV)
    message(STATUS "SCHOLION_DEV: developer/diagnostic tools enabled")
endif()

message(STATUS "Scholion Linux build configured")
message(STATUS "  Shared source : ${SHARED_SRC}")
message(STATUS "  GLAD          : ${GLAD_DIR}")
if(SCHOLION_WITH_MUPDF)
    message(STATUS "  MuPDF         : ${CMAKE_SOURCE_DIR}/third_party/mupdf/lib")
endif()
