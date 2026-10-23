# This migration target consumes Funnel's complete production source inventory.
# Keep it opt-in until the Apple platform adapters pass linking/device gates.
include("${LSX4_FUNNEL_ROOT}/cmake/lsx4_arm_desktop_layer.cmake")
lsx4_define_android_desktop_profile()
lsx4_assert_arm_desktop_ownership(${LSX4_ARM_DESKTOP_SOURCES})
set(LSX4_APPLE_MEDIA_ROOT "${LSX4_ROOT}/LSX4iOS/.build/media-iphoneos" CACHE PATH "Apple FFmpeg installation")
add_library(lsx4_apple_media INTERFACE)
add_library(FFmpeg::ffmpeg ALIAS lsx4_apple_media)
target_include_directories(lsx4_apple_media INTERFACE "${LSX4_APPLE_MEDIA_ROOT}/include")
foreach(MEDIA_LIBRARY avformat avcodec swscale avutil avfilter swresample)
    target_link_libraries(lsx4_apple_media INTERFACE "${LSX4_APPLE_MEDIA_ROOT}/lib/lib${MEDIA_LIBRARY}.a")
endforeach()
set(EXECUTOR_ANDROID_NATIVE_CORE_PROBE_ENABLE_AVPLAYER ON)
set(EXECUTOR_ANDROID_NATIVE_CORE_PROBE_NO_DESKTOP_MEDIA_USB ON)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
# The complete upstream export may retain CRLF text. Normalize only host-tool
# input views; do not rewrite the pinned external sources.
set(AWK "${CMAKE_CURRENT_LIST_DIR}/awk_lf.py" CACHE FILEPATH "Apple host awk input adapter" FORCE)
add_subdirectory("${LSX4_DEPENDENCY_ROOT}/externals" full-externals)
set(LSX4_ARM_ASR_ROOT "${LSX4_DEPENDENCY_ROOT}/externals/arm-asr")
include("${LSX4_ROOT}/cmake/lsx4_arm_asr.cmake")
target_compile_options(lsx4_arm_asr PRIVATE -include cstddef)
include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
# Conservative timer quirk route for the cross-compiled Darwin target. Device
# semantics are checked separately; CMake cannot execute an iPhone binary.
set(ALLOWS_ONESHOT_TIMERS_WITH_TIMEOUT_ZERO 0 CACHE STRING "" FORCE)
FetchContent_Declare(epoll_shim
    URL https://codeload.github.com/jiixyj/epoll-shim/tar.gz/5187f4b51316f61ca78c74db117071607a98069c
    URL_HASH SHA256=93eb1898eb25416a9172140a8d40057aa9435a7f319c3fc3e2699c8ee2df3ab0
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(epoll_shim)
set(APP_VERSION "0.15.0-apple-bridge")
set(APP_IS_RELEASE false)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${LSX4_ROOT}"
    OUTPUT_VARIABLE GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git branch --show-current WORKING_DIRECTORY "${LSX4_ROOT}"
    OUTPUT_VARIABLE GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE)
