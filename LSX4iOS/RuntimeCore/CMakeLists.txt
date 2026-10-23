cmake_minimum_required(VERSION 3.24)
project(lsx4_apple_runtime_core LANGUAGES C CXX OBJCXX ASM)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(LSX4_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LSX4_DEPENDENCY_ROOT "${LSX4_ROOT}" CACHE PATH "Checkout containing complete upstream externals")
set(LSX4_FUNNEL_ROOT "${LSX4_ROOT}/funnel-arm" CACHE PATH "Shared Funnel checkout")
if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "The Apple runtime bridge requires an arm64 Apple target")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
include("${LSX4_ROOT}/cmake/lsx4_jit_sources.cmake")

# Only platform implementations are replaced. New common files in the Android
# manifest automatically become part of this target.
set(APPLE_SHARED_SOURCES ${LSX4_JIT_COMMON_SOURCES})
list(REMOVE_ITEM APPLE_SHARED_SOURCES
    src/executor/dynamic_translation/code_memory.cpp
    src/executor/dynamic_translation/process_memory.cpp
    src/executor/dynamic_translation/jit_aarch64_stubs.S)
list(TRANSFORM APPLE_SHARED_SOURCES PREPEND "${LSX4_ROOT}/")
set(APPLE_NATIVE "${LSX4_ROOT}/LSX4iOS/LSX4iOS/Native")
set(APPLE_BRIDGE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/AppleProcessMemory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AppleGuestMemory.cpp")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND APPLE_BRIDGE_SOURCES
        "${APPLE_NATIVE}/AppleCodeMemory.cpp" "${APPLE_NATIVE}/AppleExecutableArena.cpp")
