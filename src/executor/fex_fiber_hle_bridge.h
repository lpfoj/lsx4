// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "executor/dynamic_translation/live_state_port.h"
#include "executor/fex_backend_abi_v3.h"

#include <cstdint>
#include <string_view>

namespace Executor::Fex {

// The two implementation entry points and the three public fiber entry points
// are the only HLE targets which may replace the running guest context.
[[nodiscard]] bool IsContextReplacingFiberNid(std::string_view name) noexcept;

// Publishes an adapter-owned FEX context through the existing CpuFrame live-state
// port for one fiber HLE invocation. The CpuFrames are members of this scoped
// object, so no pointer escapes the callback. Commit() consumes a requested
// replacement and hands it back to the adapter with set_guest_context; the
// adapter remains the sole owner of the non-local resume/epoch transition.
class ScopedFiberHleContextBridge final {
public:
    ScopedFiberHleContextBridge(const Lsx4FexBackendApiV3* api,
                                const Lsx4FexThreadIdentityV3& identity,
                                std::uint64_t guest_rsp,
                                std::uint64_t guest_return) noexcept;
    ~ScopedFiberHleContextBridge();

    ScopedFiberHleContextBridge(const ScopedFiberHleContextBridge&) = delete;
    ScopedFiberHleContextBridge& operator=(const ScopedFiberHleContextBridge&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return publication_active_;
    }

    [[nodiscard]] std::int32_t status() const noexcept {
        return status_;
    }

    // Must be called after the native HLE target returns, while still inside
    // the first-level adapter callback. A successful context replacement does
    // not jump here: the adapter observes its context epoch after validating
    // the ordinary HLE result and selects its ResumeContext path.
    [[nodiscard]] std::int32_t Commit() noexcept;

    static constexpr std::uint32_t CommitContextFlags =
        LSX4_FEX_CONTEXT_GPR_VALID |
        LSX4_FEX_CONTEXT_FLAGS_VALID |
        LSX4_FEX_CONTEXT_TLS_VALID;

private:
    void RestorePublication() noexcept;

    const Lsx4FexBackendApiV3* api_{};
    Lsx4FexThreadIdentityV3 identity_{};
    Lsx4::Translation::CpuFrame live_state_{};
    Lsx4::Translation::CpuFrame continuation_state_{};
    void* previous_live_storage_{};
    const void* previous_continuation_storage_{};
    std::int32_t status_{LSX4_FEX_STATUS_INVALID_STATE};
    bool inherited_replacement_request_{};
    bool publication_active_{};
    bool finished_{};
};

} // namespace Executor::Fex
