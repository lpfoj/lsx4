// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>

#include <android/log.h>

#include "common/content_fingerprint.h"
#include "common/types.h"
#include "core/memory.h"
#include "video_core/amdgpu/regs.h"

extern "C" bool executor_funnel_arm_diagnostic_write_provenance(
    std::uint64_t address, std::uint64_t size) noexcept;

namespace AmdGpu::Pm4DiffTrace {

inline constexpr const char* ArmMarker =
    "/data/data/app.lsx4.android/files/lsx4-home/diag-pm4-diff-arm";
inline constexpr const char* SourceAnchorMarker =
    "/data/data/app.lsx4.android/files/lsx4-home/diag-pm4-diff-source-anchor";
inline constexpr const char* ResourceWriteWatchMarker =
    "/data/data/app.lsx4.android/files/lsx4-home/diag-pm4-resource-write-watch";
inline constexpr const char* OutputPath =
    "/data/data/app.lsx4.android/files/lsx4-home/pm4-diff-frame.txt";

enum class Engine : u8 {
    Graphics,
    Constant,
    Compute,
};

inline const char* EngineName(const Engine engine) noexcept {
    switch (engine) {
    case Engine::Graphics:
        return "gfx";
    case Engine::Constant:
        return "ce";
    case Engine::Compute:
        return "compute";
    }
    return "unknown";
}

struct State {
    std::mutex mutex;
    std::FILE* file{};
    std::atomic<bool> active{false};
    std::atomic<u64> resource_write_watch_request{0};
    std::atomic<bool> resource_write_watch_claimed{false};
    u64 epoch{};
    u64 record_sequence{};
    u64 submit_sequence{};
    u64 packet_records{};
    u64 draw_records{};
    u64 ib_records{};
    u64 source_sequence{};
    u64 source_hash{};
    VAddr source_address{};
    u64 empty_flip_boundaries{};
    u64 source_observations{};
    bool source_anchor_capture{};
};

inline State& GetState() noexcept {
    static State state;
    return state;
}

inline bool Active() noexcept {
    return GetState().active.load(std::memory_order_acquire);
}

inline constexpr u64 ResourceWriteWatchRequestValid = u64{1} << 63;
inline constexpr u32 ResourceWriteWatchStageShift = 0;
inline constexpr u32 ResourceWriteWatchPairShift = 3;
inline constexpr u32 ResourceWriteWatchSlotShift = 7;
inline constexpr u32 ResourceWriteWatchBytesShift = 10;

inline u64 PackResourceWriteWatchRequest(const u8 stage, const u8 pair,
                                         const u8 slot,
                                         const u64 size) noexcept {
    return ResourceWriteWatchRequestValid |
           (static_cast<u64>(stage) << ResourceWriteWatchStageShift) |
           (static_cast<u64>(pair) << ResourceWriteWatchPairShift) |
           (static_cast<u64>(slot) << ResourceWriteWatchSlotShift) |
           (size << ResourceWriteWatchBytesShift);
}

inline void PollRequestedResourceWriteWatchMarker() noexcept {
    auto& state = GetState();
    if (state.resource_write_watch_claimed.load(std::memory_order_acquire)) {
        state.resource_write_watch_request.store(0, std::memory_order_release);
        return;
    }

    std::FILE* marker = std::fopen(ResourceWriteWatchMarker, "rb");
    if (marker == nullptr) {
        state.resource_write_watch_request.store(0, std::memory_order_release);
        return;
    }
    unsigned requested_stage{};
    unsigned requested_pair{};
    unsigned requested_slot{};
    unsigned long long requested_bytes{};
    const int fields = std::fscanf(marker, "%u %u %u %llu",
                                   &requested_stage, &requested_pair,
                                   &requested_slot, &requested_bytes);
    std::fclose(marker);
    if (fields != 4 || requested_stage >= 6 || requested_pair >= NUM_USER_DATA ||
        requested_slot >= 8 || requested_bytes == 0 || requested_bytes > 4096) {
        // Keep the marker in place so an incomplete atomic replacement can be
        // retried at the next frame boundary.
        state.resource_write_watch_request.store(0, std::memory_order_release);
        return;
    }
    state.resource_write_watch_request.store(
        PackResourceWriteWatchRequest(static_cast<u8>(requested_stage),
                                      static_cast<u8>(requested_pair),
                                      static_cast<u8>(requested_slot),
                                      static_cast<u64>(requested_bytes)),
        std::memory_order_release);
}

inline bool TryArmResourceWriteWatch(const u64 semantic, const u8 stage,
                                     const u8 pair, const u8 slot,
                                     const VAddr address,
                                     const u64 size) noexcept {
    auto& state = GetState();
    const u64 requested = PackResourceWriteWatchRequest(stage, pair, slot, size);
    if (state.resource_write_watch_request.load(std::memory_order_acquire) !=
        requested) {
        return false;
    }
    bool expected = false;
    if (!state.resource_write_watch_claimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    u64 expected_request = requested;
    if (!state.resource_write_watch_request.compare_exchange_strong(
            expected_request, 0, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        state.resource_write_watch_claimed.store(false,
                                                  std::memory_order_release);
        return false;
    }
    if (!executor_funnel_arm_diagnostic_write_provenance(address, size)) {
        state.resource_write_watch_claimed.store(false,
                                                  std::memory_order_release);
        u64 empty_request = 0;
        (void)state.resource_write_watch_request.compare_exchange_strong(
            empty_request, requested, std::memory_order_release,
            std::memory_order_relaxed);
        return false;
    }
    (void)std::remove(ResourceWriteWatchMarker);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PM4_RESOURCE_WRITE_WATCH] semantic=%llu stage=%u pair=%u "
        "slot=%u address=0x%llx size=%llu",
        static_cast<unsigned long long>(semantic), static_cast<unsigned>(stage),
        static_cast<unsigned>(pair), static_cast<unsigned>(slot),
        static_cast<unsigned long long>(address),
        static_cast<unsigned long long>(size));
    return true;
}

inline void ProbeRequestedResourceWriteWatch(const Regs& regs) noexcept {
    const u64 requested = GetState().resource_write_watch_request.load(
        std::memory_order_acquire);
    if ((requested & ResourceWriteWatchRequestValid) == 0) {
        return;
    }
    const u8 requested_stage = static_cast<u8>(
        (requested >> ResourceWriteWatchStageShift) & 0x7u);
    const u8 requested_pair = static_cast<u8>(
        (requested >> ResourceWriteWatchPairShift) & 0xfu);
    const u8 requested_slot = static_cast<u8>(
        (requested >> ResourceWriteWatchSlotShift) & 0x7u);
    const u64 requested_bytes =
        (requested >> ResourceWriteWatchBytesShift) & 0x1fffu;
    const ShaderProgram* program = regs.ProgramForStage(requested_stage);
    const u32 count = std::min<u32>(program->settings.num_user_regs,
                                    program->user_data.size());
    if (requested_pair + 1 >= count) {
        return;
    }
    const VAddr table_address =
        static_cast<VAddr>(program->user_data[requested_pair]) |
        (static_cast<VAddr>(program->user_data[requested_pair + 1] & 0xffffu) << 32);
    const VAddr descriptor_address =
        table_address + requested_slot * 4u * sizeof(u32);
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr || table_address == 0 || (table_address & 3) != 0) {
        return;
    }
    std::array<u32, 4> descriptor{};
    auto guard = memory->AcquireSnapshotReadGuard();
    if (!memory->TryCopyBackingMemory(guard, descriptor_address,
                                      descriptor.data(), sizeof(descriptor)) ||
        (descriptor[3] >> 30) != 0) {
        return;
    }
    const VAddr resource_address =
        static_cast<VAddr>(descriptor[0]) |
        (static_cast<VAddr>(descriptor[1] & 0x0fffu) << 32);
    const u64 stride = (descriptor[1] >> 16) & 0x3fffu;
    const u64 records = descriptor[2];
    const u64 resource_bytes =
        stride != 0 && records <= 4096 / stride ? stride * records : records;
    if (resource_address == 0 || (resource_address & 3) != 0 ||
        resource_bytes != requested_bytes) {
        return;
    }
    std::array<u32, 8> resource_head{};
    if (!memory->TryCopyBackingMemory(guard, resource_address,
                                      resource_head.data(),
                                      sizeof(resource_head)) ||
        resource_head[4] != 0x44700000u ||
        resource_head[5] != 0x44080000u ||
        resource_head[6] != 0x3a888889u ||
        resource_head[7] != 0x3af0f0f1u) {
        return;
    }
    (void)TryArmResourceWriteWatch(0, static_cast<u8>(requested_stage),
                                   static_cast<u8>(requested_pair),
                                   static_cast<u8>(requested_slot),
                                   resource_address, resource_bytes);
}

