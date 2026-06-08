cmake_minimum_required(VERSION 3.20)
project(Scholion VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Shared source tree (lives in ../scholion — one copy serves all platforms).
# Platform-specific files live here in scholion-win.
# ---------------------------------------------------------------------------
set(SHARED     "${CMAKE_SOURCE_DIR}/../scholion")
set(SHARED_SRC "${SHARED}/src")
set(SHARED_INC "${SHARED}/include")
set(SHARED_3P  "${SHARED}/third_party")
set(WIN_3P     "${CMAKE_SOURCE_DIR}/third_party")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# GLFW  — install via:  winget install glfw  OR  vcpkg install glfw3
#         MSYS2:        pacman -S mingw-w64-x86_64-glfw
find_package(glfw3 3.3 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# libcurl — vcpkg install curl  |  MSYS2: mingw-w64-x86_64-curl
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# GLAD (OpenGL 3.3 core function loader — required on Windows)
# See third_party/glad/SETUP_GLAD.md for download instructions.
# ---------------------------------------------------------------------------
set(GLAD_DIR "${WIN_3P}/glad")
set(GLAD_INC "${GLAD_DIR}/include")
set(GLAD_SRC "${GLAD_DIR}/src/glad.c")

if(NOT EXISTS "${GLAD_SRC}")
    message(FATAL_ERROR
        "GLAD source not found at ${GLAD_SRC}.\n"
        "Run setup_windows.ps1, or see ${GLAD_DIR}/SETUP_GLAD.md.")
endif()

# Also need KHR platform header directory
set(KHR_INC "${GLAD_DIR}/include")

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
# MuPDF (Windows static libs — see third_party/mupdf/lib/PLACE_WINDOWS_LIBS_HERE.txt)
# ---------------------------------------------------------------------------
option(SCHOLION_WITH_MUPDF "Link MuPDF for PDF rasterization" OFF)

if(SCHOLION_WITH_MUPDF)
    set(MUPDF_INC "${SHARED_3P}/mupdf/include")

    # MSVC produces .lib; MinGW/GCC produces .a
    if(MSVC)
        set(MUPDF_LIB   "${WIN_3P}/mupdf/lib/mupdf.lib")
        set(MUPDF_THIRD "${WIN_3P}/mupdf/lib/mupdf-third.lib")
    else()
        set(MUPDF_LIB   "${WIN_3P}/mupdf/lib/libmupdf.a")
        set(MUPDF_THIRD "${WIN_3P}/mupdf/lib/libmupdf-third.a")
    endif()

    if(NOT EXISTS "${MUPDF_LIB}")
        message(FATAL_ERROR
            "MuPDF lib not found at ${MUPDF_LIB}.\n"
            "CI: built automatically by build-windows.yml.\n"
            "Local: copy libmupdf.a / libmupdf-third.a (MinGW) or "
            "mupdf.lib / mupdf-third.lib (MSVC) into ${WIN_3P}/mupdf/lib/")
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows resource (icon + version metadata)
# ---------------------------------------------------------------------------
set(WIN_RC "${CMAKE_SOURCE_DIR}/resources/scholion.rc")

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(Scholion WIN32
    # ---- Shared source (cross-platform, lives in ../scholion/src/) ----------
    ${SHARED_SRC}/main.cpp
    ${SHARED_SRC}/canvas.cpp
    ${SHARED_SRC}/input.cpp
    ${SHARED_SRC}/renderer.cpp
    ${SHARED_SRC}/overlay.cpp
    ${SHARED_SRC}/pdf_loader.cpp
    ${SHARED_SRC}/texture_cache.cpp
    # ---- Windows-specific source --------------------------------------------
    ${CMAKE_SOURCE_DIR}/src/platform_win.cpp
    # ---- Shared third-party -------------------------------------------------
    ${SHARED_3P}/tinyfiledialogs/tinyfiledialogs.c
    # ---- Windows resources --------------------------------------------------
    ${WIN_RC}
)

target_include_directories(Scholion PRIVATE
    ${SHARED_INC}
    ${SHARED_3P}/tinyfiledialogs
    ${GLAD_INC}
)

target_compile_definitions(Scholion PRIVATE
    # Silence MSVC warnings about standard C functions (fopen, sprintf, etc.)
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(Scholion PRIVATE
    glfw
    OpenGL::GL
    imgui
    CURL::libcurl
    # Windows system libs needed by the app
    shell32   # ShellExecuteW
    ole32     # CoTaskMemFree (used by SHGetKnownFolderPath)
    gdi32     # GDI (GLFW dependency)
    user32    # Windows messaging (GLFW dependency)
    dwmapi    # DwmSetWindowAttribute — dark title bar
)

if(SCHOLION_WITH_MUPDF)
    target_include_directories(Scholion PRIVATE ${MUPDF_INC})
    target_link_libraries(Scholion PRIVATE "${MUPDF_LIB}" "${MUPDF_THIRD}")
    target_compile_definitions(Scholion PRIVATE SCHOLION_HAVE_MUPDF)
endif()

# MSVC-specific settings
if(MSVC)
    target_compile_options(Scholion PRIVATE /W3 /wd4996 /utf-8)
    set_target_properties(Scholion PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# MinGW: statically link the C/C++ and pthreads runtime so the .exe
# doesn't depend on libgcc, libstdc++, or libwinpthread DLLs.
if(MINGW)
    target_link_options(Scholion PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,--whole-archive
        -lwinpthread
        -Wl,--no-whole-archive
    )
endif()

message(STATUS "Scholion Windows build configured")
message(STATUS "  Shared source : ${SHARED_SRC}")
message(STATUS "  GLAD          : ${GLAD_DIR}")
if(SCHOLION_WITH_MUPDF)
    message(STATUS "  MuPDF         : ${WIN_3P}/mupdf/lib")
endif()
