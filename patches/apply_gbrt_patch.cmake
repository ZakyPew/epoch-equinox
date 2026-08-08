# Applies the frame-hook patch to the fetched gb-recompiled tree.
#
# Runs as FetchContent's PATCH_COMMAND, which executes on every re-configure
# without a shell — so the "already applied?" probe has to live here rather
# than in a `git apply --check || git apply` one-liner.
#
# Invoked as:
#   cmake -DPATCH_FILE=<patch> -P apply_gbrt_patch.cmake
# with the working directory set to the populated source tree.

if(NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "PATCH_FILE not set")
endif()

find_package(Git QUIET REQUIRED)

# Already applied? (reverse-apply would succeed)
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${PATCH_FILE}
    RESULT_VARIABLE reverse_ok
    OUTPUT_QUIET ERROR_QUIET
)
if(reverse_ok EQUAL 0)
    return()
endif()

execute_process(
    COMMAND ${GIT_EXECUTABLE} apply ${PATCH_FILE}
    RESULT_VARIABLE apply_rc
    ERROR_VARIABLE apply_err
)
if(NOT apply_rc EQUAL 0)
    message(FATAL_ERROR "gbrt frame-hook patch failed to apply: ${apply_err}")
endif()
message(STATUS "gbrt frame-hook patch applied")
