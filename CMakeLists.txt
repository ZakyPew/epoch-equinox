cmake_minimum_required(VERSION 3.16)
project(oracles C CXX)

include(CheckIPOSupported)
include(FetchContent)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel)
endif()
option(GBRECOMP_SHRINK "Aggressive size shrink (dead-strip + symbol strip)" ON)
option(ORACLES_WITH_SEASONS "Fetch and link Oracle of Seasons (GB-Recomp/tlozoos)" ON)
option(ORACLES_WITH_VOXEL "Build the voxel diorama renderer" ON)

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
# gb-recompiled runtime
# ---------------------------------------------------------------------------
if(NOT TARGET gbrt)
    set(GBRT_REF "main" CACHE STRING "git ref of GB-Recomp/gb-recompiled to use")
    FetchContent_Declare(gb_recompiled
        GIT_REPOSITORY https://github.com/GB-Recomp/gb-recompiled.git
        GIT_TAG ${GBRT_REF}
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(gb_recompiled)
    if(NOT gb_recompiled_POPULATED)
        FetchContent_Populate(gb_recompiled)
    endif()
    add_subdirectory(${gb_recompiled_SOURCE_DIR}/runtime ${CMAKE_BINARY_DIR}/_gbrt_build)
endif()

# ---------------------------------------------------------------------------
# Oracle of Ages (this repo's generated sources)
# ---------------------------------------------------------------------------
file(GLOB TLOZOOA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tlozooa.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tlozooa_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tlozooa_rom.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tlozooa_dispatch_chunk_*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tlozooa_funcs_*.c"
)

set(GBRECOMP_GENERATED_OPT_LEVEL "1" CACHE STRING "Optimization level for generated ROM source files")
set_source_files_properties(${TLOZOOA_SOURCES} PROPERTIES COMPILE_OPTIONS "-O${GBRECOMP_GENERATED_OPT_LEVEL}")

add_library(tlozooa_cart STATIC ${TLOZOOA_SOURCES})
target_include_directories(tlozooa_cart PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tlozooa_cart PUBLIC gbrt)

# ---------------------------------------------------------------------------
# Oracle of Seasons (fetched — the generated sources live upstream)
#
# tlozoos guards its own standalone executable behind
# `CMAKE_PROJECT_NAME STREQUAL "tlozoos"`, so adding it here yields just the
# tlozoos_cart library. Its symbols are prefixed (tlozoos__*) so the two
# carts link into one binary without colliding.
# ---------------------------------------------------------------------------
if(ORACLES_WITH_SEASONS)
    set(TLOZOOS_REF "main" CACHE STRING "git ref of GB-Recomp/tlozoos to use")
    FetchContent_Declare(tlozoos
        GIT_REPOSITORY https://github.com/GB-Recomp/tlozoos.git
        GIT_TAG ${TLOZOOS_REF}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tlozoos)
endif()

# ---------------------------------------------------------------------------
# Shared launcher-side code: mod loader + voxel renderer
# ---------------------------------------------------------------------------
set(ORACLES_SUPPORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/mod_loader.c)
if(ORACLES_WITH_VOXEL)
    list(APPEND ORACLES_SUPPORT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/voxel_tiles.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/voxel_render.c
    )
endif()

add_library(oracles_support STATIC ${ORACLES_SUPPORT_SOURCES})
target_include_directories(oracles_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(oracles_support PUBLIC gbrt)
if(ORACLES_WITH_VOXEL)
    target_compile_definitions(oracles_support PUBLIC ORACLES_HAVE_VOXEL=1)
endif()

# ---------------------------------------------------------------------------
# Game runner executable (the launcher is a separate Python app)
# ---------------------------------------------------------------------------
add_executable(oracles_runner runner_main.cpp)
set_target_properties(oracles_runner PROPERTIES OUTPUT_NAME oracles)
target_link_libraries(oracles_runner PRIVATE tlozooa_cart oracles_support gbrt)
if(ORACLES_WITH_SEASONS)
    target_link_libraries(oracles_runner PRIVATE tlozoos_cart)
    target_compile_definitions(oracles_runner PRIVATE ORACLES_HAVE_SEASONS=1)
endif()
