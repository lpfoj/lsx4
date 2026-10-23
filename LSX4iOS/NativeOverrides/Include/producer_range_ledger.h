#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace PortableGpu {
// No graphics API or guest-platform dependencies. Unknown, pending, evicted,
// reused allocation and mismatched timeline evidence all deny admission.
struct ProducerAllocation {
    std::uint64_t object{}, handle{};
    bool operator==(const ProducerAllocation&) const = default;
};
struct ProducerTicket {
    ProducerAllocation allocation{};
    std::uint64_t timeline{}, tick{}, pending_batch{};
    bool operator==(const ProducerTicket&) const = default;
};
class ProducerRangeLedger {
public:
    explicit ProducerRangeLedger(std::size_t maximum = 8192) : maximum{maximum} {}
    void Clear() { ranges.clear(); pending.clear(); pending_batch = 0; pending_timeline = 0; }
    void Invalidate(std::uint64_t begin, std::uint64_t size) {
        if (!Valid(begin, size)) { Clear(); return; }
        const auto end = begin + size;
        auto it = ranges.upper_bound(begin);
        if (it != ranges.begin()) --it;
        while (it != ranges.end() && it->first < end) {
            const auto first = it->first;
            const auto old = it->second;
            if (old.end <= begin) { ++it; continue; }
            it = ranges.erase(it);
            if (first < begin) ranges.emplace(first, Segment{begin, old.ticket});
            if (old.end > end) { ranges.emplace(end, Segment{old.end, old.ticket}); break; }
        }
        Bound();
    }
    void Write(std::uint64_t begin, std::uint64_t size, ProducerTicket ticket) {
        if (!Valid(begin, size)) { Clear(); return; }
        Invalidate(begin, size);
        ranges.emplace(begin, Segment{begin + size, ticket});
        if (ticket.pending_batch) {
            if (pending_batch != ticket.pending_batch) {
                pending.clear(); pending_batch = ticket.pending_batch; pending_timeline = ticket.timeline;
            }
            pending.emplace_back(begin, begin + size);
            if (pending.size() > maximum) { Clear(); return; }
        }
        Bound();
    }
    void Commit(std::uint64_t batch, std::uint64_t timeline, std::uint64_t tick) {
        if (!batch || !timeline || !tick) return;
        if (batch != pending_batch || timeline != pending_timeline) return;
        for (const auto& [begin, end] : pending) {
            auto it = ranges.upper_bound(begin);
            if (it != ranges.begin()) --it;
            while (it != ranges.end() && it->first < end) {
                auto& range = it->second;
                if (range.end > begin && range.ticket.pending_batch == batch &&
                    range.ticket.timeline == timeline) {
                    range.ticket.tick = tick; range.ticket.pending_batch = 0;
                }
                ++it;
            }
        }
        pending.clear(); pending_batch = 0; pending_timeline = 0;
    }
    std::optional<std::uint64_t> CompletedProducer(
        std::uint64_t begin, std::uint64_t size, ProducerAllocation allocation,
        std::uint64_t timeline, std::uint64_t visible_through,
        std::uint64_t completed_through) const {
        if (!Valid(begin, size) || !timeline) return {};
        const auto end = begin + size;
        auto it = ranges.upper_bound(begin);
        if (it == ranges.begin()) return {};
        --it;
        std::uint64_t cursor = begin, tick = 0;
        while (cursor < end) {
            if (it == ranges.end() || it->first > cursor || it->second.end <= cursor) return {};
            const auto& t = it->second.ticket;
            if (t.allocation != allocation || t.timeline != timeline || t.pending_batch ||
                !t.tick || t.tick > visible_through || t.tick > completed_through) return {};
            tick = std::max(tick, t.tick);
            cursor = std::min(end, it->second.end); ++it;
        }
        return tick;
    }
    std::size_t Size() const { return ranges.size(); }
private:
    struct Segment { std::uint64_t end; ProducerTicket ticket; };
    static bool Valid(std::uint64_t begin, std::uint64_t size) {
        return size && size <= std::numeric_limits<std::uint64_t>::max() - begin;
    }
    void Bound() { if (ranges.size() > maximum) Clear(); }
    const std::size_t maximum;
    std::map<std::uint64_t, Segment> ranges;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> pending;
    std::uint64_t pending_batch{};
    std::uint64_t pending_timeline{};
};
}
