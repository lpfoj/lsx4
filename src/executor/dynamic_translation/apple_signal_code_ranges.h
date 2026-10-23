// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Lsx4::Translation {
// Only slab allocation/retirement writes this registry. A signal handler must
// never take ExecutableArena's mutex to classify its interrupted program counter.
class AppleSignalCodeRange {
    struct Range {
        std::atomic<std::uintptr_t> begin,end;
        Range() noexcept:begin(0),end(0){}
    };
    inline static std::array<Range,512> ranges_{};
    std::size_t slot_=ranges_.size();
public:
    AppleSignalCodeRange()=default;
    AppleSignalCodeRange(const AppleSignalCodeRange&)=delete;
    AppleSignalCodeRange& operator=(const AppleSignalCodeRange&)=delete;
    ~AppleSignalCodeRange(){Clear();}
    bool Publish(std::uintptr_t first,std::size_t bytes) noexcept {
        if(!first || !bytes || first+bytes<first || slot_!=ranges_.size())return false;
        for(std::size_t i=0;i<ranges_.size();++i){
            std::uintptr_t empty=0;
            if(ranges_[i].begin.compare_exchange_strong(empty,first,std::memory_order_acq_rel)){
                slot_=i;ranges_[i].end.store(first+bytes,std::memory_order_release);return true;
            }
        }
        return false;
    }
    void Clear() noexcept {
        if(slot_==ranges_.size())return;
        ranges_[slot_].end.store(0,std::memory_order_release);
        ranges_[slot_].begin.store(0,std::memory_order_release);slot_=ranges_.size();
    }
    static bool Contains(std::uintptr_t pc) noexcept {
        static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
        for(const auto& range:ranges_){
            const auto last=range.end.load(std::memory_order_acquire);
            if(!last || pc>=last)continue;
            const auto first=range.begin.load(std::memory_order_acquire);
            if(first && pc>=first && last==range.end.load(std::memory_order_acquire))return true;
        }
        return false;
    }
};
}
