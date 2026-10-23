// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include "GuestEqueue.h"
#include "GuestVideoOut.h"
#include "GuestFrame.h"
#include <map>
namespace Lsx4::Apple {
class GuestGpu {
public:
    explicit GuestGpu(bool captureDigests=true):captureDigests_(captureDigests) {}
    struct Metrics {
        std::uint64_t submissions{},packets{},computeDispatches{},depthDraws{},colorDraws{},endOfPipes{},framesOutput{},submissionsDone{},frameDigest{},visibleDraws{},surfaceTransfers{},bytesWritten{},writeDigest{};
    } metrics;
    // Processes the supported prefix in order. The caller keeps the guest at
    // its submit boundary until all rendering and presentation are implemented.
    std::pair<std::string,std::string> SubmitAndFlip(const Translation::CpuFrame& frame,GuestAddressSpace& memory,GuestEqueues& queues);
    std::pair<std::string,std::string> Present(const Translation::CpuFrame& frame,GuestAddressSpace& memory,
        GuestEqueues& queues,GuestVideoOut& video,const GuestFrameSink& sink);
    bool FinishSubmission() {
        if(!submissionComplete_)return false;
        ++metrics.submissionsDone;return true;
    }
private:
    bool captureDigests_{true};
    bool submissionComplete_{true};
    std::array<std::uint32_t,7> flipReservation_{};
    std::map<std::uint32_t,std::uint32_t> registers_;
    std::uint32_t depthExtent_{};
    void DrawColor(std::span<const std::uint32_t> packet,GuestAddressSpace& memory);
    void DrawDepth(std::span<const std::uint32_t> packet,GuestAddressSpace& memory);
    void Compute(std::span<const std::uint32_t> packet,GuestAddressSpace& memory);
    void Write(std::uint64_t address,std::span<const std::uint8_t> bytes,GuestAddressSpace& memory);
};
}
