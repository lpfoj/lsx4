if(NOT DEFINED LSX4_ARM_ASR_ROOT)
    set(LSX4_ARM_ASR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/externals/arm-asr")
endif()
add_library(lsx4_arm_asr STATIC
    "${LSX4_ARM_ASR_ROOT}/src/shared/ffxm_assert.cpp"
    "${LSX4_ARM_ASR_ROOT}/src/shared/ffxm_object_management.cpp"
    "${LSX4_ARM_ASR_ROOT}/src/components/fsr2/ffxm_fsr2.cpp"
    "${LSX4_ARM_ASR_ROOT}/src/backends/shared/ffxm_shader_blobs.cpp"
    "${LSX4_ARM_ASR_ROOT}/src/backends/shared/blob_accessors/ffxm_fsr2_shaderblobs.cpp"
    "${LSX4_ARM_ASR_ROOT}/src/backends/vk/ffxm_hash.cpp"
    "${LSX4_ARM_ASR_ROOT}/src/backends/vk/ffxm_vk.cpp")
target_compile_definitions(lsx4_arm_asr PRIVATE FFXM_FSR=1)
target_compile_options(lsx4_arm_asr PRIVATE -w)
target_include_directories(lsx4_arm_asr PUBLIC
    "${LSX4_ARM_ASR_ROOT}"
    "${LSX4_ARM_ASR_ROOT}/include"
    "${LSX4_ARM_ASR_ROOT}/include/host")
target_include_directories(lsx4_arm_asr PRIVATE
    "${LSX4_ARM_ASR_ROOT}/include/gpu"
    "${LSX4_ARM_ASR_ROOT}/src/components"
    "${LSX4_ARM_ASR_ROOT}/src/backends/shared"
    "${LSX4_ARM_ASR_ROOT}/src/backends/shared/blob_accessors"
    "${LSX4_ARM_ASR_ROOT}/src/backends/shared/blob_accessors/prebuilt_shaders"
    "${LSX4_ARM_ASR_ROOT}/src/backends/vk")
target_link_libraries(lsx4_arm_asr PUBLIC Vulkan::Headers)
