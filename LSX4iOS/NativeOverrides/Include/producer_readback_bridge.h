#pragma once
#include "producer_range_ledger.h"

namespace PortableGpu {
bool ProducerReadbackEnabled();
void ResetProducerCache(const void* cache);
void ResetProducerTimeline(const void* scheduler);
void BeginProducerBindings(const void* cache, const void* scheduler);
void RecordPendingProducer(const void* cache, const void* scheduler,
                           ProducerAllocation allocation, std::uint64_t begin, std::uint64_t size);
void CommitProducerBindings(const void* cache, const void* scheduler, std::uint64_t tick);
void InvalidateProducerRange(const void* cache, std::uint64_t begin, std::uint64_t size);
void RecordProducerHostBarrier(const void* scheduler, std::uint64_t tick);
std::optional<std::uint64_t> FindCompletedProducer(const void* cache, const void* scheduler,
    ProducerAllocation allocation, std::uint64_t begin, std::uint64_t size,
    std::uint64_t completed_through);
// HOST barrier is recorded in an ended command buffer. The original
// publication callback still waits for that producer; this is NOT a claim
// that its bytes are already complete at admission.
std::optional<std::uint64_t> FindSealedProducer(const void* cache, const void* scheduler,
    ProducerAllocation allocation, std::uint64_t begin, std::uint64_t size);
void RecordProducerReadbackHit(std::uint64_t bytes, std::uint64_t producer_tick, std::uint64_t tail_tick);
}
