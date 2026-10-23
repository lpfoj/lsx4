#include "producer_readback_bridge.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <unordered_map>

extern "C" {
// Exported, retained counters are inspected after the FPS window; no debugger
// evaluation or periodic file I/O is required in the gameplay hot path.
__attribute__((used,visibility("default"))) std::atomic<std::uint64_t> lsx4_producer_readback_hits{0};
__attribute__((used,visibility("default"))) std::atomic<std::uint64_t> lsx4_producer_readback_bytes{0};
__attribute__((used,visibility("default"))) std::atomic<std::uint64_t> lsx4_producer_readback_queries{0};
__attribute__((used,visibility("default"))) std::atomic<std::uint64_t> lsx4_producer_readback_pending_writes{0};
__attribute__((used,visibility("default"))) std::atomic<std::uint64_t> lsx4_producer_readback_commits{0};
}
namespace PortableGpu {
namespace {
struct Timeline { std::uint64_t identity{}, visible{}; };
struct Cache { ProducerRangeLedger ledger; std::uint64_t batch{}, timeline{}; };
std::mutex proof_mutex;
std::unordered_map<const void*, Timeline> timelines;
std::unordered_map<const void*, Cache> caches;
std::uint64_t serial = 0;
}
bool ProducerReadbackEnabled() {
    static const bool enabled = [] { const char* v = std::getenv("EXECUTOR_PRODUCER_MAPPED_READBACK");
        return v && v[0] == '1' && v[1] == '\0'; }();
    return enabled;
}
void ResetProducerCache(const void* cache) {
    if (!ProducerReadbackEnabled()) return;
    std::scoped_lock lock{proof_mutex}; caches.erase(cache);
}
void ResetProducerTimeline(const void* scheduler) {
    if (!ProducerReadbackEnabled()) return;
    std::scoped_lock lock{proof_mutex}; timelines[scheduler] = Timeline{++serial, 0};
}
void BeginProducerBindings(const void* cache, const void* scheduler) {
    if (!ProducerReadbackEnabled()) return;
    std::scoped_lock lock{proof_mutex};
    auto it = timelines.find(scheduler); if (it == timelines.end()) return;
    auto& c = caches[cache]; c.batch = ++serial; c.timeline = it->second.identity;
}
void RecordPendingProducer(const void* cache, const void* scheduler,
    ProducerAllocation allocation, std::uint64_t begin, std::uint64_t size) {
    if (!ProducerReadbackEnabled()) return;
    std::scoped_lock lock{proof_mutex};
    auto t = timelines.find(scheduler); auto c = caches.find(cache);
    if (t == timelines.end() || c == caches.end() || !c->second.batch ||
        c->second.timeline != t->second.identity) {
        if (c != caches.end()) c->second.ledger.Invalidate(begin,size);
        return;
    }
    c->second.ledger.Write(begin,size,ProducerTicket{allocation,t->second.identity,0,c->second.batch});
    lsx4_producer_readback_pending_writes.fetch_add(1,std::memory_order_relaxed);
}
void CommitProducerBindings(const void* cache, const void* scheduler, std::uint64_t tick) {
    if (!ProducerReadbackEnabled()) return;
    std::scoped_lock lock{proof_mutex}; auto c = caches.find(cache); auto t = timelines.find(scheduler);
    if (c == caches.end() || t == timelines.end() || c->second.timeline != t->second.identity) return;
    c->second.ledger.Commit(c->second.batch,t->second.identity,tick); c->second.batch = 0;
    lsx4_producer_readback_commits.fetch_add(1,std::memory_order_relaxed);
}
void InvalidateProducerRange(const void* cache, std::uint64_t begin, std::uint64_t size) {
    if (!ProducerReadbackEnabled() || !size) return;
    std::scoped_lock lock{proof_mutex}; auto c = caches.find(cache);
    if (c != caches.end()) c->second.ledger.Invalidate(begin,size);
}
void RecordProducerHostBarrier(const void* scheduler, std::uint64_t tick) {
    if (!ProducerReadbackEnabled()) return;
    std::scoped_lock lock{proof_mutex}; auto t = timelines.find(scheduler);
    if (t != timelines.end()) t->second.visible = std::max(t->second.visible,tick);
}
std::optional<std::uint64_t> FindCompletedProducer(const void* cache, const void* scheduler,
    ProducerAllocation allocation, std::uint64_t begin, std::uint64_t size,
    std::uint64_t completed_through) {
    lsx4_producer_readback_queries.fetch_add(1,std::memory_order_relaxed);
    std::scoped_lock lock{proof_mutex}; auto c = caches.find(cache); auto t = timelines.find(scheduler);
    if (c == caches.end() || t == timelines.end()) return {};
    return c->second.ledger.CompletedProducer(begin,size,allocation,t->second.identity,
        t->second.visible,completed_through);
}
std::optional<std::uint64_t> FindSealedProducer(const void* cache, const void* scheduler,
    ProducerAllocation allocation, std::uint64_t begin, std::uint64_t size) {
    lsx4_producer_readback_queries.fetch_add(1,std::memory_order_relaxed);
    std::scoped_lock lock{proof_mutex}; auto c = caches.find(cache); auto t = timelines.find(scheduler);
    if (c == caches.end() || t == timelines.end()) return {};
    // The ceiling is recorded visibility, rather than current completion.
    // Callback registration + WaitRetirement publishes pending ended submits
    // and completes the selected producer before its mapped bytes are read.
    return c->second.ledger.CompletedProducer(begin,size,allocation,t->second.identity,
        t->second.visible,t->second.visible);
}
void RecordProducerReadbackHit(std::uint64_t bytes, std::uint64_t producer_tick, std::uint64_t tail_tick) {
    const auto count = lsx4_producer_readback_hits.fetch_add(1,std::memory_order_relaxed) + 1;
    lsx4_producer_readback_bytes.fetch_add(bytes,std::memory_order_relaxed);
    // One receipt proves the real backend route; counters supply subsequent totals.
    if (count == 1) std::fprintf(stderr,"[PORTABLE_PRODUCER_READBACK] first=1 bytes=%llu producer=%llu tail=%llu\n",
        static_cast<unsigned long long>(bytes),static_cast<unsigned long long>(producer_tick),
        static_cast<unsigned long long>(tail_tick));
}
}
