# Writes OUT: a header defining HOLEVR_BUILD_ID as the git revision of SRC
# (short hash, "-dirty" with uncommitted changes to tracked files), or
# OVERRIDE when given. Run at every build by the hole_vr_build_id target; the
# file is rewritten only when the id changes, so nothing rebuilds needlessly.
if(DEFINED OVERRIDE AND NOT OVERRIDE STREQUAL "")
    set(id "${OVERRIDE}")
else()
    execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD
                    OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rev_result ERROR_QUIET)
    if(NOT rev_result EQUAL 0 OR rev STREQUAL "")
        set(id "unknown")
    else()
        execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
                        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        set(id "${rev}")
        if(NOT dirty STREQUAL "")
            string(APPEND id "-dirty")
        endif()
    endif()
endif()
if(NOT id MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR "build id '${id}' may contain only letters, digits, dot, underscore, + or -")
endif()
set(text "#pragma once\n#define HOLEVR_BUILD_ID \"${id}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