inline u64 NextEpoch() noexcept {
    static std::atomic<u64> epoch{0};
    return epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

inline u64 CurrentEpoch() noexcept {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    return state.epoch;
}

inline bool ConsumeArmMarker() noexcept {
    std::FILE* marker = std::fopen(ArmMarker, "rb");
    if (marker == nullptr) {
        return false;
    }
    std::fclose(marker);
    if (std::remove(ArmMarker) != 0) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_PM4_DIFF_ARM] marker_remove_failed path=%s",
                            ArmMarker);
    }
    return true;
}

inline void Arm(const u64 epoch, const u64 source_sequence = 0,
                const u64 source_hash = 0, const VAddr source_address = 0) noexcept {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.active.store(false, std::memory_order_release);
    if (state.file != nullptr) {
        std::fclose(state.file);
        state.file = nullptr;
    }
    state.epoch = epoch;
    state.record_sequence = 0;
    state.submit_sequence = 0;
    state.packet_records = 0;
    state.draw_records = 0;
    state.ib_records = 0;
    state.source_sequence = source_sequence;
    state.source_hash = source_hash;
    state.source_address = source_address;
    state.empty_flip_boundaries = 0;
    state.source_observations = 0;
    state.source_anchor_capture = source_hash != 0;
    state.file = std::fopen(OutputPath, "w");
    if (state.file == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_PM4_DIFF_ARM] epoch=%llu open_failed path=%s",
                            static_cast<unsigned long long>(epoch), OutputPath);
        return;
    }
    // A device loss may terminate the renderer before the closing flip. Keep
    // every diagnostic line recoverable without changing command processing.
    std::setvbuf(state.file, nullptr, _IOLBF, 0);
    const bool fex = std::getenv("EXECUTOR_FEX_OWNS_FAULT_SIGNALS") != nullptr;
    std::fprintf(
        state.file,
        "H schema=lsx4.pm4-diff.v2 epoch=%llu signalOwner=%s sourceSeq=%llu "
        "sourceHash=0x%016llx sourceAddr=0x%llx anchor=%u\n",
        static_cast<unsigned long long>(epoch), fex ? "fex" : "current",
        static_cast<unsigned long long>(source_sequence),
        static_cast<unsigned long long>(source_hash),
        static_cast<unsigned long long>(source_address), source_hash != 0 ? 1u : 0u);
    state.active.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_PM4_DIFF_ARM] epoch=%llu backend=%s output=%s",
                        static_cast<unsigned long long>(epoch), fex ? "fex" : "current",
                        OutputPath);
}

