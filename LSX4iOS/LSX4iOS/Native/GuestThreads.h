// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include "GuestThreadAttributes.h"
#include <functional>
#include <map>

namespace Lsx4::Apple {
struct GuestTlsTemplate {
    std::uint64_t size{}, alignment{};
    std::vector<std::uint8_t> initial;
};
struct GuestThreadResult {std::uint32_t value{};bool waiting{};};
// One host executor schedules complete guest CPU frames. Guest pthread values,
// stacks and TLS never contain Apple pthread pointers. Selection is deterministic
// for interpreter/JIT differential checks; this is not a real-time scheduler.
class GuestThreads {
public:
    static constexpr std::uint64_t MainTcb=0x600100000000;
    static constexpr std::uint64_t ThreadReturn=0x700000000020;
    static constexpr std::uint64_t Quantum=4096;
    struct Thread {
        enum class State {Runnable,Blocked,Exited,Reaped};
        Translation::CpuFrame frame;
        std::uint64_t ip{}, handle{}, result{}, joiner{}, instructions{};
        GuestThreadParameters parameters;
        std::string name;
        State state{State::Runnable};
        bool detached{};
        std::function<bool(Thread&)> wake;
        std::vector<std::pair<std::uint64_t,std::uint64_t>> allocations;
    };
    GuestThreads(GuestAddressSpace& memory,std::vector<GuestTlsTemplate> tls);
    void StartMain(const Translation::CpuFrame& frame,std::uint64_t ip,
                   std::uint64_t stack,std::uint64_t stackBytes);
    std::uint32_t Create(std::uint64_t output,const GuestThreadParameters& parameters,
                         std::uint64_t entry,std::uint64_t argument,std::uint64_t name,
                         const Translation::CpuFrame& parent);
    GuestThreadResult Join(std::uint64_t target,std::uint64_t output);
    std::uint32_t Detach(std::uint64_t target);
    void Exit(std::uint64_t value);
    void Block(std::function<bool(Thread&)> wake);
    // Save current state, retry blocked operations, then choose the next runnable
    // guest. False means no guest can run, not that waits completed successfully.
    bool Switch(Translation::CpuFrame& frame,std::uint64_t& ip);
    Thread& Current() {return threads_.at(current_);}
    const Thread* Find(std::uint64_t handle) const;
    const auto& All() const {return threads_;}
    std::uint64_t Switches() const {return switches_;}
    bool AllExited() const;
private:
    void Release(Thread& thread);
    GuestAddressSpace& memory_;
    std::vector<GuestTlsTemplate> tls_;
    std::map<std::uint64_t,Thread> threads_;
    std::uint64_t current_{MainTcb+0x1000},next_{1},switches_{};
};
}
