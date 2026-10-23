// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/ngs2/ngs2_runtime.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include <array>
#include <cmath>
#include <limits>

namespace AppleNgsVoiceFixture {
static void Run() {
    using namespace Libraries::Ngs2;
    OrbisNgs2Handle system{}, rack{}, voice{};
    Require(Runtime::CreateSystem(nullptr, nullptr, &system) == 0,
            "NGS fixture system creation failed");
    struct Cleanup {
        OrbisNgs2Handle system;
        ~Cleanup() { Runtime::DestroySystem(system, nullptr); }
    } cleanup{system};
    Require(Runtime::CreateRack(system, 0, nullptr, nullptr, &rack) == 0 &&
            Runtime::GetVoiceHandle(rack, 0, &voice) == 0,
            "NGS fixture voice creation failed");
    OrbisNgs2CustomSamplerVoiceSetupParam setup{};
    setup.header = {sizeof(setup), 0, 0x40010000};
    setup.format = {0x12, 1, 48000, 0, 0, 0};
    Require(Runtime::ControlVoice(voice, &setup.header) == 0, "NGS PCM setup failed");
    std::array<s16, 32> pcm;
    pcm.fill(8192);
    OrbisNgs2WaveformBlock block{};
    block.dataSize = sizeof(pcm);
    block.numRepeats = std::numeric_limits<u32>::max();
    OrbisNgs2CustomSamplerVoiceWaveformBlocksParam blocks{};
    blocks.header = {sizeof(blocks), 0, 0x40010001};
    blocks.data = pcm.data();
    blocks.numBlocks = 1;
    blocks.aBlock = &block;
    auto bind = [&] { Require(Runtime::ControlVoice(voice, &blocks.header) == 0,
                            "NGS waveform publication failed"); };
    bind();
    auto event = [&](u32 id) {
        OrbisNgs2VoiceEventParam param{{sizeof(param), 0, 6}, id};
        Require(Runtime::ControlVoice(voice, &param.header) == 0, "NGS event failed");
    };
    auto flags = [&] {
        u32 value{};
        Require(Runtime::GetVoiceStateFlags(voice, &value) == 0, "NGS state query failed");
        return value;
    };
    auto render = [&](bool audible) {
        std::array<float, 16> output;
        output.fill(-99.0f);
        OrbisNgs2RenderBufferInfo buffer{output.data(), sizeof(output), 0x18, 1};
        Require(Runtime::Render(system, &buffer, 1) == 0, "NGS PCM render failed");
        for (float value : output)
            Require(std::abs(value - (audible ? 0.25f : 0.0f)) < 0.0001f,
                    "NGS render read an inactive voice or lost active PCM");
    };
    event(5); // Resume cannot create a playback lifetime.
    Require(flags() == 0, "NGS Resume started a sampler that was never played");
    render(false);
    event(0); render(true);
    event(4); Require(flags() == 3, "NGS Pause did not retain its playback lifetime");
    render(false);
    event(5); Require(flags() == 1, "NGS Resume failed on paused voice");
    render(true);
    for (u32 stop : {1u, 2u, 3u}) {
        event(stop);
        Require(flags() == 0, "NGS stop left the voice active");
        event(5);
        Require(flags() == 0, "NGS Resume revived a stopped sampler");
        render(false);
        bind(); event(0); render(true); // Explicit Play starts a new lifetime.
    }
    std::fprintf(stderr, "[APPLE_NGS_VOICE_TEST] initialResumeSilent=1 pauseResumePcm=1 stopResumeSilent=1 stopImmediateResumeSilent=1 killResumeSilent=1 explicitPlayPcm=1\n");
}
}