inline u64 RequestedSourceAnchorHash() noexcept {
    static const u64 requested = []() noexcept {
        std::FILE* marker = std::fopen(SourceAnchorMarker, "rb");
        if (marker == nullptr) {
            return u64{};
        }
        unsigned long long parsed{};
        const int fields = std::fscanf(marker, "%llx", &parsed);
        std::fclose(marker);
        return fields == 1 ? static_cast<u64>(parsed) : u64{};
    }();
    return requested;
}

inline bool TryArmFromVideoOutSource(const u64 source_sequence, const u64 source_hash,
                                     const VAddr source_address) noexcept {
    auto& state = GetState();
    if (state.active.load(std::memory_order_acquire)) {
        std::scoped_lock lock{state.mutex};
        if (state.active.load(std::memory_order_relaxed) && state.source_anchor_capture &&
            state.file != nullptr) {
            const bool changed = source_hash != state.source_hash;
            std::fprintf(
                state.file,
                "S epoch=%llu observation=%llu sourceSeq=%llu hash=0x%016llx "
                "addr=0x%llx changed=%u\n",
                static_cast<unsigned long long>(state.epoch),
                static_cast<unsigned long long>(++state.source_observations),
                static_cast<unsigned long long>(source_sequence),
                static_cast<unsigned long long>(source_hash),
                static_cast<unsigned long long>(source_address), changed ? 1u : 0u);
            if (changed) {
                state.active.store(false, std::memory_order_release);
                std::fprintf(
                    state.file,
                    "F epoch=%llu reason=source-change records=%llu submits=%llu packets=%llu "
                    "draws=%llu ibs=%llu boundaries=%llu observations=%llu\n",
                    static_cast<unsigned long long>(state.epoch),
                    static_cast<unsigned long long>(state.record_sequence),
                    static_cast<unsigned long long>(state.submit_sequence),
                    static_cast<unsigned long long>(state.packet_records),
                    static_cast<unsigned long long>(state.draw_records),
                    static_cast<unsigned long long>(state.ib_records),
                    static_cast<unsigned long long>(state.empty_flip_boundaries),
                    static_cast<unsigned long long>(state.source_observations));
                std::fclose(state.file);
                state.file = nullptr;
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_PM4_DIFF_SOURCE_CHANGE] sourceSeq=%llu sourceHash=0x%016llx "
                    "records=%llu draws=%llu boundaries=%llu",
                    static_cast<unsigned long long>(source_sequence),
                    static_cast<unsigned long long>(source_hash),
                    static_cast<unsigned long long>(state.record_sequence),
                    static_cast<unsigned long long>(state.draw_records),
                    static_cast<unsigned long long>(state.empty_flip_boundaries));
                return true;
            }
            return false;
        }
    }
    const u64 requested_hash = RequestedSourceAnchorHash();
    if (requested_hash == 0 || source_hash != requested_hash) {
        return false;
    }
    static std::atomic<bool> claimed{false};
    bool expected = false;
    if (!claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    if (std::remove(SourceAnchorMarker) != 0) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_PM4_DIFF_SOURCE_ANCHOR] marker_remove_failed path=%s",
            SourceAnchorMarker);
    }
    Arm(NextEpoch(), source_sequence, source_hash, source_address);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_PM4_DIFF_SOURCE_ANCHOR] sourceSeq=%llu sourceHash=0x%016llx "
        "sourceAddr=0x%llx active=%u",
        static_cast<unsigned long long>(source_sequence),
        static_cast<unsigned long long>(source_hash),
        static_cast<unsigned long long>(source_address), Active() ? 1u : 0u);
    return Active();
}

