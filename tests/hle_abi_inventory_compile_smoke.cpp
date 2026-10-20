// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/slot_vector.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/rtc/rtc.h"
#include "core/aerolib/hle_abi_overrides.h"

#include <cstddef>
#include <type_traits>

namespace {

using Core::AeroLib::AndroidX64HleAbiDescriptor;
using Core::AeroLib::AndroidX64SysVClass;

constexpr u8 IntegerClass = static_cast<u8>(AndroidX64SysVClass::Integer);
constexpr u8 NoClass = static_cast<u8>(AndroidX64SysVClass::NoClass);
constexpr u8 MemoryClass = static_cast<u8>(AndroidX64SysVClass::Memory);

constexpr bool IsIntegerArgument(const AndroidX64HleAbiDescriptor& descriptor,
                                 const std::size_t index) {
    return index < descriptor.argument_count &&
           descriptor.arguments[index].eightbyte_count == 1 &&
           descriptor.arguments[index].classes[0] == IntegerClass &&
           descriptor.arguments[index].classes[1] == NoClass;
}

constexpr bool HasMemoryArgument(const AndroidX64HleAbiDescriptor& descriptor) {
    for (std::size_t index = 0; index < descriptor.argument_count; ++index) {
        if (descriptor.arguments[index].classes[0] == MemoryClass ||
            descriptor.arguments[index].classes[1] == MemoryClass) {
            return true;
        }
    }
    return false;
}

using SlotWaitFn = int (*)(Common::SlotId, int, u32*);
using PrimitiveSlotWaitFn = int (*)(u32, int, u32*);
constexpr auto SlotWaitDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<SlotWaitFn>();
constexpr auto PrimitiveSlotWaitDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<PrimitiveSlotWaitFn>();

static_assert(sizeof(Common::SlotId) == sizeof(u32));
static_assert(alignof(Common::SlotId) == alignof(u32));
static_assert(std::is_trivially_copyable_v<Common::SlotId>);
static_assert(SlotWaitDescriptor.supported);
static_assert(SlotWaitDescriptor.argument_count == 3);
static_assert(IsIntegerArgument(SlotWaitDescriptor, 0));
static_assert(!HasMemoryArgument(SlotWaitDescriptor));
static_assert(SlotWaitDescriptor.descriptor_id ==
              PrimitiveSlotWaitDescriptor.descriptor_id);

using RtcTickFn = int (*)(Libraries::Rtc::OrbisRtcTick);
using PrimitiveRtcTickFn = int (*)(u64);
constexpr auto RtcTickDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<RtcTickFn>();
constexpr auto PrimitiveRtcTickDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<PrimitiveRtcTickFn>();

static_assert(sizeof(Libraries::Rtc::OrbisRtcTick) == sizeof(u64));
static_assert(alignof(Libraries::Rtc::OrbisRtcTick) == alignof(u64));
static_assert(std::is_trivially_copyable_v<Libraries::Rtc::OrbisRtcTick>);
static_assert(RtcTickDescriptor.supported);
static_assert(RtcTickDescriptor.argument_count == 1);
static_assert(IsIntegerArgument(RtcTickDescriptor, 0));
static_assert(!HasMemoryArgument(RtcTickDescriptor));
static_assert(RtcTickDescriptor.descriptor_id ==
              PrimitiveRtcTickDescriptor.descriptor_id);

using AjmInstanceCreateFn = int (*)(u32, Libraries::Ajm::AjmCodecType,
                                    Libraries::Ajm::AjmInstanceFlags, u32*);
using PrimitiveAjmInstanceCreateFn = int (*)(u32, Libraries::Ajm::AjmCodecType,
                                             u64, u32*);
constexpr auto AjmInstanceCreateDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<AjmInstanceCreateFn>();
constexpr auto PrimitiveAjmInstanceCreateDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<PrimitiveAjmInstanceCreateFn>();

static_assert(sizeof(Libraries::Ajm::AjmInstanceFlags) == sizeof(u64));
static_assert(alignof(Libraries::Ajm::AjmInstanceFlags) == alignof(u64));
static_assert(std::is_trivially_copyable_v<Libraries::Ajm::AjmInstanceFlags>);
static_assert(AjmInstanceCreateDescriptor.supported);
static_assert(AjmInstanceCreateDescriptor.argument_count == 4);
static_assert(IsIntegerArgument(AjmInstanceCreateDescriptor, 2));
static_assert(!HasMemoryArgument(AjmInstanceCreateDescriptor));
static_assert(AjmInstanceCreateDescriptor.descriptor_id ==
              PrimitiveAjmInstanceCreateDescriptor.descriptor_id);

constexpr auto AjmRunSplitDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<
        decltype(&Libraries::Ajm::sceAjmBatchJobRunSplitBufferRa)>();
static_assert(AjmRunSplitDescriptor.supported);
static_assert(AjmRunSplitDescriptor.argument_count == 10);
static_assert(AjmRunSplitDescriptor.gpr_argument_count == 6);
static_assert(AjmRunSplitDescriptor.sse_argument_count == 0);
static_assert(AjmRunSplitDescriptor.stack_argument_bytes == 32);
static_assert(!HasMemoryArgument(AjmRunSplitDescriptor));

static_assert(std::is_same_v<Libraries::AvPlayer::AvPlayerGuestVaList,
                             const void*>);
using AvPlayerVprintfFn =
    s32 (*)(const char*, Libraries::AvPlayer::AvPlayerGuestVaList);
constexpr auto AvPlayerVprintfDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<AvPlayerVprintfFn>();

static_assert(AvPlayerVprintfDescriptor.supported);
static_assert(AvPlayerVprintfDescriptor.argument_count == 2);
static_assert(AvPlayerVprintfDescriptor.gpr_argument_count == 2);
static_assert(AvPlayerVprintfDescriptor.sse_argument_count == 0);
static_assert(AvPlayerVprintfDescriptor.stack_argument_bytes == 0);
static_assert(IsIntegerArgument(AvPlayerVprintfDescriptor, 0));
static_assert(IsIntegerArgument(AvPlayerVprintfDescriptor, 1));
static_assert(!HasMemoryArgument(AvPlayerVprintfDescriptor));

constexpr auto AvPlayerCallbackDescriptor =
    Core::AeroLib::HleAbiDetail::MakeDescriptor<
        Libraries::AvPlayer::AvPlayerLogCallback>();
static_assert(AvPlayerCallbackDescriptor.supported);
static_assert(AvPlayerCallbackDescriptor.argument_count == 3);
static_assert(IsIntegerArgument(AvPlayerCallbackDescriptor, 2));
static_assert(!HasMemoryArgument(AvPlayerCallbackDescriptor));

} // namespace

extern "C" int lsx4_hle_abi_inventory_compile_smoke_anchor() {
    return 0;
}
