cmake_minimum_required(VERSION 3.16)
project(epoch-equinox C CXX)

include(FetchContent)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(GenerateCarts)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

option(GBRECOMP_SHRINK "Aggressive size shrink (dead-strip + symbol strip)" ON)
option(EPOCH_WITH_VOXEL "Build the voxel diorama renderer" ON)
set(EPOCH_ROM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/roms"
    CACHE PATH "Directory holding your own ROM dumps")

include(ProcessorCount)
ProcessorCount(_nproc)
if(_nproc EQUAL 0)
    set(_nproc 4)
endif()
set(EPOCH_JOBS ${_nproc} CACHE STRING "Parallel jobs for recompilation")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
    endif()
    if(GBRECOMP_SHRINK)
        add_compile_options(-ffunction-sections -fdata-sections)
        add_link_options(-Wl,--gc-sections)
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_link_options(-s)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# gb-recompiled: the MIT-licensed recompiler and runtime this is built on.
#
# The frame-hook patch lets a host substitute its own rendering for the guest
# frame (the voxel renderer uses it). PATCH_COMMAND runs without a shell and
# re-runs on re-configures, so the idempotence logic lives in a -P script.
# ---------------------------------------------------------------------------
set(GBRT_REF "main" CACHE STRING "git ref of GB-Recomp/gb-recompiled to use")
FetchContent_Declare(gb_recompiled
    GIT_REPOSITORY https://github.com/GB-Recomp/gb-recompiled.git
    GIT_TAG ${GBRT_REF}
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/gbrt-frame-hook.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_gbrt_patch.cmake
)
FetchContent_GetProperties(gb_recompiled)
if(NOT gb_recompiled_POPULATED)
    FetchContent_Populate(gb_recompiled)
endif()
add_subdirectory(${gb_recompiled_SOURCE_DIR}/runtime ${CMAKE_BINARY_DIR}/_gbrt_build)

# ---------------------------------------------------------------------------
# Carts, recompiled from your own ROMs.
#
# No recompiled game code lives in this repository. Each cart's C is produced
# here, on your machine, from a ROM you supply.
# ---------------------------------------------------------------------------
epoch_build_recompiler(${gb_recompiled_SOURCE_DIR})

file(GLOB EPOCH_ROMS
    "${EPOCH_ROM_DIR}/*.gbc" "${EPOCH_ROM_DIR}/*.gb" "${EPOCH_ROM_DIR}/*.sgb")

if(NOT EPOCH_ROMS)
    message(FATAL_ERROR
        "\n"
        "No ROMs found in ${EPOCH_ROM_DIR}\n"
        "\n"
        "This project recompiles the games from your own dumps -- it ships no\n"
        "game code. Put your ROMs there, named by game id:\n"
        "\n"
        "    roms/tlozooa.gbc    The Legend of Zelda: Oracle of Ages\n"
        "    roms/tlozoos.gbc    The Legend of Zelda: Oracle of Seasons\n"
        "\n"
        "Then configure again. ./setup.sh does all of this for you.\n")
endif()

set(_ids "")
set(_titles "")
set(_rom_paths "")
set(_cart_targets "")

foreach(rom ${EPOCH_ROMS})
    get_filename_component(id "${rom}" NAME_WE)

    # Pretty names for the carts this project is actually about; anything
    # else still builds and just shows its id.
    if(id STREQUAL "tlozooa")
        set(title "Oracle of Ages")
    elseif(id STREQUAL "tlozoos")
        set(title "Oracle of Seasons")
    else()
        set(title "${id}")
    endif()

    epoch_generate_cart("${rom}" "${id}" CART)

    set_source_files_properties(${CART_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
    add_library(${id}_cart STATIC ${CART_SOURCES})
    target_include_directories(${id}_cart PUBLIC ${CART_DIR})
    target_link_libraries(${id}_cart PUBLIC gbrt)

    list(APPEND _ids "${id}")
    list(APPEND _titles "${title}")
    list(APPEND _rom_paths "${rom}")
    list(APPEND _cart_targets ${id}_cart)

    # The runtime looks for roms/<id> next to the executable on first boot
    # to extract assets from. (configure_file treats a trailing slash as a
    # filename, not a directory, so use file(COPY) here.)
    file(COPY "${rom}" DESTINATION "${CMAKE_BINARY_DIR}/roms")
endforeach()

epoch_write_game_table("${_ids}" "${_titles}" "${_rom_paths}"
                         "${CMAKE_BINARY_DIR}/generated/epoch_games.h")

list(JOIN _titles ", " _title_list)
message(STATUS "Carts: ${_title_list}")

# ---------------------------------------------------------------------------
# Our own code: mod loader + voxel renderer
# ---------------------------------------------------------------------------
set(EPOCH_SUPPORT_SOURCES
    src/mod_loader.c
    src/platform_compat.c)
if(EPOCH_WITH_VOXEL)
    list(APPEND EPOCH_SUPPORT_SOURCES
        src/voxel/voxel_tiles.c
        src/voxel/voxel_oracle.c
        src/voxel/voxel_render.c)
endif()

add_library(epoch_support STATIC ${EPOCH_SUPPORT_SOURCES})
target_include_directories(epoch_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(epoch_support PUBLIC gbrt)
if(EPOCH_WITH_VOXEL)
    target_compile_definitions(epoch_support PUBLIC EPOCH_HAVE_VOXEL=1)
endif()

# ---------------------------------------------------------------------------
# Game runner (the launcher is a separate Python app -- see launcher/)
# ---------------------------------------------------------------------------
add_executable(epoch_runner runner_main.cpp)
set_target_properties(epoch_runner PROPERTIES OUTPUT_NAME epoch)
target_link_libraries(epoch_runner PRIVATE epoch_support gbrt ${_cart_targets})
