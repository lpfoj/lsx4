cmake_minimum_required(VERSION 3.24)
project(lsx4_apple_shader_core LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(LSX4_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXTERNALS "${LSX4_ROOT}/externals")
set(SHARED "${LSX4_ROOT}/funnel-arm/src")
set(BUILD_SHARED_LIBS OFF)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(SIRIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXTERNALS}/fmt" fmt)
add_subdirectory("${EXTERNALS}/ext-boost" boost)
add_subdirectory("${EXTERNALS}/sirit" sirit)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXTERNALS}/glslang" glslang)
file(READ "${SHARED}/video_core/host_shaders/tiling.comp" LSX4_TILING_SOURCE)
configure_file(HostTilingShader.h.in HostTilingShader.h @ONLY)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS "${SHARED}/shader_recompiler/*.cpp")
# Compile the repository's original shader compiler without the Android app,
# guest CPU engine, or platform renderer. The Apple host adapter is separate.
add_library(lsx4_apple_shader_core STATIC ${SHADER_SOURCES})
target_include_directories(lsx4_apple_shader_core PUBLIC
    "${SHARED}" "${LSX4_ROOT}/funnel-arm" "${LSX4_ROOT}/src"
    "${EXTERNALS}/gcn/include" "${EXTERNALS}/magic_enum/include"
    "${EXTERNALS}/robin-map/include" "${EXTERNALS}/vulkan-headers/include"
    "${EXTERNALS}/xxhash")
target_link_libraries(lsx4_apple_shader_core PUBLIC fmt::fmt Boost::headers sirit)
target_compile_options(lsx4_apple_shader_core PRIVATE -Wno-ignored-attributes)
add_library(lsx4_apple_shader_probe STATIC AppleShaderProbe.cpp AppleComputeShader.cpp AppleGraphicsShader.cpp AppleVertexFetch.cpp AppleSurfaceShader.cpp ProbeHost.cpp
    "${SHARED}/common/io_file.cpp" "${SHARED}/common/error.cpp"
    "${SHARED}/video_core/amdgpu/pixel_format.cpp"
    "${SHARED}/video_core/amdgpu/regs.cpp"
    "${SHARED}/video_core/amdgpu/tiling.cpp")
target_include_directories(lsx4_apple_shader_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lsx4_apple_shader_probe PUBLIC lsx4_apple_shader_core glslang glslang-default-resource-limits)
target_compile_options(lsx4_apple_shader_probe PRIVATE -Wno-ignored-attributes)
option(LSX4_SHADER_BUILD_PROBE "Build the standalone compiler probe" OFF)
if(LSX4_SHADER_BUILD_PROBE)
    add_executable(lsx4_vertex_fetch_tests ../Tests/vertex_fetch_tests.cpp)
    target_link_libraries(lsx4_vertex_fetch_tests PRIVATE lsx4_apple_shader_probe)
    target_compile_options(lsx4_vertex_fetch_tests PRIVATE -Wno-ignored-attributes -UNDEBUG)
    add_executable(lsx4_shader_probe ../Tests/shader_probe.cpp)
    target_link_libraries(lsx4_shader_probe PRIVATE lsx4_apple_shader_probe)
    target_compile_options(lsx4_shader_probe PRIVATE -Wno-ignored-attributes -UNDEBUG)
endif()
