cmake_minimum_required(VERSION 3.22.1)
project(lsx_runtime_bridge LANGUAGES CXX)

add_library(lsx_device_info SHARED device_info_probe.cpp)
target_compile_features(lsx_device_info PRIVATE cxx_std_17)
target_link_libraries(lsx_device_info PRIVATE EGL GLESv3 vulkan)

include(FetchContent)

find_program(CARGO_EXECUTABLE cargo REQUIRED)
get_filename_component(
    PKG_EXTRACTOR_RUST_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../../pkg-extractor-rs"
    ABSOLUTE
)
set(PKG_EXTRACTOR_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/pkg-rust-target")
set(PKG_EXTRACTOR_RUST_LIBRARY
    "${PKG_EXTRACTOR_RUST_TARGET_DIR}/aarch64-linux-android/release/liblsx4_pkg_extractor.a")
if(CMAKE_HOST_WIN32)
    set(ANDROID_NDK_HOST_TAG "windows-x86_64")
    set(ANDROID_CLANG_SUFFIX ".cmd")
elseif(CMAKE_HOST_APPLE)
    set(ANDROID_NDK_HOST_TAG "darwin-x86_64")
    set(ANDROID_CLANG_SUFFIX "")
else()
    set(ANDROID_NDK_HOST_TAG "linux-x86_64")
    set(ANDROID_CLANG_SUFFIX "")
endif()
string(REGEX REPLACE "^android-" "" PKG_EXTRACTOR_ANDROID_API
       "${ANDROID_PLATFORM}")
if(NOT PKG_EXTRACTOR_ANDROID_API MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "Unable to derive the Rust linker API from ANDROID_PLATFORM=${ANDROID_PLATFORM}")
endif()
set(PKG_EXTRACTOR_RUST_LINKER
    "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${ANDROID_NDK_HOST_TAG}/bin/aarch64-linux-android${PKG_EXTRACTOR_ANDROID_API}-clang${ANDROID_CLANG_SUFFIX}")

add_custom_command(
    OUTPUT "${PKG_EXTRACTOR_RUST_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${PKG_EXTRACTOR_RUST_TARGET_DIR}"
            "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=${PKG_EXTRACTOR_RUST_LINKER}"
            "${CARGO_EXECUTABLE}" build
            --manifest-path "${PKG_EXTRACTOR_RUST_DIR}/Cargo.toml"
            --target aarch64-linux-android
            --release
            --locked
    DEPENDS
            "${PKG_EXTRACTOR_RUST_DIR}/Cargo.toml"
            "${PKG_EXTRACTOR_RUST_DIR}/Cargo.lock"
            "${PKG_EXTRACTOR_RUST_DIR}/src/lib.rs"
    WORKING_DIRECTORY "${PKG_EXTRACTOR_RUST_DIR}"
    VERBATIM
)
add_custom_target(lsx4_pkg_extractor_rust_build DEPENDS "${PKG_EXTRACTOR_RUST_LIBRARY}")
add_library(lsx4_pkg_extractor_rust STATIC IMPORTED GLOBAL)
set_target_properties(lsx4_pkg_extractor_rust PROPERTIES
    IMPORTED_LOCATION "${PKG_EXTRACTOR_RUST_LIBRARY}")
add_dependencies(lsx4_pkg_extractor_rust lsx4_pkg_extractor_rust_build)

FetchContent_Declare(
    unrar_source
    URL https://www.rarlab.com/rar/unrarsrc-7.2.7.tar.gz
    URL_HASH SHA256=01D903A7DCF413CB2925696D7796E48E38D471F79BFE7EF3AD2AEBF6C12DBEFD
)
FetchContent_MakeAvailable(unrar_source)

set(UNRAR_SOURCE_DIR "${unrar_source_SOURCE_DIR}")
if(EXISTS "${UNRAR_SOURCE_DIR}/unrar/dll.cpp")
    set(UNRAR_SOURCE_DIR "${UNRAR_SOURCE_DIR}/unrar")
endif()

set(UNRAR_SOURCES
    archive.cpp arcread.cpp blake2s.cpp cmddata.cpp consio.cpp crc.cpp crypt.cpp
    dll.cpp encname.cpp errhnd.cpp extinfo.cpp extract.cpp file.cpp filefn.cpp
    filcreat.cpp filestr.cpp find.cpp getbits.cpp global.cpp hash.cpp headers.cpp
    largepage.cpp list.cpp match.cpp options.cpp pathfn.cpp qopen.cpp rawread.cpp
    rdwrfn.cpp resource.cpp rijndael.cpp rarvm.cpp scantree.cpp secpassword.cpp
    sha1.cpp sha256.cpp smallfn.cpp strfn.cpp strlist.cpp system.cpp threadpool.cpp
    timefn.cpp ui.cpp unicode.cpp unpack.cpp volume.cpp rs16.cpp
)
list(TRANSFORM UNRAR_SOURCES PREPEND "${UNRAR_SOURCE_DIR}/")

add_library(
    lsx_runtime_bridge
    SHARED
    runtime_bridge.cpp
    archive_bridge.cpp
    pkg_bridge.cpp
    ${UNRAR_SOURCES}
)
add_dependencies(lsx_runtime_bridge lsx4_pkg_extractor_rust_build)
target_compile_features(lsx_runtime_bridge PRIVATE cxx_std_17)
target_compile_definitions(
    lsx_runtime_bridge
    PRIVATE
    _UNIX
    RARDLL
    RAR_SMP
    _FILE_OFFSET_BITS=64
    _LARGEFILE_SOURCE
)
target_compile_options(
    lsx_runtime_bridge
    PRIVATE
    -Wno-logical-op-parentheses
    -Wno-switch
    -Wno-dangling-else
)
target_include_directories(lsx_runtime_bridge PRIVATE "${UNRAR_SOURCE_DIR}")
target_link_libraries(lsx_runtime_bridge PRIVATE lsx4_pkg_extractor_rust android log dl m)