inline void Finish(const u64 epoch) noexcept {
    auto& state = GetState();
    if (!state.active.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed)) {
        return;
    }
    if (state.source_anchor_capture) {
        if (state.file != nullptr) {
            std::fprintf(state.file,
                         "B epoch=%llu boundary=%llu records=%llu submits=%llu packets=%llu "
                         "draws=%llu\n",
                         static_cast<unsigned long long>(epoch),
                         static_cast<unsigned long long>(++state.empty_flip_boundaries),
                         static_cast<unsigned long long>(state.record_sequence),
                         static_cast<unsigned long long>(state.submit_sequence),
                         static_cast<unsigned long long>(state.packet_records),
                         static_cast<unsigned long long>(state.draw_records));
        }
        return;
    }
    state.active.store(false, std::memory_order_release);
    if (state.file != nullptr) {
        std::fprintf(state.file,
                     "F epoch=%llu records=%llu submits=%llu packets=%llu draws=%llu ibs=%llu\n",
                     static_cast<unsigned long long>(epoch),
                     static_cast<unsigned long long>(state.record_sequence),
                     static_cast<unsigned long long>(state.submit_sequence),
                     static_cast<unsigned long long>(state.packet_records),
                     static_cast<unsigned long long>(state.draw_records),
                     static_cast<unsigned long long>(state.ib_records));
        std::fclose(state.file);
        state.file = nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_PM4_DIFF_COMPLETE] epoch=%llu records=%llu submits=%llu "
                        "packets=%llu draws=%llu ibs=%llu",
                        static_cast<unsigned long long>(epoch),
                        static_cast<unsigned long long>(state.record_sequence),
                        static_cast<unsigned long long>(state.submit_sequence),
                        static_cast<unsigned long long>(state.packet_records),
                        static_cast<unsigned long long>(state.draw_records),
                        static_cast<unsigned long long>(state.ib_records));
}

inline u64 HashBytes(const void* data, const std::size_t size, const u64 tag) noexcept {
    Common::ContentFingerprint64 hash{Common::FingerprintDomain::CommandStream ^ tag};
    hash.Update(data, size);
    return hash.Finish();
}

inline u64 HashWords(const std::span<const u32> words, const u64 tag = 0) noexcept {
    Common::ContentFingerprint64 hash{Common::FingerprintDomain::CommandStream ^ tag};
    hash.Update(words.data(), words.size_bytes());
    hash.UpdateLittleEndian(static_cast<u64>(words.size()));
    return hash.Finish();
}

inline u64 ExtendPrefix(const u64 prefix, const u64 packet_hash, const u32 type,
                        const u32 opcode, const u32 words) noexcept {
    Common::ContentFingerprint64 hash{Common::FingerprintDomain::CommandStream ^
                                      0x5052454649580001ull};
    hash.UpdateLittleEndian(prefix);
    hash.UpdateLittleEndian(packet_hash);
    hash.UpdateLittleEndian(type);
    hash.UpdateLittleEndian(opcode);
    hash.UpdateLittleEndian(words);
    return hash.Finish();
}

inline u64 NextSubmitSequence() noexcept {
    if (!Active()) {
        return 0;
    }
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    return state.active.load(std::memory_order_relaxed) ? ++state.submit_sequence : 0;
}

