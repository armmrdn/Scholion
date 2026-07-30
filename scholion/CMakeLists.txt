cmake_minimum_required(VERSION 3.20)
project(scholion VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Minimum macOS 12.0 (Monterey) — required for universal arm64+x86_64 distribution.
# arm64 cannot go below 11.0, and Monterey covers all Apple Silicon Macs shipping
# from 2020 onward plus Intel Macs that can run Monterey.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

option(SCHOLION_WITH_MUPDF "Link MuPDF for PDF rasterization (milestone 2)" OFF)

# --- Dependencies -----------------------------------------------------------

# GLFW
find_package(glfw3 3.3 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# libcurl (URL download feature)
find_package(CURL REQUIRED)

# Dear ImGui (bundled in third_party/imgui — see README)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- Main Executable --------------------------------------------------------

include(${CMAKE_SOURCE_DIR}/cmake/sources.cmake)
list(TRANSFORM SCHOLION_SHARED_SOURCES PREPEND "src/" OUTPUT_VARIABLE SCHOLION_MAC_SOURCES)

add_executable(scholion
    ${SCHOLION_MAC_SOURCES}
    third_party/tinyfiledialogs/tinyfiledialogs.c
)

target_include_directories(scholion PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs
)

target_link_libraries(scholion PRIVATE
    glfw
    OpenGL::GL
    imgui
    CURL::libcurl
)

# macOS framework links
if(APPLE)
    target_link_libraries(scholion PRIVATE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework UniformTypeIdentifiers"
    )
endif()

# --- macOS app bundle --------------------------------------------------------
if(APPLE)
    set(SCHOLION_ICON "${CMAKE_SOURCE_DIR}/resources/AppIcon.icns")
    target_sources(scholion PRIVATE "${SCHOLION_ICON}")
    set_source_files_properties("${SCHOLION_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    set_target_properties(scholion PROPERTIES
        MACOSX_BUNDLE                   TRUE
        MACOSX_BUNDLE_INFO_PLIST        "${CMAKE_SOURCE_DIR}/resources/Info.plist"
        OUTPUT_NAME                     "Scholion"
    )

    target_sources(scholion PRIVATE src/scholion_osx.mm)
    set_source_files_properties(src/scholion_osx.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")

    # Ad-hoc codesign — runs locally without a Developer account.
    # Replace "-" with your Developer ID for distribution.
    add_custom_command(TARGET scholion POST_BUILD
        COMMAND codesign --force --deep --sign -
                "$<TARGET_BUNDLE_DIR:scholion>"
        COMMENT "Ad-hoc signing Scholion.app"
    )
endif()

# --- MuPDF (milestone 2) — bundled static libs in third_party/mupdf ---------
# Always linked from the in-tree copy; no Homebrew / system dependency needed.
# Note: MuPDF is AGPL; swap to pdfium if distributing a closed binary.
if(SCHOLION_WITH_MUPDF)
    set(MUPDF_DIR ${CMAKE_SOURCE_DIR}/third_party/mupdf)
    set(MUPDF_INCLUDE_DIR ${MUPDF_DIR}/include)
    set(MUPDF_LIB         ${MUPDF_DIR}/lib/libmupdf.a)
    set(MUPDF_THIRD_LIB   ${MUPDF_DIR}/lib/libmupdf-third.a)

    if(NOT EXISTS ${MUPDF_LIB})
        message(FATAL_ERROR "Bundled MuPDF not found at ${MUPDF_LIB}. "
                            "Run: scripts/build_mupdf.sh")
    endif()

    target_include_directories(scholion PRIVATE ${MUPDF_INCLUDE_DIR})
    target_link_libraries(scholion PRIVATE ${MUPDF_LIB} ${MUPDF_THIRD_LIB})
    target_compile_definitions(scholion PRIVATE SCHOLION_HAVE_MUPDF)

    # MuPDF uses these system frameworks / libs on macOS
    if(APPLE)
        target_link_libraries(scholion PRIVATE
            "-framework CoreFoundation"
            "-framework CoreGraphics"
        )
    endif()

    message(STATUS "MuPDF: bundled at ${MUPDF_DIR}")
endif()

