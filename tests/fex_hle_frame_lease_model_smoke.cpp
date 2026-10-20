// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_hle_frame_lease_abi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace {

using namespace Lsx4::FexHleLease;

[[noreturn]] void Fail(const char* check) {
    std::fprintf(stderr, "FEX_HLE_FRAME_LEASE_MODEL FAIL check=%s\n", check);
    std::abort();
}

void Require(const bool condition, const char* check) {
    if (!condition) {
        Fail(check);
    }
}

void TestMonotonicNoWrap() {
    std::atomic<std::uint64_t> counter{0};
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    Require(TryAllocateToken(counter, first) &&
                TryAllocateToken(counter, second) && first == 1 && second == 2,
            "unique-monotonic-token");
    counter.store(std::numeric_limits<std::uint64_t>::max() - 1,
                  std::memory_order_relaxed);
    std::uint64_t last = 0;
    std::uint64_t wrapped = 7;
    std::uint32_t exhausted_attempts = 0;
    Require(TryAllocateToken(counter, last) &&
                last == std::numeric_limits<std::uint64_t>::max() &&
                !TryAllocateToken(counter, wrapped, &exhausted_attempts) &&
                wrapped == 0 && exhausted_attempts == 1 &&
                counter.load(std::memory_order_relaxed) ==
                    std::numeric_limits<std::uint64_t>::max(),
            "token-wrap-fails-closed");
}