inline void Root(const u64 submit, const std::span<const u32> raw_dcb,
                 const std::span<const u32> owned_dcb,
                 const std::span<const u32> raw_ccb,
                 const std::span<const u32> owned_ccb, const VAddr raw_dcb_address,
                 const VAddr raw_ccb_address, const u64 alias_epoch,
                 const u64 write_epoch, const bool physical_aliasing) noexcept {
    if (!Active() || submit == 0) {
        return;
    }
    const u64 raw_dcb_hash = HashWords(raw_dcb, 0x44434201ull);
    const u64 owned_dcb_hash = HashWords(owned_dcb, 0x44434201ull);
    const u64 raw_ccb_hash = HashWords(raw_ccb, 0x43434201ull);
    const u64 owned_ccb_hash = HashWords(owned_ccb, 0x43434201ull);
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed) || state.file == nullptr) {
        return;
    }
    std::fprintf(
        state.file,
        "R n=%llu submit=%llu dcbAddr=0x%llx dcbDw=%zu rawDcb=0x%016llx "
        "ownedDcb=0x%016llx dcbMatch=%u ccbAddr=0x%llx ccbDw=%zu "
        "rawCcb=0x%016llx ownedCcb=0x%016llx ccbMatch=%u aliasEpoch=%llu "
        "writeEpoch=%llu physicalAliasing=%u\n",
        static_cast<unsigned long long>(++state.record_sequence),
        static_cast<unsigned long long>(submit),
        static_cast<unsigned long long>(raw_dcb_address), raw_dcb.size(),
        static_cast<unsigned long long>(raw_dcb_hash),
        static_cast<unsigned long long>(owned_dcb_hash),
        raw_dcb.size() == owned_dcb.size() && raw_dcb_hash == owned_dcb_hash ? 1u : 0u,
        static_cast<unsigned long long>(raw_ccb_address), raw_ccb.size(),
        static_cast<unsigned long long>(raw_ccb_hash),
        static_cast<unsigned long long>(owned_ccb_hash),
        raw_ccb.size() == owned_ccb.size() && raw_ccb_hash == owned_ccb_hash ? 1u : 0u,
        static_cast<unsigned long long>(alias_epoch),
        static_cast<unsigned long long>(write_epoch), physical_aliasing ? 1u : 0u);
}

inline void Indirect(const char* phase, const Engine engine, const u64 submit,
                     const u32 depth, const VAddr address,
                     const std::span<const u32> words, const bool gpu_modified,
                     const u64 alias_epoch, const u64 write_epoch) noexcept {
    if (!Active()) {
        return;
    }
    const u64 hash = HashWords(words, 0x49420000ull | static_cast<u8>(engine));
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed) || state.file == nullptr) {
        return;
    }
    ++state.ib_records;
    std::fprintf(state.file,
                 "I n=%llu submit=%llu phase=%s engine=%s depth=%u addr=0x%llx "
                 "dw=%zu hash=0x%016llx gpuModified=%u aliasEpoch=%llu writeEpoch=%llu\n",
                 static_cast<unsigned long long>(++state.record_sequence),
                 static_cast<unsigned long long>(submit), phase ? phase : "?",
                 EngineName(engine), depth, static_cast<unsigned long long>(address),
                 words.size(), static_cast<unsigned long long>(hash),
                 gpu_modified ? 1u : 0u,
                 static_cast<unsigned long long>(alias_epoch),
                 static_cast<unsigned long long>(write_epoch));
}

inline void Packet(const Engine engine, const u64 submit, const u32 depth,
                   const u64 semantic, const u32 type, const u32 opcode,
                   const std::span<const u32> packet, const u64 prefix,
                   const u64 alias_epoch, const u64 write_epoch) noexcept {
    if (!Active()) {
        return;
    }
    // A single frame is normally well below this. The cap prevents a corrupt
    // command stream from filling app storage while retaining its first fault.
    auto& state = GetState();
    const u64 packet_hash = HashWords(packet, 0x5041434b4554ull);
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed) || state.file == nullptr ||
        state.packet_records >= 65536) {
        return;
    }
    ++state.packet_records;
    std::fprintf(state.file,
                 "P n=%llu submit=%llu engine=%s depth=%u semantic=%llu type=%u "
                 "opcode=0x%x dw=%zu hash=0x%016llx prefix=0x%016llx "
                 "aliasEpoch=%llu writeEpoch=%llu words=",
                 static_cast<unsigned long long>(++state.record_sequence),
                 static_cast<unsigned long long>(submit), EngineName(engine), depth,
                 static_cast<unsigned long long>(semantic), type, opcode, packet.size(),
                 static_cast<unsigned long long>(packet_hash),
                 static_cast<unsigned long long>(prefix),
                 static_cast<unsigned long long>(alias_epoch),
                 static_cast<unsigned long long>(write_epoch));
    for (std::size_t index = 0; index < packet.size(); ++index) {
        std::fprintf(state.file, index == 0 ? "%08x" : ",%08x", packet[index]);
    }
    std::fputc('\n', state.file);
}

