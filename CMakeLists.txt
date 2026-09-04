cmake_minimum_required(VERSION 3.16)
project(epoch-equinox C CXX)

include(FetchContent)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(EPOCH_WITH_VOXEL "Build the voxel diorama renderer" ON)

# No game code is compiled, linked, or required here. The binary is a
# player: it reads a ROM the user supplies at runtime. That is also why
# this builds in about a minute instead of twenty -- earlier revisions
# recompiled each ROM into ~170 MB of generated C that turned out never to
# execute (see tools/interp_probe.c and the README's history section).

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g)
    else()
        add_compile_options(-O2)
    endif()
    add_compile_options(-ffunction-sections -fdata-sections)
    # Apple's ld64 does not support GNU ld's --gc-sections spelling.
    if(NOT APPLE)
        add_link_options(-Wl,--gc-sections)
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_link_options(-s)
    endif()
endif()

# ---------------------------------------------------------------------------
# gb-recompiled runtime (MIT): CPU/PPU/APU emulation, SDL platform layer,
# GLES renderer, savestates, cheats, link cable, the Esc menu.
#
# The frame-hook patch lets a host substitute its own rendering for the
# guest frame -- the voxel renderer uses it. PATCH_COMMAND re-runs on
# re-configures, so idempotence lives in a -P script.
# ---------------------------------------------------------------------------
# Pinned: tracking upstream main let a runtime change (SDL 2.24+ controller
# enums) break release builds without any change on our side. Bump this sha
# deliberately, after a local build against it. Not GIT_SHALLOW because
# shallow clones cannot check out an arbitrary commit hash.
set(GBRT_REF "6581880fce60e6f139901a5942fc984e9c1db8ab" CACHE STRING
    "git ref of GB-Recomp/gb-recompiled to use")
FetchContent_Declare(gb_recompiled
    GIT_REPOSITORY https://github.com/GB-Recomp/gb-recompiled.git
    GIT_TAG ${GBRT_REF}
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-frame-hook.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-windows.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-host-menu.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-display-menu.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-hires-frame.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-audio-latency.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-host-overlay.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-turbo-api.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
)
FetchContent_GetProperties(gb_recompiled)
if(NOT gb_recompiled_POPULATED)
    FetchContent_Populate(gb_recompiled)
endif()
add_subdirectory(${gb_recompiled_SOURCE_DIR}/runtime ${CMAKE_BINARY_DIR}/_gbrt_build)

# ---------------------------------------------------------------------------
# Our code: mod loader, portability shims, voxel renderer
# ---------------------------------------------------------------------------
set(EPOCH_SUPPORT_SOURCES
    src/mod_loader.c
    src/platform_compat.c
    src/epoch_rewind.c
    src/epoch_rewind_hud.cpp
    src/epoch_achievements.c
    src/epoch_stream.c
    src/epoch_splits.c
    src/epoch_livesplit.c
    src/epoch_handoff.c
    src/epoch_handoff_hud.cpp
    src/epoch_secrets.c
    src/epoch_secrets_menu.cpp
    src/epoch_panel.cpp
    src/epoch_achievements_hud.cpp
    src/epoch_overlay.cpp)
if(EPOCH_WITH_VOXEL)
    list(APPEND EPOCH_SUPPORT_SOURCES
        src/voxel/voxel_tiles.c
        src/voxel/voxel_oracle.c
        src/voxel/voxel_overrides.c
        src/voxel/voxel_edit.c
        src/voxel/voxel_edit_hud.cpp
        src/voxel/voxel_render.c
        src/voxel/voxel_world.c
        src/voxel/voxel_menu.cpp)
endif()

add_library(epoch_support STATIC ${EPOCH_SUPPORT_SOURCES})
target_include_directories(epoch_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(epoch_support PUBLIC gbrt)
if(WIN32)
    # epoch_livesplit.c speaks winsock; MSVC resolves nothing implicitly.
    # (The file also carries a #pragma comment(lib) for hand-built tools.)
    target_link_libraries(epoch_support PUBLIC ws2_32)
endif()
if(EPOCH_WITH_VOXEL)
    target_compile_definitions(epoch_support PUBLIC EPOCH_HAVE_VOXEL=1)
endif()

# ---------------------------------------------------------------------------
# The player binary (the launcher is a separate Python app -- see launcher/)
# ---------------------------------------------------------------------------
add_executable(epoch_runner runner_main.cpp)
set_target_properties(epoch_runner PROPERTIES OUTPUT_NAME epoch)
target_link_libraries(epoch_runner PRIVATE epoch_support gbrt)

# Local convenience: if the source tree has roms/, mirror them next to the
# binary so a dev build finds them. Purely optional.
file(GLOB _dev_roms "${CMAKE_CURRENT_SOURCE_DIR}/roms/*.gb"
                    "${CMAKE_CURRENT_SOURCE_DIR}/roms/*.gbc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/roms/*.sgb")
if(_dev_roms)
    file(COPY ${_dev_roms} DESTINATION "${CMAKE_BINARY_DIR}/roms")
endif()

# The runner reads achievement packs relative to its cwd; mirror them so a
# dev build behaves like the shipped folder. Refreshed on reconfigure.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/achievements"
     DESTINATION "${CMAKE_BINARY_DIR}")