void TestTokenContentionIsBoundedAndUnique() {
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t tokens_per_thread = 4096;
    constexpr std::size_t token_count = thread_count * tokens_per_thread;
    std::atomic<std::uint64_t> counter{0};
    std::atomic<bool> start{false};
    std::atomic<bool> all_success{true};
    std::atomic<std::uint32_t> maximum_attempts{0};
    std::vector<std::uint64_t> tokens(token_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {
        threads.emplace_back([&, thread_index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const std::size_t begin = thread_index * tokens_per_thread;
            for (std::size_t offset = 0; offset < tokens_per_thread; ++offset) {
                std::uint32_t attempts = 0;
                if (!TryAllocateToken(counter, tokens[begin + offset],
                                      &attempts) ||
                    attempts == 0 || attempts > MaxTokenAllocateAttempts) {
                    all_success.store(false, std::memory_order_relaxed);
                    return;
                }
                std::uint32_t observed =
                    maximum_attempts.load(std::memory_order_relaxed);
                while (observed < attempts &&
                       !maximum_attempts.compare_exchange_weak(
                           observed, attempts, std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    std::sort(tokens.begin(), tokens.end());
    Require(all_success.load(std::memory_order_relaxed) &&
                maximum_attempts.load(std::memory_order_relaxed) >= 1 &&
                maximum_attempts.load(std::memory_order_relaxed) <=
                    MaxTokenAllocateAttempts &&
                counter.load(std::memory_order_relaxed) == token_count &&
                tokens.front() == 1 && tokens.back() == token_count &&
                std::adjacent_find(tokens.begin(), tokens.end()) == tokens.end(),
            "token-contention-bounded-unique");
}

void TestProtocolGenerationFingerprint() {
    std::uint64_t generation = 0;
    Require(TryAdvanceManifestGeneration(generation) &&
                generation == InitialManifestGeneration &&
                IsCurrentManifestGeneration(generation),
            "protocol-generation-seeded");
    const std::uint64_t first = generation;
    Require(TryAdvanceManifestGeneration(generation) &&
                generation == first + 1 &&
                IsCurrentManifestGeneration(generation),
            "protocol-generation-advanced");
    Require(!IsCurrentManifestGeneration(1) &&
                !IsCurrentManifestGeneration(ManifestGenerationSeed) &&
                !IsCurrentManifestGeneration(
                    generation ^ (UINT64_C(1) << 63)),
            "legacy-or-mismatched-generation-rejected");
    generation = ManifestGenerationSeed | ManifestOrdinalMask;
    Require(!TryAdvanceManifestGeneration(generation) &&
                generation ==
                    (ManifestGenerationSeed | ManifestOrdinalMask),
            "protocol-generation-wrap-fails-closed");

    std::uint64_t funnel = InitialManifestGeneration;
    std::uint64_t box64 = InitialManifestGeneration;
    CombinedManifestGeneration combined_state{};
    std::uint64_t combined = 0;
    Require(ComposeCombinedManifestGeneration(
                combined_state, funnel, box64, combined) &&
                combined == InitialManifestGeneration &&
                IsCombinedManifestGeneration(combined_state),
            "combined-generation-seeded");
    const std::uint64_t first_combined = combined;
    Require(ComposeCombinedManifestGeneration(
                combined_state, funnel, box64, combined) &&
                combined == first_combined,
            "same-source-pair-keeps-publication");
    Require(TryAdvanceManifestGeneration(funnel) &&
                ComposeCombinedManifestGeneration(
                    combined_state, funnel, box64, combined) &&
                combined == first_combined + 1 &&
                combined_state.funnel_source == funnel &&
                combined_state.box64_source == box64,
            "funnel-change-advances-publication");
    Require(TryAdvanceManifestGeneration(box64) &&
                ComposeCombinedManifestGeneration(
                    combined_state, funnel, box64, combined) &&
                combined == first_combined + 2,
            "box64-change-advances-publication");
    Require(!ComposeCombinedManifestGeneration(
                combined_state, funnel - 1, box64, combined) &&
                combined == 0,
            "source-rollback-fails-closed");
    Require(!ComposeCombinedManifestGeneration(
                combined_state,
                funnel ^ (UINT64_C(1) << 63), box64, combined) &&
                combined == 0,
            "combined-generation-tag-mismatch-fails-closed");
    CombinedManifestGeneration exhausted{
        funnel, box64, ManifestGenerationSeed | ManifestOrdinalMask};
    Require(IsCombinedManifestGeneration(exhausted) &&
                !ComposeCombinedManifestGeneration(
                    exhausted, funnel + 1, box64, combined) &&
                combined == 0 &&
                exhausted.publication ==
                    (ManifestGenerationSeed | ManifestOrdinalMask),
            "combined-publication-wrap-fails-closed");
}

void TestAbaStaleCleanupAndMetadataValidation() {
    alignas(SlotSize) std::array<std::byte, SlotSize * 2> pool{};
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(pool.data());
    const PoolGeometry geometry{base, pool.size(), SlotSize, FrameOffset};
    auto& guard = *reinterpret_cast<std::uint64_t*>(pool.data());
    constexpr std::uint64_t stub = UINT64_C(0x1122334455667788);
    constexpr std::uint64_t first_token = 41;
    constexpr std::uint64_t second_token = 42;
    const std::uint64_t frame = base + FrameOffset;

    guard = first_token;
    FrameMetadata first{0, base, first_token, stub};
    FrameMetadata stale = first;
    Require(ValidateOwnedFrame(geometry, frame, first, stub),
            "owned-frame-valid");
    Require(CleanupOwnedFrame(geometry, frame, first, stub) ==
                CleanupResult::Released && guard == 0,
            "first-owner-release");

    guard = second_token;
    FrameMetadata second{0, base, second_token, stub};
    Require(CleanupOwnedFrame(geometry, frame, stale, stub) ==
                CleanupResult::StaleOwner && guard == second_token,
            "aba-stale-cleanup-does-not-clear-new-owner");
    Require(CleanupOwnedFrame(geometry, frame, second, stub) ==
                CleanupResult::Released && guard == 0 &&
                CleanupOwnedFrame(geometry, frame, second, stub) ==
                    CleanupResult::AlreadyReleased,
            "idempotent-owner-cleanup");

    guard = second_token;
    FrameMetadata corrupt{0, base + 1, second_token, stub};
    Require(CleanupOwnedFrame(geometry, frame, corrupt, stub) ==
                CleanupResult::InvalidMetadata && guard == second_token,
            "misaligned-guard-rejected-before-dereference");
    corrupt = {0, base, second_token, stub + 1};
    Require(CleanupOwnedFrame(geometry, frame, corrupt, stub) ==
                CleanupResult::InvalidMetadata && guard == second_token,
            "stub-mismatch-rejected");
    corrupt = {0, base, second_token, stub};
    Require(CleanupOwnedFrame(geometry, frame + 8, corrupt, stub) ==
                CleanupResult::InvalidMetadata && guard == second_token,
            "frame-mismatch-rejected");
    guard = 0;
}

void TestFallbackPrefillAndBoundedPool() {
    FrameMetadata metadata{
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
    };
    ClearStackFallbackMetadata(metadata);
    Require(metadata.original_return == 0 && metadata.guard_pointer == 0 &&
                metadata.lease_token == 0 && metadata.stub_begin == 0,
            "fallback-clears-high-halves");

    constexpr std::uint32_t slots = SlotCount;
    std::vector<std::uint64_t> guards(slots, 17);
    std::atomic<std::uint64_t> cursor{0};
    std::uint32_t acquired = 0;
    std::uint32_t attempts = 0;
    Require(!TryAcquireBounded(guards.data(), slots, cursor, 19, acquired,
                               attempts) &&
                attempts == MaxAcquireAttempts && acquired == slots,
            "full-pool-bounded-failure");
    guards[7] = 0;
    cursor.store(0, std::memory_order_relaxed);
    Require(TryAcquireBounded(guards.data(), slots, cursor, 23, acquired,
                              attempts) &&
                acquired == 7 && attempts == 8 && guards[7] == 23,
            "bounded-acquire-finds-free-slot");
}

void TestDepthBound() {
    std::uint32_t depth = 0;
    for (std::uint32_t index = 0; index < MaxNestedLeases; ++index) {
        Require(TryEnterDepth(depth), "depth-admitted-through-limit");
    }
    Require(depth == MaxNestedLeases && !TryEnterDepth(depth),
            "depth-limit-fails-closed");
    while (depth != 0) {
        LeaveDepth(depth);
    }
    Require(depth == 0, "depth-release-balanced");
}

} // namespace

int main() {
    TestMonotonicNoWrap();
    TestTokenContentionIsBoundedAndUnique();
    TestProtocolGenerationFingerprint();
    TestAbaStaleCleanupAndMetadataValidation();
    TestFallbackPrefillAndBoundedPool();
    TestDepthBound();
    std::puts("FEX_HLE_FRAME_LEASE_MODEL PASS token_no_wrap=1 "
              "token_contention_bounded=1 aba=1 "
              "fallback_qword_zero=1 bounded_pool=1 corrupt_metadata=1 "
              "depth64=1 generation_fingerprint=1 combined_generation=1");
    return 0;
}
