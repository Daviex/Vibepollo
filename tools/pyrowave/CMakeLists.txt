cmake_minimum_required(VERSION 3.27)
project(vibepollo_pyrowave_gpu_smoke LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The PyroWave D3D11 smoke harness requires Windows")
endif()

set(PYROWAVE_API_INCLUDE "" CACHE PATH "Staged include directory containing pyrowave/pyrowave.h and vulkan/vulkan.h")
set(PYROWAVE_RUNTIME_DLL "" CACHE FILEPATH "Pinned libpyrowave-shared-0.dll to copy beside the harness")
if(NOT EXISTS "${PYROWAVE_API_INCLUDE}/pyrowave/pyrowave.h" OR NOT EXISTS "${PYROWAVE_RUNTIME_DLL}")
    message(FATAL_ERROR "Set PYROWAVE_API_INCLUDE and PYROWAVE_RUNTIME_DLL to the optional dependency's staged outputs")
endif()

get_filename_component(VIBE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_executable(pyrowave-gpu-smoke
    "${VIBE_ROOT}/tools/pyrowave_gpu_smoke.cpp"
    "${VIBE_ROOT}/src/pyrowave_protocol.cpp"
    "${VIBE_ROOT}/src/platform/windows/pyrowave_runtime.cpp")
add_executable(pyrowave-decode-fixture decode_fixture.cpp "${VIBE_ROOT}/src/pyrowave_protocol.cpp")
add_executable(pyrowave-lifecycle-smoke lifecycle_smoke.cpp "${VIBE_ROOT}/src/platform/windows/pyrowave_runtime.cpp")
# Run even when only the pinned dependency changed and no harness relink is
# needed; POST_BUILD alone would leave an older runtime beside the executable.
add_custom_target(pyrowave-copy-runtime
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PYROWAVE_RUNTIME_DLL}" "$<TARGET_FILE_DIR:pyrowave-gpu-smoke>/libpyrowave-shared-0.dll"
    VERBATIM)
foreach(smoke_target IN ITEMS pyrowave-gpu-smoke pyrowave-decode-fixture pyrowave-lifecycle-smoke)
    add_dependencies(${smoke_target} pyrowave-copy-runtime)
    target_compile_features(${smoke_target} PRIVATE cxx_std_23)
    target_compile_definitions(${smoke_target} PRIVATE SUNSHINE_ENABLE_PYROWAVE NOMINMAX _WIN32_WINNT=0x0A00)
    target_include_directories(${smoke_target} PRIVATE "${VIBE_ROOT}" "${PYROWAVE_API_INCLUDE}")
    target_link_libraries(${smoke_target} PRIVATE d3d11 dxgi dxguid bcrypt psapi)
    if(MINGW)
        target_link_options(${smoke_target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endforeach()

# Explicitly built fault fixture, isolated from the genuine runtime and never
# installed or copied into the production executable directory.
add_library(pyrowave-incompatible-runtime SHARED EXCLUDE_FROM_ALL incompatible_runtime.cpp)
set_target_properties(pyrowave-incompatible-runtime PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libpyrowave-shared-0"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/abi-mismatch"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/abi-mismatch")
add_library(pyrowave-invalid-contract SHARED EXCLUDE_FROM_ALL incompatible_runtime.cpp)
target_compile_definitions(pyrowave-invalid-contract PRIVATE PYROWAVE_FAKE_CONTRACT=1)
set_target_properties(pyrowave-invalid-contract PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libpyrowave-shared-0"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/contract-mismatch"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/contract-mismatch")
