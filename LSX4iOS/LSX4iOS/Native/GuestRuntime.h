// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestImage.h"
#include "GuestFrame.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <map>

namespace Lsx4::Apple {
inline constexpr std::uint64_t TitleInstructionLimit = 10000000;
struct GuestRunControl {
    // Immutable once execution starts. Interactive sessions omit expensive
    // diagnostic byte hashes; all rendering and memory checks still run.
    bool captureGpuDigests{true};
    bool captureInstructionTrace{true};
    bool preparedScalar{true};
    std::atomic<bool> cancelled{false};
    bool IsCancelled() const {return cancelled.load(std::memory_order_relaxed);}
    void Cancel() {cancelled.store(true,std::memory_order_relaxed);}
};
struct GuestRunResult {
    std::string stop, detail, phase;
    std::uint64_t instructions{}, address{}, faultAddress{}, relocations{};
    std::uint64_t registeredDestructors{};
    bool heapAPIRegistered{};
    bool splashVisible{true};
    std::uint64_t bytesRead{},bytesWritten{},consoleBytes{};
    std::string consoleOutput;
    std::vector<std::string> openedPaths,attemptedPaths;
    std::uint64_t nativeInstructions{}, nativeCompilations{}, nativeMismatches{};
    std::uint64_t nativeCheckedInstructions{};
    std::string nativeValidation{"disabled"};
    std::uint64_t padSamplesRead{},padNonNeutralSamplesRead{};
    std::uint32_t padButtonsReadMask{};
    std::uint64_t threadsCreated{},contextSwitches{},activeThread{};
    std::uint64_t audioBuffers{},audioFrames{},audioNonSilentBuffers{},audioDeviceFailures{},audioPendingBuffers{};
    bool asynchronousAudio{};
    bool gpuDigestsRecorded{true};
    bool instructionTraceRecorded{true};
    std::uint64_t gpuSubmissions{},gpuPackets{},gpuComputeDispatches{},gpuDepthDraws{},gpuColorDraws{},gpuEndOfPipes{},gpuFramesOutput{},gpuSubmissionsDone{},gpuFrameDigest{},gpuVisibleDraws{},gpuSurfaceTransfers{},gpuBytesWritten{},gpuWriteDigest{};
    std::array<std::uint64_t,16> registers{};
    std::uint64_t conditionWord{};
    std::uint32_t simdControl{};
    std::vector<std::string> vectorRegisters;
    std::vector<std::string> trace, imports, modules;
    std::vector<std::string> threads;
    std::map<std::string,std::uint64_t> importCallCounts;
    std::string Json() const;
};

// Executes real guest instructions using the shared LSX CPU semantics. An
// unavailable system function is a reported boundary, never a successful stub.
GuestRunResult RunGuestEntry(GuestImage image, std::uint64_t instructionLimit = 1000000,const GuestRunControl* control = nullptr);
GuestRunResult RunGuestTitle(const std::filesystem::path& folder,
                            std::uint64_t instructionLimit = TitleInstructionLimit,bool native = false,GuestFrameSink frameSink = {},std::uint64_t frameLimit = 1,bool asynchronousAudio = false,bool validateNative = true,const GuestRunControl* control = nullptr);
}
