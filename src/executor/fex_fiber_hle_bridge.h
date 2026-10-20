// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "executor/dynamic_translation/live_state_port.h"
#include "executor/fex_backend_abi_v3.h"

#include <cstdint>
#include <string_view>

namespace Executor::Fex {



[[nodiscard]] bool IsContextReplacingFiberNid(std::string_view name) noexcept;






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

}
