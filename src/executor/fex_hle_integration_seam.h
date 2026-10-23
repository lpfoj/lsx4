// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/aerolib/hle_abi.h"
#include "executor/fex_backend_abi_v3.h"

extern "C" {

int executor_lsx4_fex_hle_integration_register(
    std::uint64_t native_function, const char* name,
    const Core::AeroLib::AndroidX64HleAbiDescriptor* descriptor);

int executor_lsx4_fex_hle_integration_resolve(
    std::uint64_t native_function,
    Core::AeroLib::AndroidX64HleAbiDescriptor* descriptor);

int executor_lsx4_fex_hle_integration_invoke_raw(
    const Core::AeroLib::AndroidX64HleRawCall* call,
    Core::AeroLib::AndroidX64HleRawResult* result);

int executor_lsx4_fex_hle_integration_dispatch(
    const Lsx4FexHleCallFrameV3* call, Lsx4FexHleResultFrameV3* result);

std::uint64_t executor_lsx4_fex_hle_integration_make_stub(
    std::uint64_t native_function, const char* name);

// Test seam for exercising the production low/NID reservation route without
// exporting Core::AeroLib::GetStub from the shared library.
std::uint64_t executor_lsx4_fex_hle_integration_test_make_nid_stub(
    const char* nid);

std::uint64_t executor_lsx4_fex_hle_integration_stub_slab_base();

std::uint64_t executor_lsx4_fex_hle_integration_stub_slab_size();

std::uint64_t executor_lsx4_fex_hle_integration_stub_slot_size();

std::uint64_t executor_lsx4_fex_hle_integration_stub_max_emitted_size();

std::uint64_t executor_lsx4_fex_hle_integration_stub_canary_failures();

std::uint64_t executor_lsx4_fex_hle_integration_protocol_fingerprint();

std::uint64_t executor_lsx4_fex_hle_integration_begin_generation();

void executor_lsx4_fex_hle_integration_mark_resolved(
    std::uint64_t native_function, const char* name);

int executor_lsx4_fex_hle_integration_validate_generation(
    char* error, std::size_t error_capacity);

int executor_lsx4_fex_hle_integration_seal_generation(
    char* error, std::size_t error_capacity);

}
