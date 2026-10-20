cmake_minimum_required(VERSION 3.22.1)
project(lsx4_fex_fp_semantics_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(LSX4_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LSX4_FEX2608_ADAPTER_ROOT
  "${LSX4_REPO_ROOT}/../refs/PS4Run/fex-2608-adapter-v3"
  CACHE PATH "LSX4 FEX-2608 adapter source root (headers only)")

if(ANDROID)
  if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The staged semantics probe requires Android arm64-v8a")
  endif()

  add_executable(lsx4_fex_fp_semantics_probe
    staged_fp_semantics_probe.cpp)
  add_executable(lsx4_fex_inside_wwise_simd_probe
    staged_inside_wwise_simd_probe.cpp)
  add_executable(lsx4_fex_memory_tso_probe
    staged_memory_tso_probe.cpp)
  target_include_directories(lsx4_fex_fp_semantics_probe PRIVATE
    "${LSX4_REPO_ROOT}/src/executor"
    "${LSX4_REPO_ROOT}/funnel-arm/src"
    "${LSX4_FEX2608_ADAPTER_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_include_directories(lsx4_fex_inside_wwise_simd_probe PRIVATE
    "${LSX4_REPO_ROOT}/src/executor"
    "${LSX4_REPO_ROOT}/funnel-arm/src"
    "${LSX4_FEX2608_ADAPTER_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_include_directories(lsx4_fex_memory_tso_probe PRIVATE
    "${LSX4_REPO_ROOT}/src/executor"
    "${LSX4_REPO_ROOT}/funnel-arm/src"
    "${LSX4_FEX2608_ADAPTER_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(lsx4_fex_fp_semantics_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-missing-designated-field-initializers
    -fno-exceptions -fno-rtti)
  target_compile_options(lsx4_fex_inside_wwise_simd_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-missing-designated-field-initializers
    -fno-exceptions -fno-rtti)
  target_compile_options(lsx4_fex_memory_tso_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-missing-designated-field-initializers
    -fno-exceptions -fno-rtti)
  target_link_options(lsx4_fex_fp_semantics_probe PRIVATE
    "-Wl,--no-undefined"
    "-Wl,-z,defs"
    "-Wl,-z,now"
    "-Wl,-z,relro")
  target_link_options(lsx4_fex_inside_wwise_simd_probe PRIVATE
    "-Wl,--no-undefined"
    "-Wl,-z,defs"
    "-Wl,-z,now"
    "-Wl,-z,relro")
  target_link_options(lsx4_fex_memory_tso_probe PRIVATE
    "-Wl,--no-undefined"
    "-Wl,-z,defs"
    "-Wl,-z,now"
    "-Wl,-z,relro")
  target_link_libraries(lsx4_fex_fp_semantics_probe PRIVATE dl)
  target_link_libraries(lsx4_fex_inside_wwise_simd_probe PRIVATE dl)
  target_link_libraries(lsx4_fex_memory_tso_probe PRIVATE dl)
else()
  enable_testing()
  add_executable(fp_semantics_x86_oracle
    fp_semantics_x86_oracle.cpp)
  add_executable(inside_wwise_simd_x86_oracle
    inside_wwise_simd_x86_oracle.cpp)
  add_executable(memory_tso_x86_oracle
    memory_tso_x86_oracle.cpp)
  target_include_directories(fp_semantics_x86_oracle PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_include_directories(inside_wwise_simd_x86_oracle PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_include_directories(memory_tso_x86_oracle PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(fp_semantics_x86_oracle PRIVATE /W4 /WX)
    target_compile_options(inside_wwise_simd_x86_oracle PRIVATE /W4 /WX)
    # C4324 is expected: the litmus state deliberately places publication
    # fields on separate cache lines.
    target_compile_options(memory_tso_x86_oracle PRIVATE /W4 /WX /wd4324)
  else()
    target_compile_options(fp_semantics_x86_oracle PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(inside_wwise_simd_x86_oracle PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(memory_tso_x86_oracle PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME fp_semantics_x86_oracle COMMAND fp_semantics_x86_oracle)
  add_test(NAME inside_wwise_simd_x86_oracle
    COMMAND inside_wwise_simd_x86_oracle)
  add_test(NAME memory_tso_x86_oracle COMMAND memory_tso_x86_oracle)
endif()
