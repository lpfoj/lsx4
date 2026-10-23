// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace Core::Loader { class SymbolsResolver; }
namespace Libraries::Videodec {
// Guest ABI follows shadPS4's libSceVideodec interface (not Videodec2).
struct Config {
    u64 thisSize;
    u32 codecType, profile, maxLevel;
    s32 maxFrameWidth, maxFrameHeight, maxDpbFrameCount;
    u64 videodecFlags;
};
struct Resource {
    u64 thisSize, cpuMemorySize;
    void* pCpuMemory;
    u64 cpuGpuMemorySize;
    void* pCpuGpuMemory;
    u64 maxFrameBufferSize;
    u32 frameBufferAlignment;
};
struct Control { u64 thisSize; void* handle; u64 version; };
struct FrameBuffer { u64 thisSize; void* pFrameBuffer; u64 frameBufferSize; };
struct AvcInfo {
    u32 numUnitsInTick, timeScale;
    u8 fixedFrameRateFlag, aspectRatioIdc;
    u16 sarWidth, sarHeight;
    u8 colourPrimaries, transferCharacteristics, matrixCoefficients, videoFullRangeFlag;
    u32 frameCropLeftOffset, frameCropRightOffset, frameCropTopOffset, frameCropBottomOffset;
};
struct Picture {
    u64 thisSize;
    u32 isValid, codecType, frameWidth, framePitch, frameHeight, isErrorPic;
    u64 ptsData, attachedData;
    union { u8 reserved[64]; AvcInfo avc; } codec;
};
struct Input { u64 thisSize; void* pAuData; u64 auSize, ptsData, dtsData, attachedData; };
static_assert(sizeof(Config) == 40 && sizeof(Resource) == 56);
static_assert(sizeof(Control) == 24 && sizeof(FrameBuffer) == 24);
static_assert(sizeof(Picture) == 112 && sizeof(Input) == 48);
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
