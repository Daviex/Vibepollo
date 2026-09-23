# Runs only after the optional, pinned C API DLL has been built.
foreach(_required SOURCE_DIR RUNTIME_FILE STAGE_DIR CONFIG PINS_FILE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing PyroWave staging argument: ${_required}")
    endif()
endforeach()
if(NOT EXISTS "${RUNTIME_FILE}")
    message(FATAL_ERROR "Expected PyroWave runtime was not built: ${RUNTIME_FILE}")
endif()
include("${PINS_FILE}")
set(_granite "${SOURCE_DIR}/Granite")
set(_headers "${_granite}/third_party/khronos/vulkan-headers")
file(MAKE_DIRECTORY "${STAGE_DIR}/${CONFIG}" "${STAGE_DIR}/include/pyrowave"
    "${STAGE_DIR}/licenses/PyroWave" "${STAGE_DIR}/licenses/Granite"
    "${STAGE_DIR}/licenses/volk" "${STAGE_DIR}/licenses/Vulkan-Headers")
file(COPY "${RUNTIME_FILE}" DESTINATION "${STAGE_DIR}/${CONFIG}")
file(COPY "${SOURCE_DIR}/pyrowave.h" DESTINATION "${STAGE_DIR}/include/pyrowave")
file(COPY "${_headers}/include/" DESTINATION "${STAGE_DIR}/include")
file(COPY "${SOURCE_DIR}/LICENSE" DESTINATION "${STAGE_DIR}/licenses/PyroWave")
file(COPY "${_granite}/LICENSE" DESTINATION "${STAGE_DIR}/licenses/Granite")
file(COPY "${_granite}/third_party/volk/LICENSE.md" DESTINATION "${STAGE_DIR}/licenses/volk")
file(COPY "${_headers}/LICENSE.md" "${_headers}/LICENSES" DESTINATION "${STAGE_DIR}/licenses/Vulkan-Headers")
file(COPY "${SUNSHINE_PYROWAVE_PATCH_FILE}" DESTINATION "${STAGE_DIR}/licenses/PyroWave")
file(WRITE "${STAGE_DIR}/licenses/DEPENDENCIES.txt"
    "VibePollo optional PyroWave runtime\n"
    "PyroWave API: 0.5.0 (unstable until 1.0; exact ABI checked by the host)\n"
    "https://github.com/Themaister/pyrowave ${SUNSHINE_PYROWAVE_REVISION}\n"
    "https://github.com/Themaister/Granite ${SUNSHINE_PYROWAVE_GRANITE_REVISION}\n"
    "https://github.com/zeux/volk ${SUNSHINE_PYROWAVE_VOLK_REVISION}\n"
    "https://github.com/KhronosGroup/Vulkan-Headers ${SUNSHINE_PYROWAVE_VULKAN_HEADERS_REVISION}\n"
    "Local runtime patch: pyrowave-0.5.0-nt-handle-ownership.patch SHA256=${SUNSHINE_PYROWAVE_PATCH_SHA256}\n"
    "Patch scope: NT HANDLE ownership, failed external-memory cleanup, additive runtime-contract export.\n"
    "Runtime contract: ${SUNSHINE_PYROWAVE_REVISION};nt-handle-ownership-v1. Upstream API signatures and bitstream unchanged.\n"
    "Shaders: embedded SPIR-V in the pinned PyroWave shaders/slangmosh.hpp.\n"
    "Build: C API only; FP32 math, reduced-range storage; no development tools or RenderDoc.\n"
    "Vulkan loader/driver: supplied by the host system, not redistributed here.\n")
