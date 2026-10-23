// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_fiber_hle_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>

namespace Executor::Fex {
namespace {

constexpr std::array<std::string_view, 5> ContextReplacingFiberNids{
    "a0LLrZWac0M", // sceFiberRun
    "PFT2S-tJ7Uk", // sceFiberSwitch
    "B0ZX2hx9DMw", // sceFiberReturnToThread
    "avfGJ94g36Q", // sceFiberRunImpl / TraceFiberRunImpl
    "ZqhZFuzKT6U", // sceFiberSwitchImpl / TraceFiberSwitchImpl
};

void FillCpuFrameFromFexContext(const Lsx4FexGuestContextV3& context,
                                Lsx4::Translation::CpuFrame& frame) noexcept {
    frame = {};
    std::copy(std::begin(context.gpr), std::end(context.gpr), frame.integer.begin());
    frame.resume_address = context.rip;
    frame.condition_word = context.rflags;
    frame.fs_origin = context.fs_base;
    frame.gs_origin = context.gs_base;
    frame.dispatch_phase = 0;

    // V3 exposes only the low 128 bits of each vector. They are useful to the
    // HLE snapshot, but Commit() deliberately does not mark XMM_VALID: applying
    // a saved low half while retaining an unrelated live YMM upper half would
    // create a state which never existed. Until the ABI carries full FP/x87 and
    // YMM state, adapter import preserves the complete currently-live FP state.
    if ((context.context_flags & LSX4_FEX_CONTEXT_XMM_VALID) != 0) {
        frame.simd_control = context.mxcsr;
        for (std::size_t index = 0; index < frame.vectors.size(); ++index) {
            std::memcpy(frame.vectors[index].data(), &context.xmm[index],
                        sizeof(context.xmm[index]));
        }
    }
}

Lsx4FexGuestContextV3 MakeCommitContext(
    const Lsx4::Translation::CpuFrame& frame,
    const std::uint64_t guest_thread) noexcept {
    Lsx4FexGuestContextV3 context{};
    context.struct_size = sizeof(context);
    context.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    std::copy(frame.integer.begin(), frame.integer.end(), std::begin(context.gpr));
    context.rip = frame.resume_address;
    context.rflags = frame.condition_word;
    context.fs_base = frame.fs_origin;
    context.gs_base = frame.gs_origin;
    context.context_flags = ScopedFiberHleContextBridge::CommitContextFlags;
    context.guest_thread = guest_thread;
    return context;
}

} // namespace

bool IsContextReplacingFiberNid(const std::string_view name) noexcept {
    return std::find(ContextReplacingFiberNids.begin(), ContextReplacingFiberNids.end(), name) !=
           ContextReplacingFiberNids.end();
}

ScopedFiberHleContextBridge::ScopedFiberHleContextBridge(
    const Lsx4FexBackendApiV3* const api,
    const Lsx4FexThreadIdentityV3& identity,
    const std::uint64_t guest_rsp,
    const std::uint64_t guest_return) noexcept
    : api_{api}, identity_{identity} {
    if (api_ == nullptr || api_->get_guest_context == nullptr ||
        api_->set_guest_context == nullptr || identity_.guest_thread == 0 ||
        guest_rsp == 0 || guest_return == 0 ||
        guest_rsp > std::numeric_limits<std::uint64_t>::max() - sizeof(std::uint64_t)) {
        status_ = LSX4_FEX_STATUS_INVALID_STATE;
        return;
    }

    Lsx4FexGuestContextV3 context{};
    context.struct_size = sizeof(context);
    context.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    status_ = api_->get_guest_context(api_->backend_context, &identity_, &context);
    if (status_ != LSX4_FEX_STATUS_OK) {
        return;
    }
    if (context.struct_size < sizeof(context) ||
        context.abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
        (context.context_flags & CommitContextFlags) != CommitContextFlags ||
        (context.guest_thread != 0 && context.guest_thread != identity_.guest_thread)) {
        status_ = LSX4_FEX_STATUS_ABI_MISMATCH;
        return;
    }

    FillCpuFrameFromFexContext(context, live_state_);
    continuation_state_ = live_state_;
    continuation_state_.resume_address = guest_return;
    continuation_state_.integer[Lsx4::Translation::RegisterIndex(
        Lsx4::Translation::IntegerRegister::Stack)] = guest_rsp + sizeof(std::uint64_t);
    continuation_state_.dispatch_phase = 0;

    // Preserve a containing publisher when a synthetic caller nests the port.
    // Production enters this bridge only at FEX HLE depth one, where both are
    // normally empty. The replacement bit is thread-local, so it is explicitly
    // saved and restored together with the two storage pointers.
    inherited_replacement_request_ =
        Lsx4::Translation::ConsumeLiveStateReplacementRequest();
    previous_live_storage_ =
        Lsx4::Translation::ExchangeLiveStateStorage(&live_state_);
    previous_continuation_storage_ =
        Lsx4::Translation::ExchangeContinuationStorage(&continuation_state_);
    publication_active_ = true;
    status_ = LSX4_FEX_STATUS_OK;
}

ScopedFiberHleContextBridge::~ScopedFiberHleContextBridge() {
    if (publication_active_) {
        (void)Lsx4::Translation::ConsumeLiveStateReplacementRequest();
        RestorePublication();
    }
}

void ScopedFiberHleContextBridge::RestorePublication() noexcept {
    if (!publication_active_) {
        return;
    }
    (void)Lsx4::Translation::ExchangeContinuationStorage(
        previous_continuation_storage_);
    (void)Lsx4::Translation::ExchangeLiveStateStorage(previous_live_storage_);
    publication_active_ = false;
    if (inherited_replacement_request_ && previous_live_storage_ != nullptr) {
        (void)Lsx4::Translation::RequestLiveStateReplacement();
    }
}

std::int32_t ScopedFiberHleContextBridge::Commit() noexcept {
    if (finished_) {
        return status_;
    }
    finished_ = true;
    if (!publication_active_) {
        return status_;
    }

    const bool replacement_requested =
        Lsx4::Translation::ConsumeLiveStateReplacementRequest();
    RestorePublication();
    if (!replacement_requested) {
        status_ = LSX4_FEX_STATUS_OK;
        return status_;
    }

    const Lsx4FexGuestContextV3 replacement =
        MakeCommitContext(live_state_, identity_.guest_thread);
    status_ = api_->set_guest_context(api_->backend_context, &identity_, &replacement);
    return status_;
}

} // namespace Executor::Fex
