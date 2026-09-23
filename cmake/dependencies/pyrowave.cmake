# Keep the disabled path independent of ExternalProject, downloaded headers,
# Vulkan SDK discovery and any runtime import library.
include_guard(GLOBAL)
if(NOT SUNSHINE_ENABLE_PYROWAVE)
    return()
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
        NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$" OR
        CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|ARM64EC|Win32)(,|$)")
    message(FATAL_ERROR "SUNSHINE_ENABLE_PYROWAVE currently requires Windows x64.")
endif()
if(CMAKE_VERSION VERSION_LESS "3.27")
    message(FATAL_ERROR "SUNSHINE_ENABLE_PYROWAVE requires CMake 3.27 or newer.")
endif()

include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/pyrowave-pins.cmake")
find_package(Git REQUIRED)
set(_pyrowave_root "${CMAKE_BINARY_DIR}/_deps/pyrowave")
# A parent archive extraction replaces its source directory. Give each pin set
# its own source tree so updating PyroWave cannot erase already-stamped Granite
# or Vulkan submodules and accidentally reuse a previous dependency checkout.
string(SHA256 _pyrowave_pin_id
    "${SUNSHINE_PYROWAVE_REVISION};${SUNSHINE_PYROWAVE_ARCHIVE_SHA256};${SUNSHINE_PYROWAVE_GRANITE_REVISION};${SUNSHINE_PYROWAVE_GRANITE_ARCHIVE_SHA256};${SUNSHINE_PYROWAVE_VOLK_REVISION};${SUNSHINE_PYROWAVE_VOLK_ARCHIVE_SHA256};${SUNSHINE_PYROWAVE_VULKAN_HEADERS_REVISION};${SUNSHINE_PYROWAVE_VULKAN_HEADERS_ARCHIVE_SHA256};${SUNSHINE_PYROWAVE_PATCH_SHA256}")
string(SUBSTRING "${_pyrowave_pin_id}" 0 12 _pyrowave_pin_id)
set(SUNSHINE_PYROWAVE_SOURCE_DIR "${_pyrowave_root}/src-${_pyrowave_pin_id}")
set(SUNSHINE_PYROWAVE_STAGE_DIR "${_pyrowave_root}/stage")
set(SUNSHINE_PYROWAVE_RUNTIME_NAME "libpyrowave-shared-0.dll")
set(SUNSHINE_PYROWAVE_RUNTIME_FILE
    "${SUNSHINE_PYROWAVE_STAGE_DIR}/$<CONFIG>/${SUNSHINE_PYROWAVE_RUNTIME_NAME}")
set(SUNSHINE_PYROWAVE_DOWNLOAD_DIR "${_pyrowave_root}/downloads" CACHE PATH
    "Archive cache for the pinned optional PyroWave dependencies")

# Sources are downloaded at build time, in parent-before-child order. This
# avoids importing Granite's options, language flags or install rules into the
# host project. URL_HASH also validates archives already placed in the cache.
function(sunshine_pyrowave_source name repository revision archive_hash source_dir)
    ExternalProject_Add(${name}
        PREFIX "${_pyrowave_root}/projects/${_pyrowave_pin_id}/${name}"
        URL "https://codeload.github.com/${repository}/tar.gz/${revision}"
        URL_HASH "SHA256=${archive_hash}"
        DOWNLOAD_NAME "${name}-${revision}.tar.gz"
        DOWNLOAD_DIR "${SUNSHINE_PYROWAVE_DOWNLOAD_DIR}"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        TLS_VERIFY TRUE
        SOURCE_DIR "${source_dir}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL TRUE
        DEPENDS ${ARGN})
endfunction()

sunshine_pyrowave_source(sunshine_pyrowave_source Themaister/pyrowave
    "${SUNSHINE_PYROWAVE_REVISION}" "${SUNSHINE_PYROWAVE_ARCHIVE_SHA256}"
    "${SUNSHINE_PYROWAVE_SOURCE_DIR}")
sunshine_pyrowave_source(sunshine_pyrowave_granite Themaister/Granite
    "${SUNSHINE_PYROWAVE_GRANITE_REVISION}" "${SUNSHINE_PYROWAVE_GRANITE_ARCHIVE_SHA256}"
    "${SUNSHINE_PYROWAVE_SOURCE_DIR}/Granite" sunshine_pyrowave_source)
sunshine_pyrowave_source(sunshine_pyrowave_volk zeux/volk
    "${SUNSHINE_PYROWAVE_VOLK_REVISION}" "${SUNSHINE_PYROWAVE_VOLK_ARCHIVE_SHA256}"
    "${SUNSHINE_PYROWAVE_SOURCE_DIR}/Granite/third_party/volk" sunshine_pyrowave_granite)
sunshine_pyrowave_source(sunshine_pyrowave_vulkan_headers KhronosGroup/Vulkan-Headers
    "${SUNSHINE_PYROWAVE_VULKAN_HEADERS_REVISION}" "${SUNSHINE_PYROWAVE_VULKAN_HEADERS_ARCHIVE_SHA256}"
    "${SUNSHINE_PYROWAVE_SOURCE_DIR}/Granite/third_party/khronos/vulkan-headers" sunshine_pyrowave_granite)

