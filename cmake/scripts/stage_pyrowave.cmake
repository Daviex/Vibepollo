# Runs after the optional pinned Vulkan/Metal C API has been built.
foreach(_required SOURCE_DIR RUNTIME_FILE STAGE_DIR CONFIG PINS_FILE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing PyroWave staging argument: ${_required}")
    endif()
endforeach()
if(NOT EXISTS "${RUNTIME_FILE}")
    message(FATAL_ERROR "Expected PyroWave runtime was not built: ${RUNTIME_FILE}")
endif()
if(NOT DEFINED BACKEND)
    set(BACKEND Vulkan)
endif()
include("${PINS_FILE}")
file(MAKE_DIRECTORY "${STAGE_DIR}/${CONFIG}" "${STAGE_DIR}/include/pyrowave"
    "${STAGE_DIR}/licenses/PyroWave")
# Dereference Unix SOVERSION symlinks, retaining the stable ABI filename.
get_filename_component(_runtime_name "${RUNTIME_FILE}" NAME)
file(REAL_PATH "${RUNTIME_FILE}" _runtime_real)
configure_file("${_runtime_real}" "${STAGE_DIR}/${CONFIG}/${_runtime_name}" COPYONLY)
file(COPY "${SOURCE_DIR}/LICENSE" "${SUNSHINE_PYROWAVE_PATCH_FILE}"
    DESTINATION "${STAGE_DIR}/licenses/PyroWave")
set(_backend_manifest "")
if(BACKEND STREQUAL "Metal")
    file(COPY "${SOURCE_DIR}/metal/pyrowave_metal.h" DESTINATION "${STAGE_DIR}/include/pyrowave")
    set(_backend_manifest
        "Backend: Metal; embedded MSL; Apple7 GPU family or newer; CPU input is 8-bit planar.\nApple Metal/IOSurface frameworks are system dependencies, not redistributed.\n")
else()
    set(_granite "${SOURCE_DIR}/Granite")
    set(_headers "${_granite}/third_party/khronos/vulkan-headers")
    file(COPY "${SOURCE_DIR}/pyrowave.h" DESTINATION "${STAGE_DIR}/include/pyrowave")
    file(COPY "${_headers}/include/" DESTINATION "${STAGE_DIR}/include")
    file(MAKE_DIRECTORY "${STAGE_DIR}/licenses/Granite" "${STAGE_DIR}/licenses/volk"
        "${STAGE_DIR}/licenses/Vulkan-Headers")
    file(COPY "${_granite}/LICENSE" DESTINATION "${STAGE_DIR}/licenses/Granite")
    file(COPY "${_granite}/third_party/volk/LICENSE.md" DESTINATION "${STAGE_DIR}/licenses/volk")
    file(COPY "${_headers}/LICENSE.md" "${_headers}/LICENSES" DESTINATION "${STAGE_DIR}/licenses/Vulkan-Headers")
    string(CONCAT _backend_manifest
        "Backend: Vulkan; embedded SPIR-V; no development tools or RenderDoc.\n"
        "https://github.com/Themaister/Granite ${SUNSHINE_PYROWAVE_GRANITE_REVISION}\n"
        "https://github.com/zeux/volk ${SUNSHINE_PYROWAVE_VOLK_REVISION}\n"
        "https://github.com/KhronosGroup/Vulkan-Headers ${SUNSHINE_PYROWAVE_VULKAN_HEADERS_REVISION}\n"
        "Vulkan loader/driver are supplied by the host system, not redistributed.\n")
endif()
set(_contract_manifest "Runtime contract: exact additive host contract is checked before device creation.\n")
if(DEFINED CONTRACT_FILE AND EXISTS "${CONTRACT_FILE}")
    file(COPY "${CONTRACT_FILE}" DESTINATION "${STAGE_DIR}/licenses/PyroWave")
    set(_contract_manifest "Runtime contract: see included pyrowave_runtime_contract.h; exact value checked before device creation.\n")
endif()
file(WRITE "${STAGE_DIR}/licenses/DEPENDENCIES.txt"
    "VibePollo optional PyroWave runtime\n"
    "PyroWave API: 0.5.0 (unstable until 1.0; exact ABI checked by the host)\n"
    "https://github.com/Themaister/pyrowave ${SUNSHINE_PYROWAVE_REVISION}\n"
    "Local runtime patch SHA256=${SUNSHINE_PYROWAVE_PATCH_SHA256}\n"
    "Patch includes ownership, allocation, device dispatch, color metadata, precision and CPU bridge extensions.\n"
    "Upstream bitstream syntax is preserved; color VUI describes the supplied input planes.\n"
    "${_contract_manifest}${_backend_manifest}"
    "Wavelet precision before device creation: 0 FP16, 1 FP32 math/FP16 storage, 2 FP32.\n")
