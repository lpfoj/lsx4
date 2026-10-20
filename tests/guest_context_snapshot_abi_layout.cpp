// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/guest_context_snapshot_abi.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout_v<Lsx4GuestContextLegacyPrefix>);
static_assert(std::is_trivially_copyable_v<Lsx4GuestContextLegacyPrefix>);
static_assert(std::is_standard_layout_v<Lsx4GuestContextSnapshotV2>);
static_assert(std::is_trivially_copyable_v<Lsx4GuestContextSnapshotV2>);

int main() {
    Lsx4GuestContextSnapshotV2 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.magic = LSX4_GUEST_CONTEXT_SNAPSHOT_MAGIC;
    snapshot.version = LSX4_GUEST_CONTEXT_SNAPSHOT_VERSION;
    snapshot.context_flags = LSX4_GUEST_CONTEXT_GPR_VALID |
                             LSX4_GUEST_CONTEXT_FLAGS_VALID;
    snapshot.rflags = UINT64_C(0x202);
    return snapshot.struct_size == 464 &&
                   reinterpret_cast<const std::byte*>(&snapshot.struct_size) -
                           reinterpret_cast<const std::byte*>(&snapshot) ==
                       176
               ? 0
               : 1;
}