set(GIT_DESC "Apple bridge development build")
set(GIT_REMOTE_NAME "origin")
set(GIT_REMOTE_URL "https://github.com/llayerlinux/draft")
string(TIMESTAMP BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file("${LSX4_FUNNEL_ROOT}/src/common/scm_rev.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/apple_scm_rev.cpp" @ONLY)
set(CONSOLE_SOURCES ${LSX4_ARM_DESKTOP_SOURCES})
# The CPU target already owns this shared decoder wrapper.
list(REMOVE_ITEM CONSOLE_SOURCES "${LSX4_ARM_DESKTOP_SOURCE_DIR}/common/x86_decoder.cpp")
# Android's optional SwanStation launcher owns ANativeWindow/AAudio and is
# separate from the shared PS4/PS5 runtime. The Apple adapter reports core
# unavailability until an Apple SwanStation implementation is linked.
add_library(lsx4_apple_console_runtime STATIC ${CONSOLE_SOURCES}
    "${CMAKE_CURRENT_LIST_DIR}/AppleConsoleInput.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AppleTextInput.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AppleHostServices.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AppleOptionalBackends.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/apple_scm_rev.cpp"
    "${LSX4_ROOT}/src/executor/native_runtime_api.cpp"
    "${LSX4_ROOT}/src/executor/android_performance_hint.cpp"
    "${LSX4_ROOT}/src/executor/fex_backend_loader.cpp"
    "${LSX4_ROOT}/src/executor/fex_fiber_hle_bridge.cpp"
    "${LSX4_ROOT}/src/executor/fex_hle_integration_seam.cpp"
    "${LSX4_ROOT}/src/executor/ps4_jit_platform.cpp"
    "${LSX4_ROOT}/src/executor/runtime_death_catcher.cpp")
target_compile_definitions(lsx4_apple_console_runtime PRIVATE
    LSX4_APPLE_RUNTIME=1 BOOST_ASIO_STANDALONE
    EXECUTOR_ANDROID_NATIVE_CORE_PROBE_NO_DESKTOP_MEDIA_USB
    EXECUTOR_ANDROID_NATIVE_CORE_PROBE_ENABLE_AVPLAYER
    IMGUI_USER_CONFIG="imgui/imgui_config.h")
target_compile_definitions(Dear_ImGui PRIVATE
    IMGUI_USER_CONFIG="${LSX4_ARM_DESKTOP_SOURCE_DIR}/imgui/imgui_config.h")
target_include_directories(lsx4_apple_console_runtime PRIVATE
    "${LSX4_ROOT}" "${LSX4_ROOT}/src" "${LSX4_FUNNEL_ROOT}/src" "${LSX4_FUNNEL_ROOT}"
    "${LSX4_DEPENDENCY_ROOT}" "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/Compatibility"
    "${LSX4_DEPENDENCY_ROOT}/externals/tracy/public"
    "${LSX4_DEPENDENCY_ROOT}/externals/NVIDIAImageScaling/NIS")
target_link_libraries(lsx4_apple_console_runtime PUBLIC lsx4_apple_runtime_core PRIVATE
    magic_enum::magic_enum fmt::fmt toml11::toml11 tsl::robin_map xbyak::xbyak
    RenderDoc::API Dear_ImGui gcn half::half ZLIB::ZLIB PNG::PNG
    Boost::headers GPUOpen::VulkanMemoryAllocator LibAtrac9 sirit Vulkan::Headers
    xxHash::xxhash glslang::glslang SDL3::SDL3 SDL3_mixer::SDL3_mixer pugixml::pugixml
    stb::headers nlohmann_json::nlohmann_json miniz::miniz fdk-aac CLI11::CLI11
    OpenAL::OpenAL Cpp_Httplib FFmpeg::ffmpeg lsx4_arm_asr epoll-shim)
target_compile_options(lsx4_apple_console_runtime PRIVATE -Wno-ignored-attributes
    -include "${CMAKE_CURRENT_LIST_DIR}/Compatibility/android/log.h")
set(LSX4_HOST_SHADER_EXTERNALS_ROOT "${LSX4_DEPENDENCY_ROOT}/externals")
add_subdirectory("${LSX4_ARM_DESKTOP_SOURCE_DIR}/video_core/host_shaders" full-host-shaders)
add_dependencies(lsx4_apple_console_runtime host_shaders)
target_include_directories(lsx4_apple_console_runtime PRIVATE "${HOST_SHADERS_INCLUDE}")
include("${LSX4_ROOT}/cmake/CMakeRC.cmake")
cmrc_add_resource_library(lsx4_apple_resources NAMESPACE res WHENCE "${LSX4_FUNNEL_ROOT}"
    "${LSX4_ARM_DESKTOP_SOURCE_DIR}/runtime_resources/trophy.wav"
    "${LSX4_ARM_DESKTOP_SOURCE_DIR}/runtime_resources/bronze.png"
    "${LSX4_ARM_DESKTOP_SOURCE_DIR}/runtime_resources/gold.png"
    "${LSX4_ARM_DESKTOP_SOURCE_DIR}/runtime_resources/platinum.png"
    "${LSX4_ARM_DESKTOP_SOURCE_DIR}/runtime_resources/silver.png")
target_link_libraries(lsx4_apple_console_runtime PRIVATE lsx4_apple_resources)
add_subdirectory("${LSX4_ARM_DESKTOP_SOURCE_DIR}/imgui/renderer" full-imgui-resources)
add_dependencies(lsx4_apple_console_runtime ImGui_Resources)
target_include_directories(lsx4_apple_console_runtime PRIVATE "${IMGUI_RESOURCES_INCLUDE}")
add_library(lsx4_apple_hle_abi_inventory OBJECT "${LSX4_ROOT}/tests/hle_abi_inventory_compile_smoke.cpp")
target_compile_definitions(lsx4_apple_hle_abi_inventory PRIVATE LSX4_APPLE_RUNTIME=1)
target_include_directories(lsx4_apple_hle_abi_inventory PRIVATE "${LSX4_FUNNEL_ROOT}/src")
target_link_libraries(lsx4_apple_hle_abi_inventory PRIVATE fmt::fmt)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/full-console-sources.txt"
    CONTENT "$<JOIN:$<TARGET_PROPERTY:lsx4_apple_console_runtime,SOURCES>,\n>\n")