set(_pyrowave_binary_dir "${_pyrowave_root}/build-${_pyrowave_pin_id}")
set(_pyrowave_cmake_args
    "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY:PATH=${_pyrowave_binary_dir}/runtime/$<CONFIG>"
    -DPYROWAVE_DEVEL:BOOL=OFF
    -DPYROWAVE_UTILS:BOOL=OFF
    -DPYROWAVE_FP32_STORAGE:BOOL=OFF
    -DPYROWAVE_FP32_MATH:BOOL=ON
    -DGRANITE_RENDERDOC_CAPTURE:BOOL=OFF
    -DGRANITE_TOOLS:BOOL=OFF
    -DGRANITE_INSTALL_TARGETS:BOOL=OFF)
if(CMAKE_MAKE_PROGRAM)
    list(APPEND _pyrowave_cmake_args "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}")
endif()
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _pyrowave_cmake_args "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
endif()
if(MSVC)
    list(APPEND _pyrowave_cmake_args
        "-DCMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(_pyrowave_configurations ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
list(REMOVE_DUPLICATES _pyrowave_configurations)
foreach(_pyrowave_config IN LISTS _pyrowave_configurations)
    string(TOUPPER "${_pyrowave_config}" _pyrowave_upper_config)
    list(APPEND _pyrowave_cmake_args
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_${_pyrowave_upper_config}:PATH=${_pyrowave_binary_dir}/runtime/${_pyrowave_config}")
endforeach()

# Build only the shared C API, not upstream executables. Its existing fixed
# prefix and SOVERSION produce this DLL name with both MinGW and MSVC.
ExternalProject_Add(sunshine_pyrowave_runtime
    PREFIX "${_pyrowave_root}/projects/${_pyrowave_pin_id}/runtime"
    SOURCE_DIR "${SUNSHINE_PYROWAVE_SOURCE_DIR}"
    BINARY_DIR "${_pyrowave_binary_dir}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${SUNSHINE_PYROWAVE_SOURCE_DIR}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DPATCH_FILE=${SUNSHINE_PYROWAVE_PATCH_FILE}"
        "-DPATCH_SHA256=${SUNSHINE_PYROWAVE_PATCH_SHA256}"
        -P "${CMAKE_CURRENT_LIST_DIR}/../scripts/patch_pyrowave.cmake"
    CMAKE_ARGS ${_pyrowave_cmake_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target pyrowave-shared
    INSTALL_COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${SUNSHINE_PYROWAVE_SOURCE_DIR}"
        "-DRUNTIME_FILE=${_pyrowave_binary_dir}/runtime/$<CONFIG>/${SUNSHINE_PYROWAVE_RUNTIME_NAME}"
        "-DSTAGE_DIR=${SUNSHINE_PYROWAVE_STAGE_DIR}"
        "-DCONFIG=$<CONFIG>"
        "-DPINS_FILE=${CMAKE_CURRENT_LIST_DIR}/pyrowave-pins.cmake"
        -P "${CMAKE_CURRENT_LIST_DIR}/../scripts/stage_pyrowave.cmake"
    BUILD_BYPRODUCTS "${_pyrowave_binary_dir}/runtime/$<CONFIG>/${SUNSHINE_PYROWAVE_RUNTIME_NAME}"
    INSTALL_BYPRODUCTS "${SUNSHINE_PYROWAVE_RUNTIME_FILE}"
        "${SUNSHINE_PYROWAVE_STAGE_DIR}/include/pyrowave/pyrowave.h"
    DEPENDS sunshine_pyrowave_volk sunshine_pyrowave_vulkan_headers)
ExternalProject_Add_StepDependencies(sunshine_pyrowave_runtime patch
    "${SUNSHINE_PYROWAVE_PATCH_FILE}"
    "${CMAKE_CURRENT_LIST_DIR}/../scripts/patch_pyrowave.cmake")
ExternalProject_Add_StepDependencies(sunshine_pyrowave_runtime install
    "${CMAKE_CURRENT_LIST_DIR}/../scripts/stage_pyrowave.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/pyrowave-pins.cmake")

file(MAKE_DIRECTORY "${SUNSHINE_PYROWAVE_STAGE_DIR}/include")
add_library(sunshine_pyrowave_api INTERFACE)
target_include_directories(sunshine_pyrowave_api SYSTEM INTERFACE "${SUNSHINE_PYROWAVE_STAGE_DIR}/include")
target_compile_definitions(sunshine_pyrowave_api INTERFACE SUNSHINE_ENABLE_PYROWAVE=1)
add_dependencies(sunshine_pyrowave_api sunshine_pyrowave_runtime)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES sunshine_pyrowave_api)
list(APPEND SUNSHINE_TARGET_DEPENDENCIES sunshine_pyrowave_runtime)

# Deliberately no target_link_libraries(... pyrowave-shared): the server loads
# the ABI-checked DLL explicitly only when PyroWave is requested.
message(STATUS "Optional PyroWave ${SUNSHINE_PYROWAVE_REVISION}: isolated shared C API build enabled")