else()
    # macOS is an execution/ABI test host, not an iPhone JIT permission model.
    list(APPEND APPLE_BRIDGE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Tests/MacTestExecutableArena.cpp")
endif()
set(SHARED_ASSEMBLY "${LSX4_ROOT}/src/executor/dynamic_translation/jit_aarch64_stubs.S")
set(APPLE_ASSEMBLY "${CMAKE_CURRENT_BINARY_DIR}/jit_aarch64_stubs.S")
add_custom_command(OUTPUT "${APPLE_ASSEMBLY}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/macho_assembly.py"
            "${SHARED_ASSEMBLY}" "${APPLE_ASSEMBLY}"
    DEPENDS "${SHARED_ASSEMBLY}" "${CMAKE_CURRENT_SOURCE_DIR}/macho_assembly.py"
    VERBATIM)

set(FMT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${LSX4_ROOT}/externals/fmt" fmt)
add_subdirectory("${LSX4_DEPENDENCY_ROOT}/externals/xbyak_aarch64" xbyak_aarch64)

# This target does not enable persistent native cache restoration yet. Include
# every shared source plus bridge sources in its separate Apple cache identity.
option(LSX4_APPLE_PS5_CORE_PROBE "Exercise the shared CPU/HLE ABI with the PS5 register policy; not a PS5 console" OFF)
if(LSX4_APPLE_PS5_CORE_PROBE AND LSX4_APPLE_BUILD_FULL_CONSOLE)
    message(FATAL_ERROR "The PS5 CPU fixture cannot turn the PS4 Funnel console into a PS5 runtime")
endif()
set(CACHE_IDENTITY "apple-runtime-bridge-v1")
if(LSX4_APPLE_PS5_CORE_PROBE)
    string(APPEND CACHE_IDENTITY ";ps5-core=1")
endif()
file(GLOB_RECURSE APPLE_ABI_HEADERS CONFIGURE_DEPENDS
    "${LSX4_ROOT}/src/executor/dynamic_translation/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
set(APPLE_IDENTITY_SOURCES ${APPLE_SHARED_SOURCES} ${APPLE_BRIDGE_SOURCES} ${APPLE_ABI_HEADERS}
    "${APPLE_NATIVE}/AppleCodeMemory.h" "${CMAKE_CURRENT_SOURCE_DIR}/macho_assembly.py")
string(APPEND CACHE_IDENTITY "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_OSX_DEPLOYMENT_TARGET}")
foreach(SOURCE IN LISTS APPLE_IDENTITY_SOURCES)
    file(SHA256 "${SOURCE}" SOURCE_HASH)
    string(APPEND CACHE_IDENTITY "${SOURCE_HASH}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SOURCE}")
endforeach()
file(SHA256 "${SHARED_ASSEMBLY}" ASM_HASH)
string(SHA256 CACHE_HASH "${CACHE_IDENTITY}${ASM_HASH}")
string(SUBSTRING "${CACHE_HASH}" 0 16 EXECUTOR_JIT_NATIVE_CACHE_ABI_HASH_64)
configure_file("${LSX4_ROOT}/src/executor/dynamic_translation/native_artifact_layout.h.in"
    native_artifact_layout.h @ONLY)

add_library(lsx4_apple_runtime_core STATIC
    ${APPLE_SHARED_SOURCES} "${APPLE_ASSEMBLY}"
    ${APPLE_BRIDGE_SOURCES}
    "${LSX4_FUNNEL_ROOT}/src/common/x86_decoder.cpp")
target_include_directories(lsx4_apple_runtime_core PUBLIC
    "${LSX4_ROOT}/src" "${LSX4_FUNNEL_ROOT}/src"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${APPLE_NATIVE}"
    "${LSX4_ROOT}/externals/robin-map/include"
    "${LSX4_ROOT}/externals/xxhash"
    "${LSX4_DEPENDENCY_ROOT}/externals/json/single_include")
target_link_libraries(lsx4_apple_runtime_core PUBLIC fmt::fmt xbyak_aarch64)
target_compile_options(lsx4_apple_runtime_core PRIVATE -Wno-ignored-attributes)
target_compile_definitions(lsx4_apple_runtime_core PUBLIC LSX4_APPLE_RUNTIME=1 PRIVATE DEBUG=1)
if(LSX4_APPLE_PS5_CORE_PROBE)
    # Same register authority gate as the independent Android PS5 Current core.
    # Actual PS5 title validation still requires the Kyty/Gen5 host bridge.
    target_compile_definitions(lsx4_apple_runtime_core PUBLIC
        LSX4_PS5_DESKTOP_PATH=1 LSX4_NATIVE_SIMD_EXTENDED=1)
endif()
set_target_properties(lsx4_apple_runtime_core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

option(LSX4_APPLE_CORE_BUILD_PROBE "Link the bounded shared CPU / host ABI fixture" OFF)
if(LSX4_APPLE_CORE_BUILD_PROBE)
    set(LSX4_APPLE_DECODER_LIBRARY "" CACHE FILEPATH "Rust decoder for this Apple target")
    if(NOT EXISTS "${LSX4_APPLE_DECODER_LIBRARY}")
        message(FATAL_ERROR "Build the target decoder with build_decoder.sh and set LSX4_APPLE_DECODER_LIBRARY")
    endif()
    add_executable(lsx4_shared_core_probe Tests/CoreProbe.cpp Tests/HleAbiProbe.cpp
        "${LSX4_ROOT}/externals/xxhash/xxhash.c")
    target_compile_definitions(lsx4_shared_core_probe PRIVATE LSX4_APPLE_RUNTIME=1)
    target_link_options(lsx4_shared_core_probe PRIVATE -Wl,-U,_LSX4MissingOptionalBackendProbe
        -Wl,-U,_executor_live_mono_run_pending_signal_safe_point)
    target_include_directories(lsx4_shared_core_probe PRIVATE
        "${LSX4_ROOT}/externals/xxhash" "${LSX4_DEPENDENCY_ROOT}/externals/json/single_include")
    target_link_libraries(lsx4_shared_core_probe PRIVATE lsx4_apple_runtime_core
        "${LSX4_APPLE_DECODER_LIBRARY}" "-framework Security" "-framework CoreFoundation" -lresolv)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        enable_language(OBJCXX)
        target_sources(lsx4_shared_core_probe PRIVATE Tests/DeviceProbeMain.mm)
        target_link_libraries(lsx4_shared_core_probe PRIVATE "-framework UIKit" "-framework CoreGraphics")
        set(LSX4_APPLE_DEVELOPMENT_TEAM "" CACHE STRING "Xcode signing team for the device probe")
        set_target_properties(lsx4_shared_core_probe PROPERTIES
            MACOSX_BUNDLE TRUE OUTPUT_NAME LSX4RuntimeProbe
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Tests/DeviceProbeInfo.plist"
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${LSX4_APPLE_DEVELOPMENT_TEAM}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.llayerlinux.LSX4RuntimeProbe
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    else()
        enable_testing()
        target_compile_definitions(lsx4_apple_runtime_core PRIVATE LSX4_APPLE_CORE_TEST_FIXTURES=1)
        target_sources(lsx4_shared_core_probe PRIVATE Tests/WarmProfileProbe.cpp)
        add_test(NAME shared_warm_profile_allocation_failure COMMAND lsx4_shared_core_probe warm-profile-allocation)
        add_test(NAME shared_current_native COMMAND lsx4_shared_core_probe)
        add_test(NAME shared_current_unlinked COMMAND "${CMAKE_COMMAND}" -E env
            LSX4_TEST_UNLINKED_NATIVE=1 $<TARGET_FILE:lsx4_shared_core_probe>)
        add_test(NAME shared_current_basic_edges COMMAND "${CMAKE_COMMAND}" -E env
            LSX4_TEST_UNLINKED_NATIVE=1 EXECUTOR_DIAG_JIT_BASIC_EDGES=1 $<TARGET_FILE:lsx4_shared_core_probe>)
        add_test(NAME shared_current_local_xmm COMMAND "${CMAKE_COMMAND}" -E env
            EXECUTOR_DIAG_JIT_LOCAL_XMM=1 $<TARGET_FILE:lsx4_shared_core_probe>)
        add_test(NAME shared_frozen_profile_lookup COMMAND "${CMAKE_COMMAND}" -E env
            LSX4_TEST_FROZEN_LOOKUP=1 $<TARGET_FILE:lsx4_shared_core_probe>)
        add_executable(lsx4_host_memory_probe Tests/HostMemoryProbe.cpp AppleHostServices.cpp)
        target_include_directories(lsx4_host_memory_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Compatibility")
        add_test(NAME apple_host_memory_permissions COMMAND lsx4_host_memory_probe)
        add_executable(lsx4_guest_heap_probe Tests/GuestHeapProbe.cpp AppleHostServices.cpp)
        target_include_directories(lsx4_guest_heap_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Compatibility")
        add_test(NAME apple_guest_heap_reservation COMMAND lsx4_guest_heap_probe)
        add_executable(lsx4_host_fault_dispatch_probe Tests/HostFaultDispatchProbe.cpp AppleProcessMemory.cpp)
        target_include_directories(lsx4_host_fault_dispatch_probe PRIVATE "${LSX4_ROOT}/src")
        add_test(NAME apple_host_guest_fault_dispatch COMMAND lsx4_host_fault_dispatch_probe)
        add_executable(lsx4_host_isa_probe Tests/HostIsaProbe.cpp
            "${LSX4_ROOT}/src/executor/dynamic_translation/host_isa_profile.cpp")
        target_include_directories(lsx4_host_isa_probe PRIVATE "${LSX4_ROOT}/src")
        target_compile_definitions(lsx4_host_isa_probe PRIVATE sysctlbyname=LSX4TestSysctlByName)
        add_test(NAME apple_host_isa_boundary COMMAND lsx4_host_isa_probe)
        add_executable(lsx4_attachmentless_render_area_probe
            "${LSX4_ROOT}/tests/vulkan_attachmentless_render_area.cpp")
        target_include_directories(lsx4_attachmentless_render_area_probe PRIVATE
            "${LSX4_FUNNEL_ROOT}/src" "${LSX4_ROOT}/externals/vulkan-headers/include")
        add_test(NAME shared_attachmentless_render_area
            COMMAND lsx4_attachmentless_render_area_probe)
    endif()
endif()

option(LSX4_APPLE_BUILD_FULL_CONSOLE "Build the complete shared Funnel console runtime (migration)" OFF)
if(LSX4_APPLE_BUILD_FULL_CONSOLE)
    include("${CMAKE_CURRENT_LIST_DIR}/FullConsole.cmake")
endif()