option(LSX4_APPLE_FULL_CONSOLE_LINK_PROBE "Verify the complete runtime launch link closure" OFF)
if(LSX4_APPLE_FULL_CONSOLE_LINK_PROBE)
    set(LSX4_APPLE_DECODER_LIBRARY "" CACHE FILEPATH "Rust decoder archive for this target")
    set(LSX4_APPLE_MOLTENVK_LIBRARY "" CACHE FILEPATH "MoltenVK static archive for this target")
    foreach(REQUIRED_LIBRARY LSX4_APPLE_DECODER_LIBRARY LSX4_APPLE_MOLTENVK_LIBRARY)
        if(NOT EXISTS "${${REQUIRED_LIBRARY}}")
            message(FATAL_ERROR "Set ${REQUIRED_LIBRARY} to the matching Apple archive")
        endif()
    endforeach()
    option(LSX4_APPLE_FULL_CONSOLE_DEVICE_PROBE "Build a UIKit full-runtime device fixture" OFF)
    if(LSX4_APPLE_FULL_CONSOLE_DEVICE_PROBE)
        enable_language(OBJCXX)
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
            enable_language(Swift)
            add_library(lsx4_console_overlay STATIC
                "${LSX4_ROOT}/LSX4iOS/LSX4iOS/DualShockOverlayView.swift"
                "${CMAKE_CURRENT_SOURCE_DIR}/FullConsoleLibrary.swift")
            set_target_properties(lsx4_console_overlay PROPERTIES
                Swift_LANGUAGE_VERSION 5 Swift_COMPILATION_MODE wholemodule)
            target_compile_definitions(lsx4_console_overlay PRIVATE LSX4_FULL_CONSOLE_UI)
            target_compile_options(lsx4_console_overlay PRIVATE
                -emit-objc-header-path "${CMAKE_CURRENT_BINARY_DIR}/LSX4Overlay-Swift.h")
            # Ninja's target ordering does not wait for a Swift archive before
            # compiling ObjC++. The archive build emits the imported header.
            set_source_files_properties(Tests/DeviceProbeMain.mm PROPERTIES
                OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/liblsx4_console_overlay.a")
            add_executable(lsx4_full_console_link_probe Tests/FullConsoleRuntimeProbe.cpp Tests/DeviceProbeMain.mm AppleSystemTextInput.mm)
            # Only the new UIKit responder owns weak delegates/timer callbacks.
            # Existing probe/capture translation units retain their MRC contract.
            set_source_files_properties(AppleSystemTextInput.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            add_dependencies(lsx4_full_console_link_probe lsx4_console_overlay)
            set(LSX4_APPLE_LIBRARY_CATALOG_DIR "" CACHE PATH "Catalog and artwork copied from the user's Vivo")
            if(EXISTS "${LSX4_APPLE_LIBRARY_CATALOG_DIR}/catalog.json")
                add_custom_command(TARGET lsx4_full_console_link_probe POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_directory
                        "${LSX4_APPLE_LIBRARY_CATALOG_DIR}" "$<TARGET_BUNDLE_DIR:lsx4_full_console_link_probe>/GameCatalog"
                    VERBATIM)
            endif()
            target_include_directories(lsx4_full_console_link_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
            target_link_libraries(lsx4_full_console_link_probe PRIVATE lsx4_console_overlay)
            # The core's dependency graph publishes Clang linker options.
            # Keep its linker driver and add Swift's compatibility archives.
            set_target_properties(lsx4_full_console_link_probe PROPERTIES LINKER_LANGUAGE CXX)
            get_filename_component(LSX4_SWIFT_BIN "${CMAKE_Swift_COMPILER}" DIRECTORY)
            target_link_directories(lsx4_full_console_link_probe PRIVATE
                "${LSX4_SWIFT_BIN}/../lib/swift/iphoneos")
        else()
            add_executable(lsx4_full_console_link_probe Tests/FullConsoleRuntimeProbe.cpp Tests/MacConsoleMain.mm)
        endif()
        target_sources(lsx4_full_console_link_probe PRIVATE Tests/FullConsoleGuestHeapFixture.cpp)
        target_compile_definitions(lsx4_full_console_link_probe PRIVATE LSX4_APPLE_RUNTIME=1 LSX4_FULL_CONSOLE_PROBE=1
            EXECUTOR_ANDROID_NATIVE_CORE_PROBE_NO_DESKTOP_MEDIA_USB)
        target_link_libraries(lsx4_full_console_link_probe PRIVATE Boost::headers tsl::robin_map
            Vulkan::Headers Dear_ImGui magic_enum::magic_enum xxHash::xxhash)
        target_include_directories(lsx4_full_console_link_probe PRIVATE "${APPLE_NATIVE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Compatibility"
            "${LSX4_DEPENDENCY_ROOT}/externals/tracy/public")
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(lsx4_full_console_link_probe PROPERTIES
            MACOSX_BUNDLE TRUE OUTPUT_NAME LSX4RuntimeProbe
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Tests/DeviceProbeInfo.plist"
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/FullConsole.entitlements")
        endif()
    else()
        add_executable(lsx4_full_console_link_probe Tests/FullConsoleLinkProbe.cpp)
    endif()
    target_link_libraries(lsx4_full_console_link_probe PRIVATE lsx4_apple_console_runtime
        "${LSX4_APPLE_DECODER_LIBRARY}" "${LSX4_APPLE_MOLTENVK_LIBRARY}"
        "-framework Metal" "-framework QuartzCore" "-framework IOSurface"
        "-framework CoreVideo" "-framework CoreGraphics"
        "-framework ImageIO" "-framework AVFoundation" "-framework CoreMedia"
        "-framework Foundation" "-framework Security" "-framework CoreFoundation"
        -lresolv)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(lsx4_full_console_link_probe PRIVATE "-framework UIKit")
    else()
        target_link_libraries(lsx4_full_console_link_probe PRIVATE "-framework AppKit")
        add_executable(lsx4_host_tiling_contract Tests/HostTilingContract.cpp)
        target_link_libraries(lsx4_host_tiling_contract PRIVATE
            glslang::glslang glslang::glslang-default-resource-limits)
        enable_testing()
        add_test(NAME host_tiling_bounds COMMAND lsx4_host_tiling_contract
            "${LSX4_FUNNEL_ROOT}/src/video_core/host_shaders/tiling.comp")
    endif()
    target_link_options(lsx4_full_console_link_probe PRIVATE -Wl,-dead_strip)
    add_custom_command(TARGET lsx4_full_console_link_probe POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/check_macho_tls.py"
            "$<TARGET_FILE:lsx4_full_console_link_probe>"
        VERBATIM)
    # Mach-O requires explicit allowance for weak imports absent from the link
    # inputs. Only optional legacy backend hooks qualify, never game HLEs.
    # The shared device fixture verifies that an absent weak import is nullptr.
    foreach(OPTIONAL_BACKEND_HOOK
        executor_box64_embed_get_guest_thread_full_regs_by_pthread
        executor_box64_embed_get_guest_thread_regs_by_pthread
        executor_box64_embed_get_guest_thread_regs_by_tid
        executor_lsx4_android_should_join_guest_thread
        executor_lsx4_prepare_fex_signal_handlers
        executor_lsx4_rollback_prepared_fex_signal_handlers)
        target_link_options(lsx4_full_console_link_probe PRIVATE "-Wl,-U,_${OPTIONAL_BACKEND_HOOK}")
    endforeach()
endif()
