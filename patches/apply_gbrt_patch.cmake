# Applies one patch to the fetched gb-recompiled tree, exactly once.
#
# Runs as FetchContent's PATCH_COMMAND, which re-executes whenever the
# declare arguments change -- so idempotence matters. The old probe was
# `git apply --reverse --check`, which breaks as soon as a LATER patch in
# the chain modifies lines inside an earlier patch's insertions (the
# Windows patch does exactly that to the frame hook's). A stamp file is
# immune to that: each patch records that it has been applied to this
# checkout, and the chain re-runs harmlessly forever after.
#
# Invoked as:
#   cmake -DPATCH_FILE=<patch> -P apply_gbrt_patch.cmake
# with the working directory set to the populated source tree.

if(NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "PATCH_FILE not set")
endif()

get_filename_component(_patch_name "${PATCH_FILE}" NAME_WE)
set(_stamp ".epoch-patch-${_patch_name}.stamp")

if(EXISTS "${_stamp}")
    return()
endif()

find_package(Git QUIET REQUIRED)

execute_process(
    COMMAND ${GIT_EXECUTABLE} apply ${PATCH_FILE}
    RESULT_VARIABLE apply_rc
    ERROR_VARIABLE apply_err
)

if(NOT apply_rc EQUAL 0)
    # Forward failed. If the patch reverses cleanly it was already applied
    # (e.g. a tree from before stamps existed) -- record that and move on.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${PATCH_FILE}
        RESULT_VARIABLE reverse_ok
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT reverse_ok EQUAL 0)
        message(FATAL_ERROR "patch ${_patch_name} failed to apply: ${apply_err}")
    endif()
endif()

file(WRITE "${_stamp}" "applied\n")
message(STATUS "gbrt patch applied: ${_patch_name}")
