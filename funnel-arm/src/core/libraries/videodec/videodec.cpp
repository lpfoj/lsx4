// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later
// ABI/reference: shadPS4 core/libraries/videodec. Android uses the existing
// FFmpeg dependency; no guest binary patches or synthetic successful frames.
#include "core/libraries/videodec/videodec.h"
#if !defined(EXECUTOR_ANDROID_NATIVE_CORE_PROBE_NO_DESKTOP_MEDIA_USB) || \
    defined(EXECUTOR_ANDROID_NATIVE_CORE_PROBE_ENABLE_AVPLAYER)
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "video_core/page_manager.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace Libraries::Videodec {
namespace {
constexpr s32 Error(u32 n) { return static_cast<s32>(0x80c10000u + n); }
struct Layout { u32 width, height, pitch; u64 bytes; };
static bool GetLayout(s32 width, s32 height, Layout& out) {
    // The implementation supports 8-bit AVC within the PS4 decode envelope.
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    const u32 pitch = (u32(width) + 63) & ~63u;
    const u32 rows = (u32(height) + 15) & ~15u;
    out = {(u32(width) + 15) & ~15u, rows, pitch, u64(pitch) * rows * 3 / 2};
    return true;
}
static s32 Query(const Config* cfg, Resource* resource) {
    if (!cfg || !resource) return Error(15);
    if (cfg->thisSize != sizeof(Config) || resource->thisSize != sizeof(Resource)) return Error(2);
    if (cfg->codecType != 0) return Error(1);
    Layout layout{};
    if (!GetLayout(cfg->maxFrameWidth, cfg->maxFrameHeight, layout) ||
        cfg->maxDpbFrameCount < 0 || cfg->maxDpbFrameCount > 16) return Error(14);
    const u64 dpb = cfg->maxDpbFrameCount ? cfg->maxDpbFrameCount : 8;
    const u64 worst_pitch = (u64(cfg->maxFrameWidth) + 255) & ~u64(255);
    const u64 frame = ((worst_pitch * layout.height * 3 / 2 + 255) & ~u64(255)) + 0x4000;
    *resource = {sizeof(Resource), 16ULL << 20, nullptr,
                 frame * (dpb + 2) + (8ULL << 20), nullptr, frame, 256};
    return 0;
}
struct Metadata { u64 pts, attached; };
struct Decoder {
    std::mutex mutex;
    bool closed = false, draining = false;
    AVCodecContext* codec = nullptr;
    SwsContext* scaler = nullptr;
    std::deque<AVFrame*> ready;
    std::vector<u8> pixels;
    u64 output_count = 0;
    ~Decoder() {
        for (auto* frame : ready) av_frame_free(&frame);
        avcodec_free_context(&codec);
        sws_freeContext(scaler);
    }
    s32 Receive() {
        // A bounded queue preserves every delayed/B frame across Decode/Flush.
        while (ready.size() < 64) {
            auto* frame = av_frame_alloc();
            if (!frame) return Error(0);
            const int rc = avcodec_receive_frame(codec, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
                av_frame_free(&frame);
                return 0;
            }
            if (rc < 0) { av_frame_free(&frame); return Error(17); }
            ready.push_back(frame);
        }
        return Error(0);
    }
    s32 Output(FrameBuffer& buffer, Picture& picture) {
        if (ready.empty()) return 0;
        const auto* frame = ready.front();
        Layout layout{};
        if (!GetLayout(frame->width, frame->height, layout)) return Error(18);
        if (buffer.frameBufferSize < layout.bytes) return Error(11);
        const auto address = reinterpret_cast<VAddr>(buffer.pFrameBuffer);
        if (!Core::Memory::Instance()->IsValidMapping(address, layout.bytes)) return Error(12);
        scaler = sws_getCachedContext(scaler, frame->width, frame->height,
            AVPixelFormat(frame->format), frame->width, frame->height, AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) return Error(0);
        pixels.assign(layout.bytes, 0);
        u8* planes[4]{pixels.data(), pixels.data() + u64(layout.pitch) * layout.height};
        int strides[4]{int(layout.pitch), int(layout.pitch)};
        if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides)
            != frame->height) return Error(17);
        // Same ownership lease as AvPlayer. No writes become visible while an
        // old GPU owner or a descriptor-snapshot reader still owns the pages.
        VideoCore::PageManager::PrepareBulkGuestWrite(address, layout.bytes);
        VideoCore::PageManager::EnterBulkGuestWrite(address, layout.bytes);
        auto* state = VideoCore::ResolveBulkGuestWriteThreadState();
        VideoCore::BeginBulkGuestWrite(state, address, layout.bytes);
        {
            SCOPE_EXIT {
                VideoCore::PageManager::LeaveBulkGuestWrite(address, layout.bytes);
                VideoCore::EndBulkGuestWrite(state);
            };
            std::memcpy(buffer.pFrameBuffer, pixels.data(), layout.bytes);
        }
        Core::Memory::Instance()->InvalidateMemory(address, layout.bytes);
        picture = {};
        picture.thisSize = sizeof(Picture);
        picture.isValid = 1;
        picture.frameWidth = layout.width;
        picture.framePitch = layout.pitch;
        picture.frameHeight = layout.height;
        picture.ptsData = static_cast<u64>(frame->pts);
        if (frame->opaque_ref && frame->opaque_ref->size == sizeof(Metadata)) {
            Metadata metadata{};
            std::memcpy(&metadata, frame->opaque_ref->data, sizeof(metadata));
            picture.ptsData = metadata.pts;
            picture.attachedData = metadata.attached;
        }
        picture.codec.avc.colourPrimaries = u8(frame->color_primaries);
        picture.codec.avc.transferCharacteristics = u8(frame->color_trc);
        picture.codec.avc.matrixCoefficients = u8(frame->colorspace);
        picture.codec.avc.videoFullRangeFlag = frame->color_range == AVCOL_RANGE_JPEG;
        picture.codec.avc.frameCropLeftOffset = frame->crop_left;
        picture.codec.avc.frameCropRightOffset = frame->crop_right + layout.pitch - frame->width;
        picture.codec.avc.frameCropTopOffset = frame->crop_top;
        picture.codec.avc.frameCropBottomOffset = frame->crop_bottom + layout.height - frame->height;
        if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0 &&
            frame->sample_aspect_ratio.num <= 65535 && frame->sample_aspect_ratio.den <= 65535) {
            picture.codec.avc.aspectRatioIdc = 255;
            picture.codec.avc.sarWidth = frame->sample_aspect_ratio.num;
            picture.codec.avc.sarHeight = frame->sample_aspect_ratio.den;
        }
        auto* consumed = ready.front(); ready.pop_front(); av_frame_free(&consumed);
        ++output_count;
        if (output_count == 1 || output_count % 120 == 0)
            LOG_INFO(Lib_AvPlayer, "Decoded frames={} size={}x{} pitch={} pts={}",
                     output_count, picture.frameWidth, picture.frameHeight,
                     picture.framePitch, picture.ptsData);
        return 0;
    }
};
std::mutex registry_mutex;
std::unordered_map<u64, std::shared_ptr<Decoder>> decoders;
u64 next_handle = 1;
static std::shared_ptr<Decoder> Lookup(const Control* control) {
    if (!control || control->thisSize != sizeof(Control)) return {};
    std::scoped_lock lock(registry_mutex);
    const auto found = decoders.find(reinterpret_cast<u64>(control->handle));
    return found == decoders.end() ? nullptr : found->second;
}
static s32 ValidateOutput(FrameBuffer* buffer, Picture* picture) {
    if (!buffer || !picture) return Error(15);
    if (buffer->thisSize != sizeof(FrameBuffer) || picture->thisSize != sizeof(Picture)) return Error(2);
    picture->isValid = 0;
    if (!buffer->pFrameBuffer) return Error(12);
    if ((reinterpret_cast<u64>(buffer->pFrameBuffer) & 255) != 0) return Error(13);
    return 0;
}
}
s32 PS4_SYSV_ABI sceVideodecQueryResourceInfo(const Config* cfg, Resource* resource) {
    const auto rc = Query(cfg, resource);
    LOG_INFO(Lib_AvPlayer, "QueryResourceInfo result={} alignment={} framebuffer={}",
             rc, rc ? 0 : resource->frameBufferAlignment, rc ? 0 : resource->maxFrameBufferSize);
    return rc;
}
s32 PS4_SYSV_ABI sceVideodecCreateDecoder(const Config* cfg, const Resource* resource, Control* control) {
    if (!cfg || !resource || !control) return Error(15);
    if (control->thisSize != sizeof(Control) || resource->thisSize != sizeof(Resource)) return Error(2);
    Resource required{sizeof(Resource)};
    if (auto rc = Query(cfg, &required)) return rc;
    auto decoder = std::make_shared<Decoder>();
    const auto* implementation = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!implementation) return Error(0);
    decoder->codec = avcodec_alloc_context3(implementation);
    if (!decoder->codec) return Error(0);
    decoder->codec->width = cfg->maxFrameWidth;
    decoder->codec->height = cfg->maxFrameHeight;
    decoder->codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    // Slice threading avoids extra frame latency and an unbounded CPU cohort.
    decoder->codec->thread_count = 2;
    decoder->codec->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(decoder->codec, implementation, nullptr) < 0) return Error(0);
    std::scoped_lock lock(registry_mutex);
    if (!next_handle) return Error(0);
    const u64 handle = next_handle++;
    decoders.emplace(handle, decoder);
    *control = {sizeof(Control), reinterpret_cast<void*>(handle), 1};
    LOG_INFO(Lib_AvPlayer, "Created AVC decoder handle={} size={}x{}", handle,
             cfg->maxFrameWidth, cfg->maxFrameHeight);
    return 0;
}
s32 PS4_SYSV_ABI sceVideodecDecode(Control* control, const Input* input,
                                  FrameBuffer* buffer, Picture* picture) {
    if (auto rc = ValidateOutput(buffer, picture)) return rc;
    if (!input) return Error(15);
    if (input->thisSize != sizeof(Input)) return Error(2);
    if (!input->pAuData) return Error(10);
    if (!input->auSize || input->auSize > (16ULL << 20)) return Error(9);
    auto decoder = Lookup(control);
    if (!decoder) return Error(3);
    std::scoped_lock lock(decoder->mutex);
    if (decoder->closed) return Error(3);
    if (decoder->draining) return Error(21); // Reset is required after a drained sequence.
    if (decoder->ready.size() >= 63) return Error(0);
    AVPacket* packet = av_packet_alloc();
    if (!packet) return Error(0);
    SCOPE_EXIT { av_packet_free(&packet); };
    // FFmpeg owns padded input: the guest is free to reuse its AU after return.
    if (av_new_packet(packet, int(input->auSize)) < 0) return Error(0);
    std::memcpy(packet->data, input->pAuData, input->auSize);
    packet->pts = static_cast<s64>(input->ptsData);
    packet->dts = static_cast<s64>(input->dtsData);
    packet->opaque_ref = av_buffer_alloc(sizeof(Metadata));
    if (!packet->opaque_ref) return Error(0);
    const Metadata metadata{input->ptsData, input->attachedData};
    std::memcpy(packet->opaque_ref->data, &metadata, sizeof(metadata));
    int rc = avcodec_send_packet(decoder->codec, packet);
    if (rc == AVERROR(EAGAIN)) {
        if (auto result = decoder->Receive()) return result;
        rc = avcodec_send_packet(decoder->codec, packet);
    }
    if (rc < 0) { LOG_ERROR(Lib_AvPlayer, "Decode submit failed {}", rc); return Error(17); }
    if (auto result = decoder->Receive()) return result;
    return decoder->Output(*buffer, *picture);
}
s32 PS4_SYSV_ABI sceVideodecFlush(Control* control, FrameBuffer* buffer, Picture* picture) {
    if (auto rc = ValidateOutput(buffer, picture)) return rc;
    auto decoder = Lookup(control);
    if (!decoder) return Error(3);
    std::scoped_lock lock(decoder->mutex);
    if (decoder->closed) return Error(3);
    if (!decoder->draining) {
        int rc = avcodec_send_packet(decoder->codec, nullptr);
        if (rc == AVERROR(EAGAIN)) {
            if (auto result = decoder->Receive()) return result;
            rc = avcodec_send_packet(decoder->codec, nullptr);
        }
        if (rc < 0 && rc != AVERROR_EOF) return Error(17);
        decoder->draining = true;
    }
    if (auto rc = decoder->Receive()) return rc;
    return decoder->Output(*buffer, *picture);
}
s32 PS4_SYSV_ABI sceVideodecReset(Control* control) {
    auto decoder = Lookup(control);
    if (!decoder) return Error(3);
    std::scoped_lock lock(decoder->mutex);
    if (decoder->closed) return Error(3);
    avcodec_flush_buffers(decoder->codec);
    for (auto* frame : decoder->ready) av_frame_free(&frame);
    decoder->ready.clear(); decoder->draining = false;
    return 0;
}
s32 PS4_SYSV_ABI sceVideodecDeleteDecoder(Control* control) {
    auto decoder = Lookup(control);
    if (!decoder) return Error(3);
    std::scoped_lock lock(decoder->mutex);
    if (decoder->closed) return Error(3);
    decoder->closed = true;
    { std::scoped_lock registry_lock(registry_mutex);
      decoders.erase(reinterpret_cast<u64>(control->handle)); }
    control->handle = nullptr;
    return 0;
}
void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("leCAscipfFY", "libSceVideodec", 1, "libSceVideodec", sceVideodecQueryResourceInfo);
    LIB_FUNCTION("qkgRiwHyheU", "libSceVideodec", 1, "libSceVideodec", sceVideodecCreateDecoder);
    LIB_FUNCTION("q0W5GJMovMs", "libSceVideodec", 1, "libSceVideodec", sceVideodecDecode);
    LIB_FUNCTION("jeigLlKdp5I", "libSceVideodec", 1, "libSceVideodec", sceVideodecFlush);
    LIB_FUNCTION("f8AgDv-1X8A", "libSceVideodec", 1, "libSceVideodec", sceVideodecReset);
    LIB_FUNCTION("U0kpGF1cl90", "libSceVideodec", 1, "libSceVideodec", sceVideodecDeleteDecoder);
}
}
#endif