inline void Draw(const u64 submit, const u32 depth, const u64 semantic,
                 const u32 opcode, const Regs& regs, const u64 packet_prefix,
                 const u64 state_generation, const u64 program_generation,
                 const u64 user_generation, const u64 alias_epoch,
                 const u64 write_epoch) noexcept {
    if (!Active()) {
        return;
    }
    const u64 regs_hash = HashBytes(regs.reg_array.data(),
                                    regs.reg_array.size() * sizeof(u32),
                                    0x52454753ull);
    Common::ContentFingerprint64 shaders{Common::FingerprintDomain::ShaderUserData ^
                                         0x53484452ull};
    Common::ContentFingerprint64 user_data{Common::FingerprintDomain::ShaderUserData ^
                                           0x55534552ull};
    for (u32 stage = 0; stage < 6; ++stage) {
        const ShaderProgram* program = regs.ProgramForStage(stage);
        shaders.Update(program, sizeof(*program));
        user_data.Update(program->user_data.data(), program->user_data.size() * sizeof(u32));
    }
    const u64 shader_hash = shaders.Finish();
    const u64 user_hash = user_data.Finish();

    // A raw user-data pointer is not stable between frames: games commonly
    // alternate descriptor-table ring slots.  Capture both the registers and
    // the bytes they reference so JIT/FEX comparisons can align by content
    // instead of mistaking a valid ring-slot change for corruption.
    struct UserReference {
        u8 stage{};
        u8 pair{};
        u16 bytes{};
        u8 backing{};
        u8 stable{};
        u32 map_type{};
        VAddr address{};
        VAddr map_base{};
        u64 map_size{};
        u64 hash_a{};
        u64 hash_b{};
        std::array<u32, 8> head{};
    };
    struct ResourceReference {
        u8 stage{};
        u8 pair{};
        u8 slot{};
        u16 bytes{};
        u8 backing{};
        u8 stable{};
        u32 map_type{};
        VAddr address{};
        VAddr map_base{};
        u64 map_size{};
        u64 hash_a{};
        u64 hash_b{};
        std::array<u32, 4> descriptor{};
        std::array<u32, 8> head{};
    };
    std::array<UserReference, 6 * (NUM_USER_DATA - 1)> references{};
    std::array<ResourceReference, 192> resource_references{};
    std::size_t reference_count = 0;
    std::size_t resource_reference_count = 0;
    auto* memory = Core::Memory::Instance();
    if (memory != nullptr) {
        auto guard = memory->AcquireSnapshotReadGuard();
        for (u32 stage = 0; stage < 6; ++stage) {
            const ShaderProgram* program = regs.ProgramForStage(stage);
            const u32 count = std::min<u32>(program->settings.num_user_regs,
                                            program->user_data.size());
            for (u32 pair = 0; pair + 1 < count; ++pair) {
                const VAddr address = static_cast<VAddr>(program->user_data[pair]) |
                                      (static_cast<VAddr>(program->user_data[pair + 1] & 0xffffu)
                                       << 32);
                constexpr u64 SnapshotBytes = 128;
                Core::MemoryManager::SnapshotAccessDiagnostic diagnostic{};
                if (address == 0 || (address & 3) != 0 ||
                    !memory->ValidateSnapshotAccess(guard, address, SnapshotBytes,
                                                    true, false, &diagnostic)) {
                    continue;
                }
                auto& reference = references[reference_count++];
                reference.stage = static_cast<u8>(stage);
                reference.pair = static_cast<u8>(pair);
                reference.bytes = SnapshotBytes;
                reference.address = address;
                reference.map_type = static_cast<u32>(diagnostic.area_type);
                reference.map_base = diagnostic.area_base;
                reference.map_size = diagnostic.area_size;
                std::array<u8, SnapshotBytes> bytes_a{};
                std::array<u8, SnapshotBytes> bytes_b{};
                const bool copied_a = memory->TryCopyBackingMemory(
                    guard, address, bytes_a.data(), bytes_a.size());
                const bool copied_b = copied_a && memory->TryCopyBackingMemory(
                    guard, address, bytes_b.data(), bytes_b.size());
                reference.backing = copied_a && copied_b;
                if (reference.backing) {
                    const u64 tag = 0x5544524546000000ull |
                                    (static_cast<u64>(stage) << 8) | pair;
                    reference.hash_a = HashBytes(bytes_a.data(), bytes_a.size(), tag);
                    reference.hash_b = HashBytes(bytes_b.data(), bytes_b.size(), tag);
                    reference.stable = reference.hash_a == reference.hash_b;
                    std::memcpy(reference.head.data(), bytes_a.data(),
                                reference.head.size() * sizeof(u32));
                    // A user-data pointer commonly names a table of 16-byte
                    // AMD buffer resource descriptors. Hash the bounded data
                    // behind each plausible descriptor as well: the table
                    // itself can be identical modulo allocation addresses
                    // while the lighting constants it selects differ.
                    if (pair >= 2) {
                        constexpr u64 MinResourceBytes = 128;
                        constexpr u64 MaxResourceBytes = 4096;
                        for (u8 slot = 0;
                             slot < bytes_a.size() / (4 * sizeof(u32)) &&
                             resource_reference_count < resource_references.size();
                             ++slot) {
                            std::array<u32, 4> descriptor{};
                            std::memcpy(descriptor.data(),
                                        bytes_a.data() + slot * descriptor.size() * sizeof(u32),
                                        descriptor.size() * sizeof(u32));
                            if ((descriptor[3] >> 30) != 0) {
                                continue;
                            }
                            const VAddr resource_address =
                                static_cast<VAddr>(descriptor[0]) |
                                (static_cast<VAddr>(descriptor[1] & 0x0fffu) << 32);
                            if (resource_address == 0 || (resource_address & 3) != 0 ||
                                resource_address == address) {
                                continue;
                            }
                            const u64 stride = (descriptor[1] >> 16) & 0x3fffu;
                            const u64 records = descriptor[2];
                            const u64 estimated_bytes =
                                stride != 0 && records <= MaxResourceBytes / stride
                                    ? stride * records
                                    : records;
                            const u64 resource_bytes = std::clamp<u64>(
                                estimated_bytes, MinResourceBytes, MaxResourceBytes);
                            Core::MemoryManager::SnapshotAccessDiagnostic resource_diagnostic{};
                            if (!memory->ValidateSnapshotAccess(
                                    guard, resource_address, resource_bytes, true, false,
                                    &resource_diagnostic)) {
                                continue;
                            }
                            auto& resource =
                                resource_references[resource_reference_count++];
                            resource.stage = static_cast<u8>(stage);
                            resource.pair = static_cast<u8>(pair);
                            resource.slot = slot;
                            resource.bytes = static_cast<u16>(resource_bytes);
                            resource.address = resource_address;
                            resource.map_type =
                                static_cast<u32>(resource_diagnostic.area_type);
                            resource.map_base = resource_diagnostic.area_base;
                            resource.map_size = resource_diagnostic.area_size;
                            resource.descriptor = descriptor;
                            std::array<u8, MaxResourceBytes> resource_a{};
                            std::array<u8, MaxResourceBytes> resource_b{};
                            const bool resource_copied_a = memory->TryCopyBackingMemory(
                                guard, resource_address, resource_a.data(), resource_bytes);
                            const bool resource_copied_b =
                                resource_copied_a && memory->TryCopyBackingMemory(
                                                         guard, resource_address,
                                                         resource_b.data(), resource_bytes);
                            resource.backing = resource_copied_a && resource_copied_b;
                            if (!resource.backing) {
                                continue;
                            }
                            const u64 resource_tag =
                                0x5245534f55520000ull |
                                (static_cast<u64>(stage) << 12) |
                                (static_cast<u64>(pair) << 4) | slot;
                            resource.hash_a = HashBytes(resource_a.data(), resource_bytes,
                                                        resource_tag);
                            resource.hash_b = HashBytes(resource_b.data(), resource_bytes,
                                                        resource_tag);
                            resource.stable = resource.hash_a == resource.hash_b;
                            std::memcpy(resource.head.data(), resource_a.data(),
                                        resource.head.size() * sizeof(u32));
                        }
                    }
                }
            }
        }
    }

    Common::ContentFingerprint64 draw{Common::FingerprintDomain::CommandStream ^
                                      0x44524157ull};
    draw.Update(&regs.primitive_type, sizeof(regs.primitive_type));
    draw.UpdateLittleEndian(regs.num_indices);
    draw.Update(&regs.num_instances, sizeof(regs.num_instances));
    draw.Update(&regs.index_base_address, sizeof(regs.index_base_address));
    draw.UpdateLittleEndian(regs.index_size);
    draw.UpdateLittleEndian(regs.max_index_size);
    draw.Update(&regs.index_buffer_type, sizeof(regs.index_buffer_type));
    const u64 draw_hash = draw.Finish();

    Common::ContentFingerprint64 attachments{Common::FingerprintDomain::AttachmentSet};
    attachments.Update(regs.color_buffers, sizeof(regs.color_buffers));
    attachments.Update(&regs.depth_buffer, sizeof(regs.depth_buffer));
    attachments.Update(&regs.color_target_mask, sizeof(regs.color_target_mask));
    attachments.Update(&regs.color_shader_mask, sizeof(regs.color_shader_mask));
    const u64 attachment_hash = attachments.Finish();

    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed) || state.file == nullptr) {
        return;
    }
    ++state.draw_records;
    std::fprintf(state.file,
                 "D n=%llu submit=%llu depth=%u semantic=%llu opcode=0x%x "
                 "prefix=0x%016llx regs=0x%016llx shaders=0x%016llx "
                 "user=0x%016llx attachments=0x%016llx draw=0x%016llx "
                 "stateGen=%llu programGen=%llu userGen=%llu "
                 "aliasEpoch=%llu writeEpoch=%llu\n",
                 static_cast<unsigned long long>(++state.record_sequence),
                 static_cast<unsigned long long>(submit), depth,
                 static_cast<unsigned long long>(semantic), opcode,
                 static_cast<unsigned long long>(packet_prefix),
                 static_cast<unsigned long long>(regs_hash),
                 static_cast<unsigned long long>(shader_hash),
                 static_cast<unsigned long long>(user_hash),
                 static_cast<unsigned long long>(attachment_hash),
                 static_cast<unsigned long long>(draw_hash),
                 static_cast<unsigned long long>(state_generation),
                 static_cast<unsigned long long>(program_generation),
                 static_cast<unsigned long long>(user_generation),
                 static_cast<unsigned long long>(alias_epoch),
                 static_cast<unsigned long long>(write_epoch));
    for (u32 stage = 0; stage < 6; ++stage) {
        const ShaderProgram* program = regs.ProgramForStage(stage);
        std::fprintf(state.file,
                     "U n=%llu submit=%llu semantic=%llu stage=%u num=%u words=",
                     static_cast<unsigned long long>(++state.record_sequence),
                     static_cast<unsigned long long>(submit),
                     static_cast<unsigned long long>(semantic), stage,
                     program->settings.num_user_regs);
        for (std::size_t index = 0; index < program->user_data.size(); ++index) {
            std::fprintf(state.file, index == 0 ? "%08x" : ",%08x",
                         program->user_data[index]);
        }
        std::fputc('\n', state.file);
    }
    for (std::size_t index = 0; index < reference_count; ++index) {
        const auto& reference = references[index];
        std::fprintf(state.file,
                     "T n=%llu submit=%llu semantic=%llu stage=%u pair=%u "
                     "addr=0x%llx mapType=%u mapBase=0x%llx mapSize=%llu mapOff=%llu "
                     "bytes=%u backing=%u stable=%u hashA=0x%016llx hashB=0x%016llx head=",
                     static_cast<unsigned long long>(++state.record_sequence),
                     static_cast<unsigned long long>(submit),
                     static_cast<unsigned long long>(semantic), reference.stage,
                     reference.pair, static_cast<unsigned long long>(reference.address),
                     reference.map_type,
                     static_cast<unsigned long long>(reference.map_base),
                     static_cast<unsigned long long>(reference.map_size),
                     static_cast<unsigned long long>(reference.address - reference.map_base),
                     reference.bytes, reference.backing, reference.stable,
                     static_cast<unsigned long long>(reference.hash_a),
                     static_cast<unsigned long long>(reference.hash_b));
        const std::size_t head_words =
            std::min<std::size_t>(reference.head.size(), reference.bytes / sizeof(u32));
        for (std::size_t word = 0; word < head_words; ++word) {
            std::fprintf(state.file, word == 0 ? "%08x" : ",%08x",
                         reference.head[word]);
        }
        std::fputc('\n', state.file);
    }
    for (std::size_t index = 0; index < resource_reference_count; ++index) {
        const auto& resource = resource_references[index];
        std::fprintf(state.file,
                     "Q n=%llu submit=%llu semantic=%llu stage=%u pair=%u slot=%u "
                     "addr=0x%llx mapType=%u mapBase=0x%llx mapSize=%llu mapOff=%llu "
                     "bytes=%u backing=%u stable=%u hashA=0x%016llx hashB=0x%016llx "
                     "desc=%08x,%08x,%08x,%08x head=",
                     static_cast<unsigned long long>(++state.record_sequence),
                     static_cast<unsigned long long>(submit),
                     static_cast<unsigned long long>(semantic), resource.stage,
                     resource.pair, resource.slot,
                     static_cast<unsigned long long>(resource.address), resource.map_type,
                     static_cast<unsigned long long>(resource.map_base),
                     static_cast<unsigned long long>(resource.map_size),
                     static_cast<unsigned long long>(resource.address - resource.map_base),
                     resource.bytes, resource.backing, resource.stable,
                     static_cast<unsigned long long>(resource.hash_a),
                     static_cast<unsigned long long>(resource.hash_b),
                     resource.descriptor[0], resource.descriptor[1],
                     resource.descriptor[2], resource.descriptor[3]);
        for (std::size_t word = 0; word < resource.head.size(); ++word) {
            std::fprintf(state.file, word == 0 ? "%08x" : ",%08x",
                         resource.head[word]);
        }
        std::fputc('\n', state.file);
    }
}

inline void CeDump(const u64 submit, const VAddr destination, const u32 offset,
                   const u32 size, const void* source, const u64 alias_epoch,
                   const u64 write_epoch) noexcept {
    if (!Active()) {
        return;
    }
    const u64 hash = HashBytes(source, size, 0x434544554d50ull);
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed) || state.file == nullptr) {
        return;
    }
    std::fprintf(state.file,
                 "C n=%llu submit=%llu dst=0x%llx offset=%u bytes=%u "
                 "hash=0x%016llx aliasEpoch=%llu writeEpoch=%llu\n",
                 static_cast<unsigned long long>(++state.record_sequence),
                 static_cast<unsigned long long>(submit),
                 static_cast<unsigned long long>(destination), offset, size,
                 static_cast<unsigned long long>(hash),
                 static_cast<unsigned long long>(alias_epoch),
                 static_cast<unsigned long long>(write_epoch));
}

} // namespace AmdGpu::Pm4DiffTrace

#endif
