#pragma once
#include <mutex>
#include <unordered_map>
#include <vector>
#include <utility>

namespace Lsx4::Portable {
// Completion callbacks publish immutable work; only the owning safe boundary
// consumes it. No callback executes a consumer while a producer lock is held.
template<class Key, class Work>
class StagedWorkMap {
    struct Entry { bool enabled{}; std::vector<Work> work; };
    std::mutex mutex_;
    std::unordered_map<Key, Entry> entries_;
public:
    void Reset(Key key, bool enabled) {
        std::scoped_lock lock(mutex_);
        entries_[key] = Entry{enabled, {}};
    }
    bool Publish(Key key, std::vector<Work> work) {
        std::scoped_lock lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end() || !it->second.enabled) return false;
        auto& pending = it->second.work;
        pending.insert(pending.end(), std::make_move_iterator(work.begin()),
                       std::make_move_iterator(work.end()));
        return true;
    }
    std::vector<Work> Take(Key key) {
        std::scoped_lock lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end() || !it->second.enabled) return {};
        auto result = std::move(it->second.work);
        it->second.work.clear();
        return result;
    }
};
}
