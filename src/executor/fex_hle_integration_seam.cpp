// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_hle_integration_seam.h"

#include "core/aerolib/stubs.h"

#include <algorithm>
#include <cstring>
#include <string>

extern "C" int32_t ExecutorFexDispatchHle(
    void*, const Lsx4FexHleCallFrameV3*, Lsx4FexHleResultFrameV3*);

namespace {

void CopyError(const std::string& error, char* destination,
               const std::size_t capacity) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const std::size_t count = std::min(error.size(), capacity - 1);
    std::memcpy(destination, error.data(), count);
    destination[count] = '\0';
}

}

extern "C" int executor_lsx4_fex_hle_integration_register(
    const std::uint64_t native_function, const char* name,
    const Core::AeroLib::AndroidX64HleAbiDescriptor* descriptor) {
    if (native_function == 0 || descriptor == nullptr) {
        return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    Core::AeroLib::RegisterAndroidX64HleAbiDescriptor(
        native_function, name, *descriptor);
    Core::AeroLib::AndroidX64HleAbiDescriptor resolved{};
    return Core::AeroLib::ResolveAndroidX64HleAbiDescriptor(
               native_function, &resolved)
               ? LSX4_FEX_STATUS_OK
               : LSX4_FEX_STATUS_UNSUPPORTED;
}

extern "C" int executor_lsx4_fex_hle_integration_resolve(
    const std::uint64_t native_function,
    Core::AeroLib::AndroidX64HleAbiDescriptor* descriptor) {
    return Core::AeroLib::ResolveAndroidX64HleAbiDescriptor(
               native_function, descriptor)
               ? LSX4_FEX_STATUS_OK
               : LSX4_FEX_STATUS_NOT_FOUND;
}

extern "C" int executor_lsx4_fex_hle_integration_invoke_raw(
    const Core::AeroLib::AndroidX64HleRawCall* call,
    Core::AeroLib::AndroidX64HleRawResult* result) {
    if (call == nullptr || result == nullptr) {
        return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    return Core::AeroLib::InvokeAndroidX64HleTypedTarget(*call, result)
               ? LSX4_FEX_STATUS_OK
               : LSX4_FEX_STATUS_BACKEND_FAILURE;
}

extern "C" int executor_lsx4_fex_hle_integration_dispatch(
    const Lsx4FexHleCallFrameV3* call, Lsx4FexHleResultFrameV3* result) {
    return ExecutorFexDispatchHle(nullptr, call, result);
}

extern "C" std::uint64_t executor_lsx4_fex_hle_integration_make_stub(
    const std::uint64_t native_function, const char* name) {
    if (native_function == 0) {
        return 0;
    }
    return Core::AeroLib::GetAndroidX64HleStubForNative(name,
                                                         native_function);
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_test_make_nid_stub(const char* nid) {
    return nid != nullptr ? Core::AeroLib::GetStub(nid) : 0;
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_stub_slab_base() {
    return Core::AeroLib::GetAndroidX64ZeroStubSlabBase();
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_stub_slab_size() {
    return Core::AeroLib::GetAndroidX64ZeroStubSlabSize();
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_stub_slot_size() {
    return Core::AeroLib::GetAndroidX64HleStubSlotSize();
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_stub_max_emitted_size() {
    return Core::AeroLib::GetAndroidX64HleMaxEmittedBytes();
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_stub_canary_failures() {
    return Core::AeroLib::GetAndroidX64HleEmissionCanaryFailures();
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_protocol_fingerprint() {
    return Core::AeroLib::GetAndroidX64HleProtocolFingerprint();
}

extern "C" std::uint64_t
executor_lsx4_fex_hle_integration_begin_generation() {
    return Core::AeroLib::BeginAndroidX64ResolvedHleGeneration();
}

extern "C" void executor_lsx4_fex_hle_integration_mark_resolved(
    const std::uint64_t native_function, const char* name) {
    Core::AeroLib::MarkAndroidX64ResolvedHleTarget(native_function, name);
}

extern "C" int executor_lsx4_fex_hle_integration_validate_generation(
    char* error, const std::size_t error_capacity) {
    std::string message;
    const bool valid = Core::AeroLib::ValidateAndroidX64ResolvedHleAbis(
        &message);
    CopyError(message, error, error_capacity);
    return valid ? LSX4_FEX_STATUS_OK : LSX4_FEX_STATUS_ABI_MISMATCH;
}

extern "C" int executor_lsx4_fex_hle_integration_seal_generation(
    char* error, const std::size_t error_capacity) {
    std::string message;
    const bool sealed = Core::AeroLib::SealAndroidX64ResolvedHleGeneration(
        &message);
    CopyError(message, error, error_capacity);
    return sealed ? LSX4_FEX_STATUS_OK : LSX4_FEX_STATUS_ABI_MISMATCH;
}
