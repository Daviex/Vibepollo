# Apply only the checksum-verified local fixes to the pinned optional runtime.
foreach(_required SOURCE_DIR GIT_EXECUTABLE PATCH_FILE PATCH_SHA256)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing PyroWave patch argument: ${_required}")
    endif()
endforeach()
file(SHA256 "${PATCH_FILE}" _actual_hash)
if(NOT _actual_hash STREQUAL PATCH_SHA256)
    message(FATAL_ERROR "PyroWave patch checksum mismatch: ${PATCH_FILE}")
endif()

# An extracted dependency inside Sunshine is still inside its Git worktree.
# `git -C <dependency> apply` can silently skip every patch path there. Resolve
# the enclosing worktree via a relative path (also works with MSYS Git), then
# pass an explicit --directory prefix. A source tree outside Git uses its own
# working directory normally. No index or repository contents are modified.
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --show-cdup
    RESULT_VARIABLE _inside_git OUTPUT_VARIABLE _parent_path
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
set(_working_dir "${SOURCE_DIR}")
set(_directory_arg)
if(_inside_git EQUAL 0)
    file(REAL_PATH "${SOURCE_DIR}/${_parent_path}" _working_dir)
    file(RELATIVE_PATH _source_prefix "${_working_dir}" "${SOURCE_DIR}")
    if(_source_prefix MATCHES "^\\.\\.(/|$)")
        message(FATAL_ERROR "PyroWave patch source escaped its enclosing worktree")
    endif()
    if(NOT _source_prefix STREQUAL "")
        set(_directory_arg "--directory=${_source_prefix}")
    endif()
endif()
set(_apply_command "${GIT_EXECUTABLE}" -C "${_working_dir}" apply
    ${_directory_arg} --whitespace=nowarn)
execute_process(COMMAND ${_apply_command} --check -- "${PATCH_FILE}"
    RESULT_VARIABLE _can_apply ERROR_VARIABLE _apply_error)
if(NOT _can_apply EQUAL 0)
    execute_process(COMMAND ${_apply_command} --reverse --check -- "${PATCH_FILE}"
        RESULT_VARIABLE _already_applied ERROR_QUIET)
    if(_already_applied EQUAL 0)
        message(STATUS "Verified PyroWave runtime patch is already applied")
        return()
    endif()
    message(FATAL_ERROR "Pinned PyroWave runtime patch does not apply: ${_apply_error}")
endif()
execute_process(COMMAND ${_apply_command} -- "${PATCH_FILE}"
    RESULT_VARIABLE _applied ERROR_VARIABLE _apply_error)
if(NOT _applied EQUAL 0)
    message(FATAL_ERROR "Could not apply pinned PyroWave runtime patch: ${_apply_error}")
endif()
execute_process(COMMAND ${_apply_command} --reverse --check -- "${PATCH_FILE}"
    RESULT_VARIABLE _verified ERROR_VARIABLE _verify_error)
if(NOT _verified EQUAL 0)
    message(FATAL_ERROR "Could not verify applied PyroWave runtime patch: ${_verify_error}")
endif()
message(STATUS "Applied PyroWave runtime patch SHA256=${PATCH_SHA256}")
