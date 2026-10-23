// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ImGui::Core {
// Additive host-input route. Legacy Android callers retain their existing queue.
// A token identifies one focused text field in one modal-layer lifetime.
class HostTextSessionQueue {
public:
    enum class Action { Text, Backspace, Submit, Cancel };
    struct Event { std::uint64_t session; Action action; std::string text; std::uint32_t count; };
    static constexpr std::size_t MaxTextBytes=16*1024, MaxQueuedCost=64*1024, MaxEvents=128;
    static bool ValidText(std::string_view text) {
        if(text.empty() || text.size()>MaxTextBytes)return false;
        for(std::size_t i=0;i<text.size();) {
            const auto lead=static_cast<unsigned char>(text[i++]);
            if(lead==0)return false;
            if(lead<0x80)continue;
            unsigned count=0;std::uint32_t point=0,minimum=0;
            if(lead>=0xc2 && lead<=0xdf){count=1;point=lead&0x1f;minimum=0x80;}
            else if(lead>=0xe0 && lead<=0xef){count=2;point=lead&0xf;minimum=0x800;}
            else if(lead>=0xf0 && lead<=0xf4){count=3;point=lead&7;minimum=0x10000;}
            else return false;
            if(count>text.size()-i)return false;
            for(unsigned n=0;n<count;++n) {
                const auto byte=static_cast<unsigned char>(text[i++]);
                if((byte&0xc0)!=0x80)return false;
                point=(point<<6)|(byte&0x3f);
            }
            if(point<minimum || point>0x10ffff || (point>=0xd800 && point<=0xdfff))return false;
        }
        return true;
    }
    std::uint64_t Current() const { return active_.load(std::memory_order_acquire); }
    void Invalidate() {
        std::scoped_lock lock{mutex_};ClearLocked();
    }
    void Focus(std::uint32_t field) {
        if(!field && !Current())return;
        std::scoped_lock lock{mutex_};
        if(field==field_)return;
        ClearLocked();
        if(field && generation_!=std::numeric_limits<std::uint64_t>::max()) {
            field_=field;active_.store(++generation_,std::memory_order_release);
        }
    }
    bool Queue(std::uint64_t session,Action action,std::string_view text={},std::uint32_t count=1) {
        if(static_cast<unsigned>(action)>static_cast<unsigned>(Action::Cancel))return false;
        const std::size_t cost=action==Action::Text?text.size():count;
        if(action==Action::Text ? !ValidText(text) : (!text.empty() || !count || count>MaxTextBytes))return false;
        if((action==Action::Submit || action==Action::Cancel) && count!=1)return false;
        std::scoped_lock lock{mutex_};
        if(!session || session!=active_.load(std::memory_order_relaxed) ||
           events_.size()>=MaxEvents || cost>MaxQueuedCost-queued_cost_)return false;
        events_.push_back({session,action,std::string(text),count});queued_cost_+=cost;
        return true;
    }
    template<class Consumer> void Drain(Consumer&& consume) {
        DrainReady([&](const Event& event){consume(event);return true;});
    }
    template<class Consumer> void DrainReady(Consumer&& consume) {
        // False leaves this event and its successors queued. Semantic submit
        // waits until preceding ImGui text/key events have reached the field.
        // Consumers must not reenter this owner while its lock is held.
        std::scoped_lock lock{mutex_};
        while(!events_.empty()) {
            const auto& event=events_.front();
            if(event.session && event.session==active_.load(std::memory_order_relaxed) &&
               !consume(event))break;
            queued_cost_-=event.action==Action::Text?event.text.size():event.count;
            events_.pop_front();
        }
    }

private:
    void ClearLocked() { active_.store(0,std::memory_order_release);field_=0;events_.clear();queued_cost_=0; }
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> active_{};
    std::uint64_t generation_{};
    std::uint32_t field_{};
    std::deque<Event> events_;
    std::size_t queued_cost_{};
};
}
