// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include "core/libraries/audio/audio_mix.h"
#include <optional>
#include <future>
#include <utility>
#include <string_view>
namespace Lsx4::Apple {
class GuestAudioSink {
public:
    virtual ~GuestAudioSink()=default;
    virtual bool Initialize()=0;
    virtual bool Open(std::uint32_t handle)=0;
    virtual void Close(std::uint32_t handle)=0;
    // Sink methods must synchronize device/port state: Output may run on a
    // bounded worker while the guest executor changes another port.
    // Consumes interleaved stereo at 48 kHz and waits for playback completion.
    // A failed/absent output device must return false rather than discard PCM.
    virtual bool Output(std::uint32_t handle,std::span<const float> samples)=0;
};
std::unique_ptr<GuestAudioSink> CreatePlatformAudioSink();
class GuestAudio {
public:
    struct Diagnostics {std::uint64_t completedBuffers{},completedFrames{},nonSilentBuffers{},deviceFailures{};} diagnostics;
    struct OutputJob {
        std::future<bool> playback;
        std::uint32_t handle{},frames{},channels{};
        bool nonSilent{},consumed{};
        std::uint64_t value{};
    };
    using PendingOutput=std::shared_ptr<OutputJob>;
    PendingOutput TakePendingOutput() {return std::exchange(pendingOutput_,{});}
    std::optional<std::uint64_t> FinishOutput(const PendingOutput& job);
    bool WaitForOutput();
    std::uint64_t PendingCount() const;
    explicit GuestAudio(std::unique_ptr<GuestAudioSink> sink):sink_(std::move(sink)){}
    ~GuestAudio();
    std::optional<std::uint64_t> Call(std::string_view identity,const Translation::CpuFrame& frame,GuestAddressSpace& memory,bool asynchronous=false);
private:
    struct Port {
        std::uint32_t handle{},frames{},type{};
        PendingOutput pending;
        Libraries::AudioOut::AudioBufferFormat format;
        std::array<float,8> gains{1,1,1,1,1,1,1,1};
        Libraries::AudioOut::AudioConversionGuard conversion;
    };
    std::unique_ptr<GuestAudioSink> sink_;
    bool initialized_{};
    PendingOutput pendingOutput_;
    std::array<std::optional<Port>,25> ports_{};
};
}
