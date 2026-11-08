cmake_minimum_required(VERSION 3.20)
project(ps3vis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)

# FFmpeg: from pkg-config (Linux, macOS, MSYS2), or from an unpacked prebuilt FFmpeg "shared"
# build given as -DFFMPEG_DIR=... (Windows, see tools/windows-build.sh).
set(FFMPEG_DIR "" CACHE PATH "Folder of a prebuilt FFmpeg (with include/ and lib/)")
set(FFMPEG_LIBS avformat avcodec swresample avutil)
if(FFMPEG_DIR)
    add_library(ffmpeg INTERFACE)
    target_include_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/include")
    foreach(lib IN LISTS FFMPEG_LIBS)
        find_library(FFMPEG_${lib} NAMES ${lib} lib${lib}.dll.a PATHS "${FFMPEG_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(ffmpeg INTERFACE "${FFMPEG_${lib}}")
    endforeach()
else()
    find_package(PkgConfig REQUIRED)
    list(TRANSFORM FFMPEG_LIBS PREPEND lib OUTPUT_VARIABLE FFMPEG_MODULES)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET ${FFMPEG_MODULES})
    add_library(ffmpeg ALIAS PkgConfig::FFMPEG)
endif()

add_executable(ps3vis WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/gl.cpp
    src/firmware.cpp
    src/cg.cpp
    src/canyon.cpp
    src/audio.cpp
    src/overlay.cpp
    src/playlist.cpp
    src/menu.cpp
    src/vkpresent.cpp
    src/sysinfo.cpp
    src/gamepad.cpp
    src/glyphs.cpp
    src/earth.cpp
    src/dither.cpp
    src/uistyle.cpp
    src/albumart.cpp
    src/presetlib.cpp
)
# third_party/vulkan: Khronos Vulkan headers (Vulkan itself is loaded at runtime through SDL)
target_include_directories(ps3vis PRIVATE src third_party third_party/vulkan)
target_link_libraries(ps3vis PRIVATE SDL3::SDL3 ffmpeg)
if(MINGW)
    # No libstdc++/libgcc DLLs to ship alongside the program.
    target_link_options(ps3vis PRIVATE -static-libgcc -static-libstdc++)
endif()
if(WIN32)
    # Put the DLLs next to the built .exe so it also starts straight from the build folder.
    # (SDL3::SDL3 is only an interface wrapper with older CMake; the -shared target has the file.)
    if(TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET ps3vis POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:ps3vis>
            VERBATIM)
    endif()
    if(FFMPEG_DIR)
        file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
        add_custom_command(TARGET ps3vis POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:ps3vis>
            VERBATIM)
    endif()
endif()

# Which build script made this (shown in debug dumps); set by the scripts in tools/.
set(PS3VIS_BUILD_TARGET "manual" CACHE STRING "Build route: manual, linux-build, windows-build, flatpak, appimage")
target_compile_definitions(ps3vis PRIVATE PS3VIS_BUILD_TARGET="${PS3VIS_BUILD_TARGET}"
                                          PS3VIS_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

if(MSVC)
    target_compile_options(ps3vis PRIVATE /W3 /utf-8)
    target_compile_definitions(ps3vis PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(ps3vis PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

install(TARGETS ps3vis RUNTIME DESTINATION bin BUNDLE DESTINATION .)
# Free fonts for the UI themes (found next to the program, or in ../share/ps3vis/fonts).
install(DIRECTORY fonts DESTINATION share/ps3vis)
