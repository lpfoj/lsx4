// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>

#include "common/config.h"
#include "common/logging/log.h"
#ifdef __ANDROID__
#include "common/path_util.h"
#endif
#include "core/libraries/audio/audio_mix.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/kernel/threads.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HAS_SSE2
#endif

#define SDL_INVALID_AUDIODEVICEID 0

namespace Libraries::AudioOut {

namespace {

u64 ElapsedNanoseconds(const std::chrono::steady_clock::time_point start) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

} // namespace

constexpr float VOLUME_0DB = 32768.0f;
constexpr float INV_VOLUME_0DB = 1.0f / VOLUME_0DB;
constexpr float VOLUME_EPSILON = 0.001f;
constexpr u64 VOLUME_CHECK_INTERVAL_US = 50000;
constexpr u64 MIN_SLEEP_THRESHOLD_US = 10;
constexpr u64 TIMING_RESYNC_THRESHOLD_US = 100000;

constexpr u32 QUEUE_MULTIPLIER = 4;
constexpr size_t AUDIO_BUFFER_ALIGNMENT = 32;

#ifdef __ANDROID__
static_assert(std::atomic<u64>::is_always_lock_free,
              "AAudio callback counters must stay lock-free on the RT thread");
static_assert(std::atomic<AAudioStream*>::is_always_lock_free,
              "AAudio callback stream publication must stay lock-free on the RT thread");
static_assert(std::atomic<aaudio_result_t>::is_always_lock_free,
              "AAudio callback errors must stay lock-free on the RT thread");
static_assert(std::atomic<bool>::is_always_lock_free,
              "AAudio callback flags must stay lock-free on the RT thread");

static void AndroidAudioLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "LSX4Native", format, args);
    va_end(args);
}

static bool AndroidSdlAudioRequested() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.lsx4.audio_backend", value) > 0 &&
        std::strcmp(value, "sdl") == 0) {
        return true;
    }
    const char* force_sdl = std::getenv("EXECUTOR_FORCE_SDL_AUDIO");
    return force_sdl != nullptr && std::strcmp(force_sdl, "1") == 0;
}

static bool AndroidAudioCallbackTransportRequested() {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get("debug.lsx4.audio_transport", value) > 0 &&
           std::strcmp(value, "callback") == 0;
}

static bool AndroidAudioConsumedCreditRequested() {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get("debug.lsx4.audio_credit", value) > 0 &&
           std::strcmp(value, "consume") == 0;
}

static bool AndroidAudioOutputDisabledRequested() {
    const char* value = std::getenv("EXECUTOR_DISABLE_AUDIO_OUTPUT");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

static constexpr char AndroidRawPcmCaptureProperty[] = "debug.lsx4.audio_raw_capture";
static constexpr char AndroidRawPcmCaptureFile[] =
    "diag-audio-raw-preconvert-f32.wav";
static std::atomic_flag AndroidRawPcmCaptureOwner = ATOMIC_FLAG_INIT;

static bool AndroidRawPcmCaptureRequested() {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get(AndroidRawPcmCaptureProperty, value) > 0 &&
           value[0] != '\0' && std::strcmp(value, "0") != 0;
}

class AndroidRawPcmCapture final {
public:
    static constexpr u32 CaptureSeconds = 12;
    static constexpr u64 MaxCaptureBytes = UINT64_C(64) * 1024 * 1024;

    static bool Supports(const u32 sample_rate, const u32 channels, const u32 sample_size,
                         const bool source_is_float) noexcept {
        if (sample_rate < 8000 || sample_rate > 192000 ||
            (channels != 2 && channels != 8) ||
            sample_size != (source_is_float ? sizeof(float) : sizeof(s16))) {
            return false;
        }
        const u64 output_bytes = static_cast<u64>(sample_rate) * CaptureSeconds * channels *
                                 sizeof(float);
        return output_bytes <= MaxCaptureBytes;
    }

    AndroidRawPcmCapture(const u32 sample_rate_, const u32 channels_,
                         const bool source_is_float_, const bool standard_8ch_)
        : sample_rate(sample_rate_), channels(channels_), source_is_float(source_is_float_),
          standard_8ch(standard_8ch_),
          target_frames(static_cast<u64>(sample_rate) * CaptureSeconds),
          output_path((Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                       AndroidRawPcmCaptureFile)
                          .string()),
          samples(static_cast<size_t>(target_frames) * channels),
          writer_thread([this] {
              writer_signal.acquire();
              WriteCapture();
          }) {
        (void)std::remove(output_path.c_str());
        AndroidAudioLog(
            "[EXECUTOR_AUDIO_RAW_CAPTURE] state=armed property=%s path=%s seconds=%u "
            "sampleRate=%u channels=%u source=%s standard8=%u capacityBytes=%llu",
            AndroidRawPcmCaptureProperty, output_path.c_str(), CaptureSeconds, sample_rate,
            channels, source_is_float ? "float32" : "s16", standard_8ch ? 1u : 0u,
            static_cast<unsigned long long>(samples.size() * sizeof(float)));
    }

    ~AndroidRawPcmCapture() {
        Finish();
        if (writer_thread.joinable()) {
            writer_thread.join();
        }
        AndroidRawPcmCaptureOwner.clear(std::memory_order_release);
    }

    void AppendGuest(const void* source, const u32 frames) noexcept {
        if (!source || !accepting || frames == 0) {
            return;
        }
        const u64 frames_to_copy = std::min<u64>(frames, target_frames - captured_frames);
        const size_t sample_count = static_cast<size_t>(frames_to_copy) * channels;
        if (!capture_started && !HasSignal(source, sample_count)) {
            return;
        }
        MarkStarted();
        float* destination = samples.data() + static_cast<size_t>(captured_frames) * channels;
        if (source_is_float) {
            std::memcpy(destination, source, sample_count * sizeof(float));
        } else {
            const auto* source_s16 = static_cast<const s16*>(source);
            for (size_t sample = 0; sample < sample_count; ++sample) {
                destination[sample] = static_cast<float>(source_s16[sample]) * INV_VOLUME_0DB;
            }
        }
        CommitFrames(frames_to_copy);
    }

    void AppendFloat(const float* source, const u32 frames, const u32 source_channels) noexcept {
        if (!source || !accepting || frames == 0 || source_channels != channels) {
            return;
        }
        const u64 frames_to_copy = std::min<u64>(frames, target_frames - captured_frames);
        const size_t sample_count = static_cast<size_t>(frames_to_copy) * channels;
        if (!capture_started && !HasFloatSignal(source, sample_count)) {
            return;
        }
        MarkStarted();
        float* destination = samples.data() + static_cast<size_t>(captured_frames) * channels;
        std::memcpy(destination, source, sample_count * sizeof(float));
        CommitFrames(frames_to_copy);
    }

private:
    [[nodiscard]] static bool HasFloatSignal(const float* source,
                                             const size_t sample_count) noexcept {
        for (size_t sample = 0; sample < sample_count; ++sample) {
            if (source[sample] != 0.0f) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool HasSignal(const void* source, const size_t sample_count) const noexcept {
        if (source_is_float) {
            return HasFloatSignal(static_cast<const float*>(source), sample_count);
        }
        const auto* source_s16 = static_cast<const s16*>(source);
        for (size_t sample = 0; sample < sample_count; ++sample) {
            if (source_s16[sample] != 0) {
                return true;
            }
        }
        return false;
    }

    void MarkStarted() noexcept {
        if (capture_started) {
            return;
        }
        capture_started = true;
        AndroidAudioLog(
            "[EXECUTOR_AUDIO_RAW_CAPTURE] state=started sampleRate=%u channels=%u source=%s",
            sample_rate, channels, source_is_float ? "float32" : "s16");
    }

    static void StoreU16(std::array<u8, 44>& header, const size_t offset, const u16 value) {
        header[offset] = static_cast<u8>(value);
        header[offset + 1] = static_cast<u8>(value >> 8);
    }

    static void StoreU32(std::array<u8, 44>& header, const size_t offset, const u32 value) {
        header[offset] = static_cast<u8>(value);
        header[offset + 1] = static_cast<u8>(value >> 8);
        header[offset + 2] = static_cast<u8>(value >> 16);
        header[offset + 3] = static_cast<u8>(value >> 24);
    }

    void CommitFrames(const u64 frames) noexcept {
        captured_frames += frames;
        if (captured_frames == target_frames) {
            SignalWriter();
        }
    }

    void SignalWriter() noexcept {
        accepting = false;
        frames_ready = captured_frames;
        if (!write_signaled.exchange(true, std::memory_order_acq_rel)) {
            writer_signal.release();
        }
    }

    void Finish() noexcept {
        if (!write_signaled.load(std::memory_order_acquire)) {
            SignalWriter();
        }
    }

    void WriteCapture() noexcept {
        const u64 output_bytes_64 = frames_ready * channels * sizeof(float);
        const u32 output_bytes = static_cast<u32>(output_bytes_64);
        std::array<u8, 44> header{};
        std::memcpy(header.data(), "RIFF", 4);
        StoreU32(header, 4, 36 + output_bytes);
        std::memcpy(header.data() + 8, "WAVEfmt ", 8);
        StoreU32(header, 16, 16);
        StoreU16(header, 20, 3); // WAVE_FORMAT_IEEE_FLOAT
        StoreU16(header, 22, static_cast<u16>(channels));
        StoreU32(header, 24, sample_rate);
        StoreU32(header, 28, sample_rate * channels * sizeof(float));
        StoreU16(header, 32, static_cast<u16>(channels * sizeof(float)));
        StoreU16(header, 34, 32);
        std::memcpy(header.data() + 36, "data", 4);
        StoreU32(header, 40, output_bytes);

        std::FILE* file = std::fopen(output_path.c_str(), "wb");
        bool success = file != nullptr;
        if (success) {
            success = std::fwrite(header.data(), 1, header.size(), file) == header.size();
        }
        if (success && output_bytes != 0) {
            success = std::fwrite(samples.data(), 1, output_bytes, file) == output_bytes;
        }
        if (file) {
            success = std::fclose(file) == 0 && success;
        }
        AndroidAudioLog(
            "[EXECUTOR_AUDIO_RAW_CAPTURE] state=%s path=%s frames=%llu seconds=%.3f "
            "sampleRate=%u channels=%u format=float32-le standard8=%u bytes=%u",
            success ? "complete" : "write-failed", output_path.c_str(),
            static_cast<unsigned long long>(frames_ready),
            sample_rate == 0 ? 0.0 : static_cast<double>(frames_ready) / sample_rate,
            sample_rate, channels, standard_8ch ? 1u : 0u, output_bytes);
    }

    const u32 sample_rate;
    const u32 channels;
    const bool source_is_float;
    const bool standard_8ch;
    const u64 target_frames;
    const std::string output_path;
    std::vector<float> samples;
    u64 captured_frames{};
    u64 frames_ready{};
    bool accepting{true};
    bool capture_started{};
    std::atomic<bool> write_signaled{};
    std::binary_semaphore writer_signal{0};
    std::jthread writer_thread;
};

// Keep short-lived PCM diagnostics visible even when the normal live runner
// suppresses informational logcat traffic. The probe is read-only and bounded.
static void AndroidAudioProbeWarn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_WARN, "LSX4AudioProbe", format, args);
    va_end(args);
}

enum class AndroidAudioProbeMode : u8 {
    Disabled,
    All,
    FirstS16Stereo48k1024,
};

static AndroidAudioProbeMode AndroidAudioProbeModeRequested() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.lsx4.audio_probe", value) <= 0) {
        return AndroidAudioProbeMode::Disabled;
    }
    if (value[0] == '1') {
        return AndroidAudioProbeMode::All;
    }
    if (std::strcmp(value, "s16-2ch-48k-1024") == 0) {
        return AndroidAudioProbeMode::FirstS16Stereo48k1024;
    }
    return AndroidAudioProbeMode::Disabled;
}

static u64 AndroidAudioProbeHash(const void* data, const size_t size) {
    constexpr u64 Offset = UINT64_C(1469598103934665603);
    constexpr u64 Prime = UINT64_C(1099511628211);
    const auto* bytes = static_cast<const u8*>(data);
    u64 hash = Offset;
    size_t offset{};
    for (; offset + sizeof(u64) <= size; offset += sizeof(u64)) {
        u64 word{};
        std::memcpy(&word, bytes + offset, sizeof(word));
        hash ^= word;
        hash *= Prime;
    }
    for (; offset < size; ++offset) {
        hash ^= bytes[offset];
        hash *= Prime;
    }
    return hash;
}

struct AudioPcmProbeLaneStats {
    double sum{};
    double sum_squares{};
    float peak{};
    u32 finite_samples{};
    u32 non_finite_samples{};
    u32 clipped_samples{};
    u32 zero_samples{};
    u32 subnormal_samples{};

    void Observe(const float sample) {
        if (!std::isfinite(sample)) {
            ++non_finite_samples;
            return;
        }
        ++finite_samples;
        if (sample == 0.0f) {
            ++zero_samples;
        }
        if (std::fpclassify(sample) == FP_SUBNORMAL) {
            ++subnormal_samples;
        }
        const float magnitude = std::abs(sample);
        peak = std::max(peak, magnitude);
        clipped_samples += magnitude > 1.0f ? 1u : 0u;
        sum += sample;
        sum_squares += static_cast<double>(sample) * sample;
    }

    [[nodiscard]] double Rms() const {
        return finite_samples == 0
                   ? 0.0
                   : std::sqrt(sum_squares / static_cast<double>(finite_samples));
    }

    [[nodiscard]] double Dc() const {
        return finite_samples == 0 ? 0.0 : sum / static_cast<double>(finite_samples);
    }
};
#endif

enum ChannelPos : u8 {
    FL = 0,
    FR = 1,
    FC = 2,
    LF = 3,
    SL = 4,
    SR = 5,
    BL = 6,
    BR = 7,
    STD_SL = 6,
    STD_SR = 7,
    STD_BL = 4,
    STD_BR = 5
};

class SDLPortBackend : public PortBackend {
public:
    explicit SDLPortBackend(const PortOut& port)
        : frame_size(port.format_info.FrameSize()), guest_buffer_size(port.BufferSize()),
          buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          num_channels(port.format_info.num_channels), is_float(port.format_info.is_float),
          is_std(port.format_info.is_std), channel_layout(port.format_info.channel_layout) {

        if (!Initialize(port.type)) {
            LOG_ERROR(Lib_AudioOut, "Failed to initialize SDL audio backend");
        }
    }

    ~SDLPortBackend() override {
        Cleanup();
    }

    void Output(void* ptr) override {
        if (!stream || !internal_buffer) [[unlikely]] {
            return;
        }

        if (ptr == nullptr) [[unlikely]] {
            return;
        }

        const AudioBufferFormat format{
            .sample_type = is_float ? AudioSampleType::Float32 : AudioSampleType::S16,
            .channels = static_cast<u8>(num_channels),
            // SDL's input channel map consumes the guest's physical Std order
            // directly. Semantic normalization is only needed before software
            // downmixing/mixing, not on this native eight-channel route.
            .standard_8ch = false,
        };
        (void)ConvertAudioBufferToFloat(
            ptr, buffer_frames, format, num_channels, 1.0f,
            std::span<float>{static_cast<float*>(internal_buffer),
                             static_cast<size_t>(buffer_frames) * num_channels},
            conversion_guard);
        SubmitFloat(static_cast<const float*>(internal_buffer));
    }

    void OutputFloat(const float* ptr, const u32 frames, const u32 channels) override {
        if (!stream || !ptr) [[unlikely]] {
            return;
        }
        if (frames != buffer_frames || channels != num_channels) [[unlikely]] {
            LOG_ERROR(Lib_AudioOut,
                      "Invalid normalized SDL audio buffer: frames={}/{} channels={}/{}", frames,
                      buffer_frames, channels, num_channels);
            return;
        }
        if (is_std && channels == 8) {
            // The shared mixer exposes canonical FL/FR/FC/LFE/SL/SR/BL/BR.
            // Restore the Orbis Std physical order expected by the SDL input
            // map: FL/FR/FC/LFE/BL/BR/SL/SR.
            auto* physical = static_cast<float*>(internal_buffer);
            for (u32 frame = 0; frame < frames; ++frame) {
                const size_t offset = static_cast<size_t>(frame) * 8;
                physical[offset + 0] = ptr[offset + 0];
                physical[offset + 1] = ptr[offset + 1];
                physical[offset + 2] = ptr[offset + 2];
                physical[offset + 3] = ptr[offset + 3];
                physical[offset + 4] = ptr[offset + 6];
                physical[offset + 5] = ptr[offset + 7];
                physical[offset + 6] = ptr[offset + 4];
                physical[offset + 7] = ptr[offset + 5];
            }
            SubmitFloat(physical);
            return;
        }
        SubmitFloat(ptr);
    }

    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        if (!stream) [[unlikely]] {
            return;
        }

        float max_channel_gain = 0.0f;
        const u32 channels_to_check = std::min(num_channels, 8u);

        for (u32 i = 0; i < channels_to_check; i++) {
            const float channel_gain = static_cast<float>(ch_volumes[i]) * INV_VOLUME_0DB;
            max_channel_gain = std::max(max_channel_gain, channel_gain);
        }

        const float slider_gain = Config::getVolumeSlider() * 0.01f;
        const float total_gain = max_channel_gain * slider_gain;

        const float current = current_gain.load(std::memory_order_acquire);
        if (std::abs(total_gain - current) < VOLUME_EPSILON) {
            return;
        }

        if (SDL_SetAudioStreamGain(stream, total_gain)) {
            current_gain.store(total_gain, std::memory_order_release);
            LOG_DEBUG(Lib_AudioOut,
                      "Set combined audio gain to {:.3f} (channel: {:.3f}, slider: {:.3f})",
                      total_gain, max_channel_gain, slider_gain);
        } else {
            LOG_ERROR(Lib_AudioOut, "Failed to set audio stream gain: {}", SDL_GetError());
        }
    }

    u64 GetLastOutputTime() const {
        return last_output_time.load(std::memory_order_acquire);
    }

private:
    void SubmitFloat(const float* ptr) {
        UpdateVolumeIfChanged();
        const u64 current_time = Kernel::sceKernelGetProcessTime();
        HandleTiming(current_time);

        if ((output_count++ & 0xF) == 0) {
            ManageAudioQueue();
        }

        const auto write_started = std::chrono::steady_clock::now();
        const bool write_ok = SDL_PutAudioStreamData(stream, ptr, internal_buffer_size);
        RecordAudioBackendWrite(ElapsedNanoseconds(write_started),
                                write_ok ? static_cast<s64>(buffer_frames) : -1, buffer_frames, 0);
        if (!write_ok) [[unlikely]] {
            LOG_ERROR(Lib_AudioOut, "Failed to output to SDL audio stream: {}", SDL_GetError());
        }

        last_output_time.store(current_time, std::memory_order_release);
    }
    bool Initialize(OrbisAudioOutPort type) {
        period_us = (1000000ULL * buffer_frames + sample_rate / 2) / sample_rate;

        internal_buffer_size = buffer_frames * sizeof(float) * num_channels;

#ifdef _WIN32
        internal_buffer = _aligned_malloc(internal_buffer_size, AUDIO_BUFFER_ALIGNMENT);
#else
        if (posix_memalign(&internal_buffer, AUDIO_BUFFER_ALIGNMENT, internal_buffer_size) != 0) {
            internal_buffer = nullptr;
        }
#endif

        if (!internal_buffer) {
            LOG_ERROR(Lib_AudioOut, "Failed to allocate aligned audio buffer of size {}",
                      internal_buffer_size);
            return false;
        }

        current_gain.store(Config::getVolumeSlider() * 0.01f, std::memory_order_relaxed);

        if (!SelectConverter()) {
            FreeAlignedBuffer();
            return false;
        }

        if (!OpenDevice(type)) {
            FreeAlignedBuffer();
            return false;
        }

        CalculateQueueThreshold();
        return true;
    }

    void Cleanup() {
        if (stream) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        FreeAlignedBuffer();
    }

    void FreeAlignedBuffer() {
        if (internal_buffer) {
#ifdef _WIN32
            _aligned_free(internal_buffer);
#else
            free(internal_buffer);
#endif
            internal_buffer = nullptr;
        }
    }

    void UpdateVolumeIfChanged() {
        const u64 current_time = Kernel::sceKernelGetProcessTime();

        if (current_time - last_volume_check_time < VOLUME_CHECK_INTERVAL_US) {
            return;
        }

        last_volume_check_time = current_time;

        const float config_volume = Config::getVolumeSlider() * 0.01f;
        const float stored_gain = current_gain.load(std::memory_order_acquire);

        if (std::abs(config_volume - stored_gain) > VOLUME_EPSILON) {
            if (SDL_SetAudioStreamGain(stream, config_volume)) {
                current_gain.store(config_volume, std::memory_order_release);
                LOG_DEBUG(Lib_AudioOut, "Updated audio gain to {:.3f}", config_volume);
            } else {
                LOG_ERROR(Lib_AudioOut, "Failed to set audio stream gain: {}", SDL_GetError());
            }
        }
    }

    void HandleTiming(u64 current_time) {
        if (next_output_time == 0) [[unlikely]] {
            next_output_time = current_time + period_us;
            return;
        }

        const s64 time_diff = static_cast<s64>(current_time - next_output_time);

        if (time_diff > static_cast<s64>(TIMING_RESYNC_THRESHOLD_US)) [[unlikely]] {
            next_output_time = current_time + period_us;
        } else if (time_diff < 0) {
            const u64 time_to_wait = static_cast<u64>(-time_diff);
            next_output_time += period_us;

            if (time_to_wait > MIN_SLEEP_THRESHOLD_US) {
                const u64 sleep_duration = time_to_wait - MIN_SLEEP_THRESHOLD_US;
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_duration));
            }
        } else {
            next_output_time += period_us;
        }
    }

    void ManageAudioQueue() {
        const auto queued = SDL_GetAudioStreamQueued(stream);

        if (queued >= queue_threshold) [[unlikely]] {
            LOG_DEBUG(Lib_AudioOut, "Clearing backed up audio queue ({} >= {})", queued,
                      queue_threshold);
            SDL_ClearAudioStream(stream);
            CalculateQueueThreshold();
        }
    }

    bool OpenDevice(OrbisAudioOutPort type) {
        const SDL_AudioSpec fmt = {
            .format = SDL_AUDIO_F32LE,
            .channels = static_cast<u8>(num_channels),
            .freq = static_cast<int>(sample_rate),
        };

        const std::string device_name = GetDeviceName(type);
        const SDL_AudioDeviceID dev_id = SelectAudioDevice(device_name, type);

        if (dev_id == SDL_INVALID_AUDIODEVICEID) {
            return false;
        }

        stream = SDL_OpenAudioDeviceStream(dev_id, &fmt, nullptr, nullptr);
        if (!stream) {
            LOG_ERROR(Lib_AudioOut, "Failed to create SDL audio stream: {}", SDL_GetError());
            return false;
        }

        if (!ConfigureChannelMap()) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            return false;
        }

        const float initial_gain = current_gain.load(std::memory_order_relaxed);
        if (!SDL_SetAudioStreamGain(stream, initial_gain)) {
            LOG_WARNING(Lib_AudioOut, "Failed to set initial audio gain: {}", SDL_GetError());
        }

        if (!SDL_ResumeAudioStreamDevice(stream)) {
            LOG_ERROR(Lib_AudioOut, "Failed to resume audio stream: {}", SDL_GetError());
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            return false;
        }

        LOG_INFO(Lib_AudioOut, "Opened audio device: {} ({} Hz, {} ch, gain: {:.3f})", device_name,
                 sample_rate, num_channels, initial_gain);
        return true;
    }

    SDL_AudioDeviceID SelectAudioDevice(const std::string& device_name, OrbisAudioOutPort type) {
        if (device_name == "None") {
            LOG_INFO(Lib_AudioOut, "Audio device disabled for port type {}",
                     static_cast<int>(type));
            return SDL_INVALID_AUDIODEVICEID;
        }

        if (device_name.empty() || device_name == "Default Device") {
            return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        }

        int num_devices = 0;
        SDL_AudioDeviceID* dev_array = SDL_GetAudioPlaybackDevices(&num_devices);

        if (!dev_array) {
            LOG_WARNING(Lib_AudioOut, "No audio devices found, using default");
            return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        }

        SDL_AudioDeviceID selected_device = SDL_INVALID_AUDIODEVICEID;

        for (int i = 0; i < num_devices; i++) {
            const char* dev_name = SDL_GetAudioDeviceName(dev_array[i]);
            if (dev_name && device_name == dev_name) {
                selected_device = dev_array[i];
                break;
            }
        }

        SDL_free(dev_array);

        if (selected_device == SDL_INVALID_AUDIODEVICEID) {
            LOG_WARNING(Lib_AudioOut, "Audio device '{}' not found, using default", device_name);
            return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        }

        return selected_device;
    }

    bool ConfigureChannelMap() {
        if (num_channels == 0) {
            return true;
        }

        std::vector<int> channel_map(num_channels);

        if (is_std && num_channels == 8) {
            channel_map = {FL, FR, FC, LF, STD_SL, STD_SR, STD_BL, STD_BR};
        } else {
            std::copy_n(channel_layout.begin(), num_channels, channel_map.begin());
        }

        if (!SDL_SetAudioStreamInputChannelMap(stream, channel_map.data(), num_channels)) {
            LOG_ERROR(Lib_AudioOut, "Failed to set channel map: {}", SDL_GetError());
            return false;
        }

        return true;
    }

    std::string GetDeviceName(OrbisAudioOutPort type) const {
        switch (type) {
        case OrbisAudioOutPort::Main:
        case OrbisAudioOutPort::Bgm:
            return Config::getMainOutputDevice();
        case OrbisAudioOutPort::PadSpk:
            return Config::getPadSpkOutputDevice();
        default:
            return Config::getMainOutputDevice();
        }
    }

    bool SelectConverter() {
        if (is_float) {
            switch (num_channels) {
            case 1:
                convert = &ConvertF32Mono;
                break;
            case 2:
                convert = &ConvertF32Stereo;
                break;
            case 8:
                convert = is_std ? &ConvertF32Std8CH : &ConvertF32_8CH;
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported float channel count: {}", num_channels);
                return false;
            }
        } else {
            switch (num_channels) {
            case 1:
                convert = &ConvertS16Mono;
                break;
            case 2:
#if defined(HAS_SSE2)
                convert = &ConvertS16StereoSIMD;
#else
                convert = &ConvertS16Stereo;
#endif
                break;
            case 8:
#if defined(HAS_SSE2)
                convert = &ConvertS16_8CH_SIMD;
#else
                convert = &ConvertS16_8CH;
#endif
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported S16 channel count: {}", num_channels);
                return false;
            }
        }

        return true;
    }

    void CalculateQueueThreshold() {
        if (!stream) {
            return;
        }

        SDL_AudioSpec discard;
        int sdl_buffer_frames = 0;

        if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &discard,
                                      &sdl_buffer_frames)) {
            LOG_WARNING(Lib_AudioOut, "Failed to get SDL buffer size: {}", SDL_GetError());
        }

        const u32 sdl_buffer_size = sdl_buffer_frames * sizeof(float) * num_channels;
        queue_threshold = std::max(guest_buffer_size, sdl_buffer_size) * QUEUE_MULTIPLIER;

        LOG_DEBUG(Lib_AudioOut, "Audio queue threshold: {} bytes (SDL buffer: {} frames)",
                  queue_threshold, sdl_buffer_frames);
    }

    using ConverterFunc = void (*)(const void* src, void* dst, u32 frames, const float* volumes);

    static void ConvertS16Mono(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        for (u32 i = 0; i < frames; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }

    static void ConvertS16Stereo(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const u32 num_samples = frames << 1;
        for (u32 i = 0; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }

#ifdef HAS_SSE2
    static void ConvertS16StereoSIMD(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const __m128 scale = _mm_set1_ps(INV_VOLUME_0DB);
        const u32 num_samples = frames << 1;
        u32 i = 0;

        for (; i + 8 <= num_samples; i += 8) {
            __m128i s16_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&s[i]));

            __m128i s32_lo = _mm_cvtepi16_epi32(s16_vals);
            __m128i s32_hi = _mm_cvtepi16_epi32(_mm_srli_si128(s16_vals, 8));

            __m128 f_lo = _mm_mul_ps(_mm_cvtepi32_ps(s32_lo), scale);
            __m128 f_hi = _mm_mul_ps(_mm_cvtepi32_ps(s32_hi), scale);

            _mm_storeu_ps(&d[i], f_lo);
            _mm_storeu_ps(&d[i + 4], f_hi);
        }

        for (; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }
#endif

    static void ConvertS16_8CH(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const u32 num_samples = frames << 3;
        for (u32 i = 0; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }

#ifdef HAS_SSE2
    static void ConvertS16_8CH_SIMD(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const __m128 scale = _mm_set1_ps(INV_VOLUME_0DB);
        const u32 num_samples = frames << 3;
        u32 i = 0;

        for (; i + 8 <= num_samples; i += 8) {
            __m128i s16_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&s[i]));
            __m128i s32_lo = _mm_cvtepi16_epi32(s16_vals);
            __m128i s32_hi = _mm_cvtepi16_epi32(_mm_srli_si128(s16_vals, 8));
            __m128 f_lo = _mm_mul_ps(_mm_cvtepi32_ps(s32_lo), scale);
            __m128 f_hi = _mm_mul_ps(_mm_cvtepi32_ps(s32_hi), scale);
            _mm_storeu_ps(&d[i], f_lo);
            _mm_storeu_ps(&d[i + 4], f_hi);
        }

        for (; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }
#endif

    static void ConvertF32Mono(const void* src, void* dst, u32 frames, const float*) {
        std::memcpy(dst, src, frames * sizeof(float));
    }

    static void ConvertF32Stereo(const void* src, void* dst, u32 frames, const float*) {
        std::memcpy(dst, src, frames * 2 * sizeof(float));
    }

    static void ConvertF32_8CH(const void* src, void* dst, u32 frames, const float*) {
        std::memcpy(dst, src, frames * 8 * sizeof(float));
    }

    static void ConvertF32Std8CH(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);

        for (u32 i = 0; i < frames; i++) {
            const u32 offset = i << 3;

            d[offset + FL] = s[offset + FL];
            d[offset + FR] = s[offset + FR];
            d[offset + FC] = s[offset + FC];
            d[offset + LF] = s[offset + LF];
            d[offset + SL] = s[offset + STD_SL];
            d[offset + SR] = s[offset + STD_SR];
            d[offset + BL] = s[offset + STD_BL];
            d[offset + BR] = s[offset + STD_BR];
        }
    }

    const u32 frame_size;
    const u32 guest_buffer_size;
    const u32 buffer_frames;
    const u32 sample_rate;
    const u32 num_channels;
    const bool is_float;
    const bool is_std;
    const std::array<int, 8> channel_layout;

    alignas(64) u64 period_us{0};
    alignas(64) std::atomic<u64> last_output_time{0};
    u64 next_output_time{0};
    u64 last_volume_check_time{0};
    u32 output_count{0};

    u32 internal_buffer_size{0};
    void* internal_buffer{nullptr};

    ConverterFunc convert{nullptr};
    AudioConversionGuard conversion_guard{};

    alignas(64) std::atomic<float> current_gain{1.0f};

    SDL_AudioStream* stream{nullptr};
    u32 queue_threshold{0};
};

#ifdef __ANDROID__
class AndroidAAudioPortBackend : public PortBackend {
public:
    explicit AndroidAAudioPortBackend(const PortOut& port)
        : buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          guest_channels(port.format_info.num_channels),
          guest_sample_size(port.format_info.sample_size),
          output_channels(port.format_info.num_channels == 1 ? 1u : 2u),
          is_float(port.format_info.is_float), is_std(port.format_info.is_std),
          audio_output_disabled(AndroidAudioOutputDisabledRequested()) {
        output_buffer.resize(static_cast<size_t>(buffer_frames) * output_channels);
        channel_gains.fill(Config::getVolumeSlider() * 0.01f);
        callback_transport = AndroidAudioCallbackTransportRequested();
        if (callback_transport) {
            callback_fifo_capacity_frames = buffer_frames * 8;
            callback_prime_frames = buffer_frames * 2;
            callback_fifo.resize(static_cast<size_t>(callback_fifo_capacity_frames) *
                                 output_channels);
        }
        if (!OpenStream()) {
            Cleanup();
        } else {
            StartRawPcmCapture();
            if (callback_transport) {
                StartRecoveryThread();
            }
        }
    }

    ~AndroidAAudioPortBackend() override {
        Cleanup();
    }

    bool IsReady() const {
        return callback_transport ? published_stream.load(std::memory_order_acquire) != nullptr
                                  : stream != nullptr;
    }

    void Output(void* ptr) override {
        if (!ptr) {
            return;
        }

        if (AudioOutputDisabled()) {
            return;
        }
        if (raw_pcm_capture) [[unlikely]] {
            raw_pcm_capture->AppendGuest(ptr, buffer_frames);
        }
        if (!CanSubmitOutput()) {
            return;
        }

        const u32 probe_id = ProbeRawGuestPcm(ptr);
        const bool any_nonzero = ConvertToOutput(ptr);
        if (probe_id != 0) {
            ProbePostStereo(probe_id);
        }
        SubmitOutputBuffer(any_nonzero, probe_id);
    }

    void OutputFloat(const float* ptr, const u32 frames, const u32 channels) override {
        if (!ptr || AudioOutputDisabled()) {
            return;
        }
        if (raw_pcm_capture) [[unlikely]] {
            raw_pcm_capture->AppendFloat(ptr, frames, channels);
        }
        if (!CanSubmitOutput()) {
            return;
        }
        if (frames != buffer_frames || (channels != 1 && channels != 2 && channels != 8)) {
            AndroidAudioLog(
                "[EXECUTOR_AUDIO_AAUDIO_OUTPUT] invalid_normalized frames=%u/%u channels=%u",
                frames, buffer_frames, channels);
            return;
        }
        const AudioBufferFormat format{
            .sample_type = AudioSampleType::Float32,
            .channels = static_cast<u8>(channels),
            .standard_8ch = false,
        };
        const auto converted = uniform_channel_gain
                                   ? ConvertAudioBufferToFloat(
                                         ptr, frames, format, output_channels,
                                         gain.load(std::memory_order_acquire), output_buffer,
                                         normalized_conversion_guard)
                                   : ConvertAudioBufferToFloatPerChannel(
                                          ptr, frames, format, output_channels, channel_gains,
                                          output_buffer, normalized_per_channel_conversion_guard);
        SubmitOutputBuffer(converted.any_nonzero);
    }

    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        const float slider_gain = Config::getVolumeSlider() * 0.01f;
        const u32 channels_to_check = std::min(guest_channels, 8u);
        for (u32 channel = 0; channel < channel_gains.size(); ++channel) {
            channel_gains[channel] =
                std::max(0.0f, static_cast<float>(ch_volumes[channel]) * INV_VOLUME_0DB) *
                slider_gain;
        }
        const float first_gain = channel_gains[0];
        uniform_channel_gain = true;
        for (u32 channel = 1; channel < channels_to_check; ++channel) {
            if (channel_gains[channel] != first_gain) {
                uniform_channel_gain = false;
                break;
            }
        }
        gain.store(first_gain, std::memory_order_release);
    }

private:
    void StartRawPcmCapture() {
        if (!AndroidRawPcmCaptureRequested()) {
            return;
        }
        if (!AndroidRawPcmCapture::Supports(sample_rate, guest_channels, guest_sample_size,
                                            is_float)) {
            AndroidAudioLog(
                "[EXECUTOR_AUDIO_RAW_CAPTURE] state=skipped reason=unsupported "
                "sampleRate=%u channels=%u sampleSize=%u float=%u",
                sample_rate, guest_channels, guest_sample_size, is_float ? 1u : 0u);
            return;
        }
        if (AndroidRawPcmCaptureOwner.test_and_set(std::memory_order_acq_rel)) {
            AndroidAudioLog(
                "[EXECUTOR_AUDIO_RAW_CAPTURE] state=skipped reason=owner-active channels=%u",
                guest_channels);
            return;
        }
        raw_pcm_capture = std::make_unique<AndroidRawPcmCapture>(sample_rate, guest_channels,
                                                                is_float, is_std);
    }

    bool CanSubmitOutput() noexcept {
        if (!callback_transport) {
            return stream != nullptr;
        }
        callback_producer_heartbeat.fetch_add(1, std::memory_order_relaxed);
        return published_stream.load(std::memory_order_acquire) != nullptr;
    }

    bool AudioOutputDisabled() const noexcept {
        return audio_output_disabled;
    }

    u32 ProbeRawGuestPcm(const void* ptr) {
        const AndroidAudioProbeMode mode = AndroidAudioProbeModeRequested();
        const bool first_s16_block = mode == AndroidAudioProbeMode::FirstS16Stereo48k1024;
        const bool enabled = mode == AndroidAudioProbeMode::All ||
                             (first_s16_block && !is_float && guest_sample_size == sizeof(s16) &&
                              guest_channels == 2 && sample_rate == 48000 &&
                              buffer_frames == 1024);
        if (!enabled) {
            diagnostic_probe_active = false;
            diagnostic_probe_exhausted = false;
            return 0;
        }
        if (!diagnostic_probe_active && !diagnostic_probe_exhausted) {
            raw_probe_budget.store(first_s16_block ? 1 : 128, std::memory_order_relaxed);
            raw_probe_sequence.store(0, std::memory_order_relaxed);
            raw_previous_hash = 0;
            raw_two_back_hash = 0;
            post_previous_hash = 0;
            post_two_back_hash = 0;
            diagnostic_probe_active = true;
        }
        if (raw_probe_budget.load(std::memory_order_relaxed) <= 0) {
            diagnostic_probe_active = false;
            diagnostic_probe_exhausted = true;
            return 0;
        }

        const size_t raw_size = static_cast<size_t>(buffer_frames) * guest_channels *
                                (is_float ? sizeof(float) : sizeof(s16));
        const u64 raw_hash = AndroidAudioProbeHash(ptr, raw_size);
        constexpr u32 PatternFrames = 8;
        std::array<AudioPcmProbeLaneStats, 8> lane_stats{};
        std::array<std::array<u32, 8>, PatternFrames> frame_bits{};
        const u32 pattern_frames = std::min(buffer_frames, PatternFrames);
        bool any_nonzero = false;
        for (u32 frame = 0; frame < buffer_frames; ++frame) {
            const size_t frame_base = static_cast<size_t>(frame) * guest_channels;
            for (u32 channel = 0; channel < guest_channels; ++channel) {
                const size_t sample_index = frame_base + channel;
                float sample{};
                u32 bits{};
                if (is_float) {
                    sample = static_cast<const float*>(ptr)[sample_index];
                    std::memcpy(&bits, &sample, sizeof(bits));
                } else {
                    const s16 raw_sample = static_cast<const s16*>(ptr)[sample_index];
                    sample = static_cast<float>(raw_sample) * INV_VOLUME_0DB;
                    bits = static_cast<u16>(raw_sample);
                }
                lane_stats[channel].Observe(sample);
                any_nonzero |= sample != 0.0f || !std::isfinite(sample);
                if (frame < pattern_frames) {
                    frame_bits[frame][channel] = bits;
                }
            }
        }
        if (!any_nonzero) {
            return 0;
        }

        int budget = raw_probe_budget.load(std::memory_order_relaxed);
        while (budget > 0 &&
               !raw_probe_budget.compare_exchange_weak(budget, budget - 1,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
        }
        if (budget <= 0) {
            return 0;
        }

        const u32 probe_id = raw_probe_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool same_previous = raw_previous_hash != 0 && raw_hash == raw_previous_hash;
        const bool same_two_back = raw_two_back_hash != 0 && raw_hash == raw_two_back_hash;
        if (probe_id <= 8 || same_previous || same_two_back) {
            AndroidAudioProbeWarn(
                "[EXECUTOR_AUDIO_RAW_HASH] id=%u hash=%016llx previous=%016llx "
                "twoBack=%016llx same1=%u same2=%u frames=%u sampleRate=%u channels=%u "
                "sample=%s",
                probe_id, static_cast<unsigned long long>(raw_hash),
                static_cast<unsigned long long>(raw_previous_hash),
                static_cast<unsigned long long>(raw_two_back_hash), same_previous ? 1u : 0u,
                same_two_back ? 1u : 0u, buffer_frames, sample_rate, guest_channels,
                is_float ? "f32" : "s16");
        }
        raw_two_back_hash = raw_previous_hash;
        raw_previous_hash = raw_hash;

        if (probe_id <= 4) {
            AndroidAudioProbeWarn(
                "[EXECUTOR_AUDIO_RAW_PCM] id=%u frames=%u channels=%u sample=%s std=%d "
                "patternFrames=%u remaining=%d",
                probe_id, buffer_frames, guest_channels, is_float ? "f32" : "s16",
                is_std ? 1 : 0, pattern_frames, budget - 1);
            for (u32 channel = 0; channel < guest_channels; ++channel) {
                const auto& stats = lane_stats[channel];
                AndroidAudioProbeWarn(
                    "[EXECUTOR_AUDIO_RAW_PCM_LANE] id=%u physical=%u peak=%.9g rms=%.9g "
                    "dc=%.9g finite=%u nonFinite=%u clipped=%u zero=%u subnormal=%u",
                    probe_id, channel, stats.peak, stats.Rms(), stats.Dc(), stats.finite_samples,
                    stats.non_finite_samples, stats.clipped_samples, stats.zero_samples,
                    stats.subnormal_samples);
            }
            for (u32 frame = 0; frame < pattern_frames; ++frame) {
                const auto& bits = frame_bits[frame];
                AndroidAudioProbeWarn(
                    "[EXECUTOR_AUDIO_RAW_PCM_BITS] id=%u frame=%u valid=%u "
                    "bits=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                    probe_id, frame, guest_channels, bits[0], bits[1], bits[2], bits[3], bits[4],
                    bits[5], bits[6], bits[7]);
            }
        }
        return probe_id;
    }

    void ProbePostStereo(const u32 probe_id) {
        const u64 post_hash = AndroidAudioProbeHash(
            output_buffer.data(), output_buffer.size() * sizeof(output_buffer.front()));
        const bool same_previous = post_previous_hash != 0 && post_hash == post_previous_hash;
        const bool same_two_back = post_two_back_hash != 0 && post_hash == post_two_back_hash;
        if (probe_id <= 8 || same_previous || same_two_back) {
            AndroidAudioProbeWarn(
                "[EXECUTOR_AUDIO_POST_HASH] id=%u hash=%016llx previous=%016llx "
                "twoBack=%016llx same1=%u same2=%u frames=%u channels=%u",
                probe_id, static_cast<unsigned long long>(post_hash),
                static_cast<unsigned long long>(post_previous_hash),
                static_cast<unsigned long long>(post_two_back_hash), same_previous ? 1u : 0u,
                same_two_back ? 1u : 0u, buffer_frames, output_channels);
        }
        post_two_back_hash = post_previous_hash;
        post_previous_hash = post_hash;

        if (probe_id > 4) {
            return;
        }
        std::array<AudioPcmProbeLaneStats, 2> lane_stats{};
        for (u32 frame = 0; frame < buffer_frames; ++frame) {
            const size_t frame_base = static_cast<size_t>(frame) * output_channels;
            for (u32 channel = 0; channel < output_channels; ++channel) {
                lane_stats[channel].Observe(output_buffer[frame_base + channel]);
            }
        }
        AndroidAudioProbeWarn(
            "[EXECUTOR_AUDIO_POST_STEREO] id=%u frames=%u sampleRate=%u channels=%u "
            "sourceChannels=%u sourceSample=%s",
            probe_id, buffer_frames, sample_rate, output_channels, guest_channels,
            is_float ? "f32" : "s16");
        for (u32 channel = 0; channel < output_channels; ++channel) {
            const auto& stats = lane_stats[channel];
            AndroidAudioProbeWarn(
                "[EXECUTOR_AUDIO_POST_STEREO_LANE] id=%u channel=%u peak=%.9g rms=%.9g "
                "dc=%.9g finite=%u nonFinite=%u clipped=%u zero=%u subnormal=%u",
                probe_id, channel, stats.peak, stats.Rms(), stats.Dc(), stats.finite_samples,
                stats.non_finite_samples, stats.clipped_samples, stats.zero_samples,
                stats.subnormal_samples);
        }
    }

    bool EnqueueCallbackBuffer(const u32 probe_id) {
        const u64 read_frame = callback_read_frame.load(std::memory_order_acquire);
        const u64 write_frame = callback_write_frame.load(std::memory_order_relaxed);
        const u64 queued_frames = write_frame - read_frame;
        if (queued_frames > callback_fifo_capacity_frames - buffer_frames) {
            callback_overflow_blocks.fetch_add(1, std::memory_order_relaxed);
            RecordAudioBackendWrite(0, 0, buffer_frames, 0);
            if (probe_id != 0) {
                AndroidAudioProbeWarn(
                    "[EXECUTOR_AUDIO_CALLBACK_ENQUEUE] id=%u enqueued=0 queued=%llu capacity=%u",
                    probe_id, static_cast<unsigned long long>(queued_frames),
                    callback_fifo_capacity_frames);
            }
            return false;
        }

        const auto copy_started = std::chrono::steady_clock::now();
        const u32 write_offset = static_cast<u32>(write_frame % callback_fifo_capacity_frames);
        const u32 first_frames =
            std::min(buffer_frames, callback_fifo_capacity_frames - write_offset);
        const size_t first_samples = static_cast<size_t>(first_frames) * output_channels;
        std::memcpy(callback_fifo.data() + static_cast<size_t>(write_offset) * output_channels,
                    output_buffer.data(), first_samples * sizeof(float));
        const u32 second_frames = buffer_frames - first_frames;
        if (second_frames != 0) {
            std::memcpy(callback_fifo.data(), output_buffer.data() + first_samples,
                        static_cast<size_t>(second_frames) * output_channels * sizeof(float));
        }
        callback_write_frame.store(write_frame + buffer_frames, std::memory_order_release);
        RecordAudioBackendWrite(ElapsedNanoseconds(copy_started), buffer_frames, buffer_frames, 0);
        if (probe_id != 0 && probe_id <= 8) {
            AndroidAudioProbeWarn(
                "[EXECUTOR_AUDIO_CALLBACK_ENQUEUE] id=%u enqueued=1 queued=%llu capacity=%u",
                probe_id, static_cast<unsigned long long>(queued_frames + buffer_frames),
                callback_fifo_capacity_frames);
        }
        return true;
    }

    static void AAudioErrorCallback(AAudioStream* failed_stream, void* user_data,
                                    const aaudio_result_t error) noexcept {
        static_cast<AndroidAAudioPortBackend*>(user_data)->SignalRecovery(failed_stream, error);
    }

    static aaudio_data_callback_result_t AAudioDataCallback(AAudioStream* callback_stream,
                                                            void* user_data,
                                                            void* audio_data,
                                                            int32_t num_frames) noexcept {
        return static_cast<AndroidAAudioPortBackend*>(user_data)->OnAAudioData(
            callback_stream, static_cast<float*>(audio_data), num_frames);
    }

    aaudio_data_callback_result_t OnAAudioData(AAudioStream* callback_stream, float* destination,
                                               const int32_t num_frames) noexcept {
        if (destination == nullptr || num_frames <= 0) {
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }

        const size_t destination_samples = static_cast<size_t>(num_frames) * output_channels;
        if (callback_stream != published_stream.load(std::memory_order_acquire)) {
            std::memset(destination, 0, destination_samples * sizeof(float));
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        callback_consumer_heartbeat.fetch_add(1, std::memory_order_relaxed);
        const u64 read_frame = callback_read_frame.load(std::memory_order_relaxed);
        const u64 write_frame = callback_write_frame.load(std::memory_order_acquire);
        const u64 available_frames = write_frame - read_frame;
        const u64 prime_target =
            std::min<u64>(callback_fifo_capacity_frames,
                          std::max<u64>(callback_prime_frames, static_cast<u64>(num_frames)));
        if (!callback_primed.load(std::memory_order_relaxed) &&
            available_frames < prime_target) {
            std::memset(destination, 0, destination_samples * sizeof(float));
            callback_preroll_frames.fetch_add(static_cast<u64>(num_frames),
                                              std::memory_order_relaxed);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        callback_primed.store(true, std::memory_order_relaxed);

        const u32 frames_to_copy =
            static_cast<u32>(std::min<u64>(available_frames, static_cast<u64>(num_frames)));
        const u32 read_offset = static_cast<u32>(read_frame % callback_fifo_capacity_frames);
        const u32 first_frames =
            std::min(frames_to_copy, callback_fifo_capacity_frames - read_offset);
        const size_t first_samples = static_cast<size_t>(first_frames) * output_channels;
        std::memcpy(destination,
                    callback_fifo.data() + static_cast<size_t>(read_offset) * output_channels,
                    first_samples * sizeof(float));
        const u32 second_frames = frames_to_copy - first_frames;
        if (second_frames != 0) {
            std::memcpy(destination + first_samples, callback_fifo.data(),
                        static_cast<size_t>(second_frames) * output_channels * sizeof(float));
        }
        callback_read_frame.store(read_frame + frames_to_copy, std::memory_order_release);

        if (frames_to_copy < static_cast<u32>(num_frames)) {
            const u32 missing_frames = static_cast<u32>(num_frames) - frames_to_copy;
            std::memset(destination + static_cast<size_t>(frames_to_copy) * output_channels, 0,
                        static_cast<size_t>(missing_frames) * output_channels * sizeof(float));
            callback_underflow_frames.fetch_add(missing_frames, std::memory_order_relaxed);
            callback_primed.store(false, std::memory_order_relaxed);
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void SubmitOutputBuffer(const bool any_nonzero, const u32 probe_id = 0) {
        if (!any_nonzero && !output_buffer.empty()) {
            silence_dither_phase = !silence_dither_phase;
            output_buffer[0] = silence_dither_phase ? 1.0e-8f : -1.0e-8f;
        }

        if (callback_transport) {
            (void)EnqueueCallbackBuffer(probe_id);
            return;
        }

        // AAudio explicitly permits a bounded write to return fewer frames than
        // requested.  A 1024-frame PS4 block at 48 kHz is 21.33 ms, so the old
        // fixed 20 ms timeout could systematically discard the tail of every
        // block on legacy synchronous AudioTrack paths.  Preserve the guest
        // stream by advancing the source pointer and completing the block.  A
        // zero/error return terminates the bounded loop without spinning.
        const int64_t period_ns =
            sample_rate == 0
                ? INT64_C(20'000'000)
                : static_cast<int64_t>((UINT64_C(1'000'000'000) * buffer_frames +
                                        sample_rate - 1) /
                                       sample_rate);
        const int64_t write_timeout_ns =
            std::max<int64_t>(INT64_C(20'000'000), period_ns * 2);
        int32_t total_written = 0;
        u32 write_chunks = 0;
        u64 total_write_nanoseconds = 0;
        aaudio_result_t terminal_result = AAUDIO_OK;
        const int64_t frames_written_before =
            probe_id == 0 ? 0 : AAudioStream_getFramesWritten(stream);
        const int64_t frames_read_before =
            probe_id == 0 ? 0 : AAudioStream_getFramesRead(stream);
        while (total_written < static_cast<int32_t>(buffer_frames)) {
            const int32_t remaining = static_cast<int32_t>(buffer_frames) - total_written;
            const auto write_start = std::chrono::steady_clock::now();
            const aaudio_result_t written = AAudioStream_write(
                stream,
                output_buffer.data() + static_cast<size_t>(total_written) * output_channels,
                remaining, write_timeout_ns);
            const u64 write_nanoseconds = ElapsedNanoseconds(write_start);
            total_write_nanoseconds += write_nanoseconds;
            ++write_chunks;
            RecordAudioBackendWrite(write_nanoseconds, written, static_cast<u32>(remaining), 0);
            if (written <= 0) {
                terminal_result = written;
                break;
            }
            total_written += written;
        }

        if (probe_id != 0) {
            const int64_t frames_written_after = AAudioStream_getFramesWritten(stream);
            const int64_t frames_read_after = AAudioStream_getFramesRead(stream);
            const int64_t written_delta = frames_written_after - frames_written_before;
            const int64_t read_delta = frames_read_after - frames_read_before;
            if (probe_id <= 8 || written_delta != total_written) {
                AndroidAudioProbeWarn(
                    "[EXECUTOR_AUDIO_AAUDIO_FLOW] id=%u completed=%d/%u chunks=%u "
                    "writtenBefore=%lld writtenAfter=%lld writtenDelta=%lld "
                    "readBefore=%lld readAfter=%lld readDelta=%lld terminal=%d "
                    "sampleRate=%u sourceChannels=%u sourceSample=%s",
                    probe_id, total_written, buffer_frames, write_chunks,
                    static_cast<long long>(frames_written_before),
                    static_cast<long long>(frames_written_after),
                    static_cast<long long>(written_delta),
                    static_cast<long long>(frames_read_before),
                    static_cast<long long>(frames_read_after),
                    static_cast<long long>(read_delta), static_cast<int>(terminal_result),
                    sample_rate, guest_channels, is_float ? "f32" : "s16");
            }
        }

        if (terminal_result < 0) {
            if (TakeOutputLogBudget()) {
                AndroidAudioLog(
                    "[EXECUTOR_AUDIO_AAUDIO_OUTPUT] error=%s completed=%d/%u chunks=%u",
                    AAudio_convertResultToText(terminal_result), total_written, buffer_frames,
                    write_chunks);
            }
            return;
        }
        u32 xrun_delta{};
        if ((++output_call_count & 255u) == 0u) {
            const int32_t xruns = AAudioStream_getXRunCount(stream);
            if (xruns > last_xrun_count) {
                xrun_delta = static_cast<u32>(xruns - last_xrun_count);
                last_xrun_count = xruns;
            }
        }
        if (xrun_delta != 0) {
            RecordAudioBackendWrite(0, 0, 0, xrun_delta);
        }

        if (TakeOutputLogBudget()) {
            float peak = 0.0f;
            const size_t sample_count =
                std::min<size_t>(output_buffer.size(), static_cast<size_t>(512));
            for (size_t i = 0; i < sample_count; i++) {
                peak = std::max(peak, std::abs(output_buffer[i]));
            }
            AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_OUTPUT] frames=%d/%u outCh=%u guestCh=%u "
                            "float=%d peak=%.4f chunks=%u writeUs=%llu timeoutUs=%lld",
                            total_written, buffer_frames, output_channels, guest_channels,
                            is_float ? 1 : 0, peak, write_chunks,
                            static_cast<unsigned long long>(total_write_nanoseconds / 1000),
                            static_cast<long long>(write_timeout_ns / 1000));
        }

        if (any_nonzero && TakeContentLogBudget()) {
            std::array<double, 2> sum{};
            std::array<double, 2> sum_squares{};
            std::array<float, 2> channel_peak{};
            u32 clipped_samples{};
            u32 non_finite_samples{};
            for (u32 frame = 0; frame < buffer_frames; ++frame) {
                for (u32 channel = 0; channel < output_channels; ++channel) {
                    const float sample = output_buffer[static_cast<size_t>(frame) *
                                                       output_channels + channel];
                    if (!std::isfinite(sample)) {
                        ++non_finite_samples;
                        continue;
                    }
                    sum[channel] += sample;
                    sum_squares[channel] += static_cast<double>(sample) * sample;
                    channel_peak[channel] = std::max(channel_peak[channel], std::abs(sample));
                    clipped_samples += std::abs(sample) > 1.0f ? 1u : 0u;
                }
            }
            const double frame_count = static_cast<double>(buffer_frames);
            const double left_rms = std::sqrt(sum_squares[0] / frame_count);
            const double right_rms = output_channels > 1
                                         ? std::sqrt(sum_squares[1] / frame_count)
                                         : left_rms;
            const double left_dc = sum[0] / frame_count;
            const double right_dc = output_channels > 1 ? sum[1] / frame_count : left_dc;
            AndroidAudioLog(
                "[EXECUTOR_AUDIO_AAUDIO_CONTENT] peak=%.5f/%.5f rms=%.5f/%.5f "
                "dc=%.5f/%.5f clipped=%u nonFinite=%u chunks=%u completed=%d/%u",
                channel_peak[0], output_channels > 1 ? channel_peak[1] : channel_peak[0],
                left_rms, right_rms, left_dc, right_dc, clipped_samples,
                non_finite_samples, write_chunks, total_written, buffer_frames);
        }
    }
    void SignalRecovery(AAudioStream* failed_stream, const aaudio_result_t error) noexcept {
        if (!callback_transport || error == AAUDIO_OK ||
            recovery_shutting_down.load(std::memory_order_acquire) ||
            failed_stream != published_stream.load(std::memory_order_acquire)) {
            return;
        }
        recovery_error.store(error, std::memory_order_relaxed);
        if (!recovery_pending.exchange(true, std::memory_order_acq_rel)) {
            recovery_signal.release();
        }
    }

    bool CallbackQueueHasFrames() const noexcept {
        return callback_write_frame.load(std::memory_order_acquire) !=
               callback_read_frame.load(std::memory_order_acquire);
    }

    void ResetCallbackQueueForRestart() noexcept {
        const u64 write_frame = callback_write_frame.load(std::memory_order_acquire);
        callback_read_frame.store(write_frame, std::memory_order_release);
        callback_primed.store(false, std::memory_order_relaxed);
    }

    void CloseOwnedStream() noexcept {
        AAudioStream* closing_stream = stream;
        stream = nullptr;
        if (!closing_stream) {
            return;
        }
        // Stop publication first so the RT/error callbacks cannot schedule
        // recovery for a stream whose owner is being retired. AAudio stop is
        // asynchronous; wait for one bounded state transition before close so
        // the callback no longer observes backend storage during destruction.
        published_stream.store(nullptr, std::memory_order_release);
        (void)AAudioStream_requestStop(closing_stream);
        aaudio_stream_state_t state = AAudioStream_getState(closing_stream);
        constexpr int64_t StopWaitSliceNs = INT64_C(125000000);
        constexpr int StopWaitTransitions = 4;
        for (int transition = 0; transition < StopWaitTransitions; ++transition) {
            if (state == AAUDIO_STREAM_STATE_STOPPED ||
                state == AAUDIO_STREAM_STATE_CLOSED ||
                state == AAUDIO_STREAM_STATE_DISCONNECTED ||
                state == AAUDIO_STREAM_STATE_UNINITIALIZED) {
                break;
            }
            aaudio_stream_state_t next_state = state;
            const aaudio_result_t wait_result = AAudioStream_waitForStateChange(
                closing_stream, state, &next_state, StopWaitSliceNs);
            if (wait_result != AAUDIO_OK && wait_result != AAUDIO_ERROR_TIMEOUT) {
                break;
            }
            state = next_state;
        }
        (void)AAudioStream_close(closing_stream);
    }

    bool RecoverStream(const char* reason, const aaudio_result_t error) {
        published_stream.store(nullptr, std::memory_order_release);
        CloseOwnedStream();
        ResetCallbackQueueForRestart();
        ++callback_recovery_generation;
        const bool opened = OpenStream();
        if (opened) {
            ++callback_recovery_count;
        } else {
            ++callback_recovery_failures;
        }
        AndroidAudioLog(
            "[EXECUTOR_AUDIO_AAUDIO_RECOVERY] reason=%s error=%d generation=%llu result=%s",
            reason, static_cast<int>(error),
            static_cast<unsigned long long>(callback_recovery_generation),
            opened ? "opened" : "failed");
        return opened;
    }

    bool RestartStoppedStream() {
        if (!stream) {
            return false;
        }
        aaudio_stream_state_t state = AAudioStream_getState(stream);
        (void)AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_UNKNOWN, &state, 0);
        if (state != AAUDIO_STREAM_STATE_OPEN && state != AAUDIO_STREAM_STATE_PAUSED &&
            state != AAUDIO_STREAM_STATE_STOPPED && state != AAUDIO_STREAM_STATE_STARTING) {
            return false;
        }
        if (state != AAUDIO_STREAM_STATE_STARTING) {
            const aaudio_result_t result = AAudioStream_requestStart(stream);
            if (result != AAUDIO_OK) {
                return false;
            }
        }
        AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_RECOVERY] action=request-start state=%s",
                        AAudio_convertStreamStateToText(state));
        return true;
    }

    void RecoveryLoop(const std::stop_token stop) {
        constexpr auto PollInterval = std::chrono::milliseconds{250};
        constexpr auto CallbackStartTimeout = std::chrono::milliseconds{750};
        constexpr u32 StallPollLimit = 3;
        constexpr std::array RetryBackoff{
            std::chrono::milliseconds{100}, std::chrono::milliseconds{250},
            std::chrono::milliseconds{500}, std::chrono::milliseconds{1000},
            std::chrono::milliseconds{2000}, std::chrono::milliseconds{5000}};

        size_t retry_index{};
        bool retry_scheduled{};
        bool awaiting_callback{};
        u64 awaited_callback_heartbeat{};
        auto retry_at = std::chrono::steady_clock::time_point::max();
        auto callback_deadline = std::chrono::steady_clock::time_point::max();
        u64 observed_callback = callback_consumer_heartbeat.load(std::memory_order_relaxed);
        u64 observed_producer = callback_producer_heartbeat.load(std::memory_order_relaxed);
        u32 stalled_polls{};

        const auto schedule_retry = [&] {
            const auto delay = RetryBackoff[std::min(retry_index, RetryBackoff.size() - 1)];
            if (retry_index + 1 < RetryBackoff.size()) {
                ++retry_index;
            }
            retry_at = std::chrono::steady_clock::now() + delay;
            retry_scheduled = true;
            awaiting_callback = false;
        };

        const auto begin_callback_wait = [&](const u64 heartbeat_before) {
            const u64 heartbeat_after =
                callback_consumer_heartbeat.load(std::memory_order_acquire);
            if (heartbeat_after != heartbeat_before) {
                retry_index = 0;
                retry_scheduled = false;
                awaiting_callback = false;
                return;
            }
            awaited_callback_heartbeat = heartbeat_after;
            callback_deadline = std::chrono::steady_clock::now() + CallbackStartTimeout;
            retry_scheduled = false;
            awaiting_callback = true;
        };

        while (!stop.stop_requested()) {
            (void)recovery_signal.try_acquire_for(PollInterval);
            if (stop.stop_requested()) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool explicit_recovery =
                recovery_pending.exchange(false, std::memory_order_acq_rel);
            if (explicit_recovery) {
                const u64 heartbeat_before =
                    callback_consumer_heartbeat.load(std::memory_order_acquire);
                const aaudio_result_t error = recovery_error.load(std::memory_order_relaxed);
                if (RecoverStream("error-callback", error)) {
                    begin_callback_wait(heartbeat_before);
                } else {
                    schedule_retry();
                }
                stalled_polls = 0;
            }

            const u64 callback_heartbeat =
                callback_consumer_heartbeat.load(std::memory_order_acquire);
            const u64 producer_heartbeat =
                callback_producer_heartbeat.load(std::memory_order_acquire);

            if (awaiting_callback) {
                if (callback_heartbeat != awaited_callback_heartbeat) {
                    awaiting_callback = false;
                    retry_scheduled = false;
                    retry_index = 0;
                    stalled_polls = 0;
                } else if (now >= callback_deadline) {
                    published_stream.store(nullptr, std::memory_order_release);
                    CloseOwnedStream();
                    ResetCallbackQueueForRestart();
                    ++callback_recovery_failures;
                    AndroidAudioLog(
                        "[EXECUTOR_AUDIO_AAUDIO_RECOVERY] reason=start-timeout generation=%llu",
                        static_cast<unsigned long long>(callback_recovery_generation));
                    schedule_retry();
                }
            } else if (published_stream.load(std::memory_order_acquire) != nullptr) {
                if (callback_heartbeat != observed_callback) {
                    stalled_polls = 0;
                } else if (producer_heartbeat != observed_producer || CallbackQueueHasFrames()) {
                    if (++stalled_polls >= StallPollLimit) {
                        ++callback_watchdog_stalls;
                        const u64 heartbeat_before = callback_heartbeat;
                        if (RestartStoppedStream()) {
                            begin_callback_wait(heartbeat_before);
                        } else if (RecoverStream("callback-stall", AAUDIO_ERROR_DISCONNECTED)) {
                            begin_callback_wait(heartbeat_before);
                        } else {
                            schedule_retry();
                        }
                        stalled_polls = 0;
                    }
                } else {
                    stalled_polls = 0;
                }
            } else if (retry_scheduled && now >= retry_at) {
                const u64 heartbeat_before = callback_heartbeat;
                if (RecoverStream("retry", recovery_error.load(std::memory_order_relaxed))) {
                    begin_callback_wait(heartbeat_before);
                } else {
                    schedule_retry();
                }
            }

            observed_callback = callback_heartbeat;
            observed_producer = producer_heartbeat;
        }
    }

    void StartRecoveryThread() {
        recovery_thread =
            std::jthread([this](const std::stop_token stop) { RecoveryLoop(stop); });
    }

    void StopRecoveryThread() noexcept {
        if (!callback_transport) {
            return;
        }
        recovery_shutting_down.store(true, std::memory_order_release);
        published_stream.store(nullptr, std::memory_order_release);
        if (!recovery_thread.joinable()) {
            return;
        }
        recovery_thread.request_stop();
        if (!recovery_pending.exchange(true, std::memory_order_acq_rel)) {
            recovery_signal.release();
        }
        recovery_thread.join();
    }

    bool OpenStream() {
        if (AudioOutputDisabled()) {
            AndroidAudioLog(
                "[EXECUTOR_AUDIO_AAUDIO_OPEN] disabled=1 (MIUI AudioTrack crash escape)");
            return false;
        }
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK || !builder) {
            AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_OPEN] create_builder error=%s",
                            AAudio_convertResultToText(result));
            return false;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sample_rate));
        AAudioStreamBuilder_setChannelCount(builder, static_cast<int32_t>(output_channels));
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setBufferCapacityInFrames(
            builder, static_cast<int32_t>(std::max<u32>(buffer_frames * 4, 512)));
        if (callback_transport) {
            AAudioStreamBuilder_setDataCallback(builder, AAudioDataCallback, this);
            AAudioStreamBuilder_setErrorCallback(builder, AAudioErrorCallback, this);
        }

        AAudioStream* candidate = nullptr;
        result = AAudioStreamBuilder_openStream(builder, &candidate);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !candidate) {
            AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_OPEN] open error=%s",
                            AAudio_convertResultToText(result));
            return false;
        }

        if (callback_transport) {
            const int32_t effective_sample_rate = AAudioStream_getSampleRate(candidate);
            const int32_t effective_channels = AAudioStream_getChannelCount(candidate);
            const aaudio_format_t effective_format = AAudioStream_getFormat(candidate);
            if (effective_sample_rate != static_cast<int32_t>(sample_rate) ||
                effective_channels != static_cast<int32_t>(output_channels) ||
                effective_format != AAUDIO_FORMAT_PCM_FLOAT) {
                AndroidAudioLog(
                    "[EXECUTOR_AUDIO_AAUDIO_OPEN] incompatible effectiveRate=%d effectiveCh=%d "
                    "effectiveFormat=%d requestedRate=%u requestedCh=%u",
                    effective_sample_rate, effective_channels, static_cast<int>(effective_format),
                    sample_rate, output_channels);
                (void)AAudioStream_close(candidate);
                return false;
            }
        }

        stream = candidate;
        if (callback_transport) {
            published_stream.store(candidate, std::memory_order_release);
        }
        result = AAudioStream_requestStart(candidate);
        if (result != AAUDIO_OK) {
            AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_OPEN] start error=%s",
                            AAudio_convertResultToText(result));
            if (callback_transport) {
                published_stream.store(nullptr, std::memory_order_release);
            }
            CloseOwnedStream();
            return false;
        }

        AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_OPEN] ok sampleRate=%u guestCh=%u outCh=%u "
                        "frames=%u transport=%s fifoFrames=%u primeFrames=%u",
                        sample_rate, guest_channels, output_channels, buffer_frames,
                        callback_transport ? "callback" : "legacy-sync",
                        callback_fifo_capacity_frames, callback_prime_frames);
        return true;
    }

    void Cleanup() {
        StopRecoveryThread();
        CloseOwnedStream();
        if (callback_transport) {
            AndroidAudioLog(
                "[EXECUTOR_AUDIO_CALLBACK_SUMMARY] prerollFrames=%llu underflowFrames=%llu "
                "overflowBlocks=%llu recoveries=%llu recoveryFailures=%llu watchdogStalls=%llu",
                static_cast<unsigned long long>(
                    callback_preroll_frames.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    callback_underflow_frames.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    callback_overflow_blocks.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(callback_recovery_count),
                static_cast<unsigned long long>(callback_recovery_failures),
                static_cast<unsigned long long>(callback_watchdog_stalls));
        }
    }

    bool TakeOutputLogBudget() {
        int budget = output_log_budget.load(std::memory_order_relaxed);
        while (budget > 0 && !output_log_budget.compare_exchange_weak(budget, budget - 1,
                                                                      std::memory_order_relaxed,
                                                                      std::memory_order_relaxed)) {
        }
        return budget > 0;
    }

    bool TakeContentLogBudget() {
        int budget = content_log_budget.load(std::memory_order_relaxed);
        while (budget > 0 &&
               !content_log_budget.compare_exchange_weak(budget, budget - 1,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
        }
        return budget > 0;
    }

#if 0
    // Retained temporarily as a readable scalar oracle for older captures.
    // Production conversion below is shared with SDL and the isolated PS5
    // runtime through audio_mix.cpp.
    bool ValidateSimdOutput(const void* ptr, const float current_gain) const {
        const auto matches = [](const float actual, const float expected) {
            if (std::isnan(expected)) {
                return std::isnan(actual);
            }
            if (!std::isfinite(actual) || !std::isfinite(expected)) {
                return actual == expected;
            }
            const float tolerance =
                1.0e-5f * std::max(1.0f, std::abs(expected));
            return std::abs(actual - expected) <= tolerance;
        };
        if (guest_channels == 8) {
            constexpr float CenterMix = 0.70710678f;
            constexpr float SurroundMix = 0.70710678f;
            for (u32 frame = 0; frame < buffer_frames; ++frame) {
                const u32 src_base = frame * 8;
                const u32 dst_base = frame * 2;
                float left{};
                float right{};
                if (is_float) {
                    const float* src = static_cast<const float*>(ptr);
                    const float center = src[src_base + FC] * CenterMix;
                    left = (src[src_base + FL] + center +
                            (src[src_base + SL] + src[src_base + BL]) *
                                SurroundMix) *
                           current_gain;
                    right = (src[src_base + FR] + center +
                             (src[src_base + SR] + src[src_base + BR]) *
                                 SurroundMix) *
                            current_gain;
                } else {
                    const s16* src = static_cast<const s16*>(ptr);
                    const float center =
                        static_cast<float>(src[src_base + FC]) * CenterMix;
                    left = (static_cast<float>(src[src_base + FL]) + center +
                            static_cast<float>(
                                src[src_base + SL] + src[src_base + BL]) *
                                SurroundMix) *
                           INV_VOLUME_0DB * current_gain;
                    right = (static_cast<float>(src[src_base + FR]) + center +
                             static_cast<float>(
                                 src[src_base + SR] + src[src_base + BR]) *
                                 SurroundMix) *
                            INV_VOLUME_0DB * current_gain;
                }
                if (!matches(output_buffer[dst_base], left) ||
                    !matches(output_buffer[dst_base + 1], right)) {
                    return false;
                }
            }
            return true;
        }

        for (u32 frame = 0; frame < buffer_frames; ++frame) {
            const u32 src_base = frame * guest_channels;
            const u32 dst_base = frame * output_channels;
            float left{};
            float right{};
            if (is_float) {
                const float* src = static_cast<const float*>(ptr);
                left = src[src_base] * current_gain;
                if (output_channels == 2) {
                    right =
                        src[src_base + (guest_channels > 1 ? 1 : 0)] * current_gain;
                }
            } else {
                const s16* src = static_cast<const s16*>(ptr);
                left = static_cast<float>(src[src_base]) *
                       INV_VOLUME_0DB * current_gain;
                if (output_channels == 2) {
                    right = static_cast<float>(
                                src[src_base + (guest_channels > 1 ? 1 : 0)]) *
                            INV_VOLUME_0DB * current_gain;
                }
            }
            if (!matches(output_buffer[dst_base], left) ||
                (output_channels == 2 &&
                 !matches(output_buffer[dst_base + 1], right))) {
                return false;
            }
        }
        return true;
    }

    bool ConvertToOutput(const void* ptr) {
        const auto conversion_start = std::chrono::steady_clock::now();
        const float current_gain = gain.load(std::memory_order_acquire);
        bool any_nonzero = false;
        bool used_simd = false;

#if defined(LSX4_HAS_ARM64_NEON)
        if (IsManagedAudioSimdEnabled() && !simd_guard_failed) {
            used_simd = true;
            if (guest_channels == 8) {
                constexpr float CenterMix = 0.70710678f;
                constexpr float SurroundMix = 0.70710678f;
                const float32x4_t left_low_weights =
                    {1.0f, 0.0f, CenterMix, 0.0f};
                const float32x4_t right_low_weights =
                    {0.0f, 1.0f, CenterMix, 0.0f};
                const float32x4_t left_high_weights =
                    {SurroundMix, 0.0f, SurroundMix, 0.0f};
                const float32x4_t right_high_weights =
                    {0.0f, SurroundMix, 0.0f, SurroundMix};
                const float scale =
                    current_gain * (is_float ? 1.0f : INV_VOLUME_0DB);
                if (is_float) {
                    const float* src = static_cast<const float*>(ptr);
                    for (u32 frame = 0; frame < buffer_frames; ++frame) {
                        const float32x4_t low = vld1q_f32(src + frame * 8);
                        const float32x4_t high =
                            vld1q_f32(src + frame * 8 + 4);
                        const float left =
                            (vaddvq_f32(vmulq_f32(low, left_low_weights)) +
                             vaddvq_f32(vmulq_f32(high, left_high_weights))) *
                            scale;
                        const float right =
                            (vaddvq_f32(vmulq_f32(low, right_low_weights)) +
                             vaddvq_f32(vmulq_f32(high, right_high_weights))) *
                            scale;
                        output_buffer[frame * 2] = left;
                        output_buffer[frame * 2 + 1] = right;
                        any_nonzero |= left != 0.0f || right != 0.0f;
                    }
                } else {
                    const s16* src = static_cast<const s16*>(ptr);
                    for (u32 frame = 0; frame < buffer_frames; ++frame) {
                        const int16x8_t samples =
                            vld1q_s16(src + frame * 8);
                        const float32x4_t low = vcvtq_f32_s32(
                            vmovl_s16(vget_low_s16(samples)));
                        const float32x4_t high = vcvtq_f32_s32(
                            vmovl_s16(vget_high_s16(samples)));
                        const float left =
                            (vaddvq_f32(vmulq_f32(low, left_low_weights)) +
                             vaddvq_f32(vmulq_f32(high, left_high_weights))) *
                            scale;
                        const float right =
                            (vaddvq_f32(vmulq_f32(low, right_low_weights)) +
                             vaddvq_f32(vmulq_f32(high, right_high_weights))) *
                            scale;
                        output_buffer[frame * 2] = left;
                        output_buffer[frame * 2 + 1] = right;
                        any_nonzero |= left != 0.0f || right != 0.0f;
                    }
                }
            } else if (is_float) {
                const float* src = static_cast<const float*>(ptr);
                const size_t samples =
                    static_cast<size_t>(buffer_frames) * output_channels;
                const float32x4_t scale = vdupq_n_f32(current_gain);
                size_t index = 0;
                for (; index + 4 <= samples; index += 4) {
                    const float32x4_t output =
                        vmulq_f32(vld1q_f32(src + index), scale);
                    vst1q_f32(output_buffer.data() + index, output);
                    const uint32x4_t zero = vceqq_f32(
                        output, vdupq_n_f32(0.0f));
                    any_nonzero |=
                        vminvq_u32(zero) != std::numeric_limits<u32>::max();
                }
                for (; index < samples; ++index) {
                    const float output = src[index] * current_gain;
                    output_buffer[index] = output;
                    any_nonzero |= output != 0.0f;
                }
            } else {
                const s16* src = static_cast<const s16*>(ptr);
                const size_t samples =
                    static_cast<size_t>(buffer_frames) * output_channels;
                const float32x4_t scale =
                    vdupq_n_f32(INV_VOLUME_0DB * current_gain);
                size_t index = 0;
                for (; index + 8 <= samples; index += 8) {
                    const int16x8_t input = vld1q_s16(src + index);
                    const float32x4_t low = vmulq_f32(
                        vcvtq_f32_s32(vmovl_s16(vget_low_s16(input))),
                        scale);
                    const float32x4_t high = vmulq_f32(
                        vcvtq_f32_s32(vmovl_s16(vget_high_s16(input))),
                        scale);
                    vst1q_f32(output_buffer.data() + index, low);
                    vst1q_f32(output_buffer.data() + index + 4, high);
                    const uint32x4_t low_zero =
                        vceqq_f32(low, vdupq_n_f32(0.0f));
                    const uint32x4_t high_zero =
                        vceqq_f32(high, vdupq_n_f32(0.0f));
                    any_nonzero |=
                        vminvq_u32(vandq_u32(low_zero, high_zero)) !=
                        std::numeric_limits<u32>::max();
                }
                for (; index < samples; ++index) {
                    const float output =
                        static_cast<float>(src[index]) *
                        INV_VOLUME_0DB * current_gain;
                    output_buffer[index] = output;
                    any_nonzero |= output != 0.0f;
                }
            }
        }
#endif

        if (used_simd) {
            if (!simd_guard_completed && any_nonzero) {
                if (!ValidateSimdOutput(ptr, current_gain)) {
                    simd_guard_failed = true;
                    used_simd = false;
                    any_nonzero = false;
                    AndroidAudioLog(
                        "[EXECUTOR_AUDIO_SIMD_GUARD] result=fallback guestCh=%u "
                        "outCh=%u float=%u frames=%u",
                        guest_channels, output_channels, is_float ? 1u : 0u,
                        buffer_frames);
                } else {
                    simd_guard_completed = true;
                    AndroidAudioLog(
                        "[EXECUTOR_AUDIO_SIMD_GUARD] result=pass guestCh=%u "
                        "outCh=%u float=%u frames=%u",
                        guest_channels, output_channels, is_float ? 1u : 0u,
                        buffer_frames);
                }
            }
        }

        if (used_simd) {
            g_audio_conversion_calls.fetch_add(1, std::memory_order_relaxed);
            g_audio_converted_frames.fetch_add(
                buffer_frames, std::memory_order_relaxed);
            g_audio_simd_frames.fetch_add(
                buffer_frames, std::memory_order_relaxed);
            g_audio_conversion_nanoseconds.fetch_add(
                ElapsedNanoseconds(conversion_start),
                std::memory_order_relaxed);
            return any_nonzero;
        }

        if (guest_channels == 8) {
            constexpr float CenterMix = 0.70710678f;
            constexpr float SurroundMix = 0.70710678f;
            if (is_float) {
                const float* src = static_cast<const float*>(ptr);
                for (u32 frame = 0; frame < buffer_frames; frame++) {
                    const u32 src_base = frame * 8;
                    const u32 dst_base = frame * 2;
                    const float center = src[src_base + FC] * CenterMix;
                    output_buffer[dst_base] =
                        (src[src_base + FL] + center +
                         (src[src_base + SL] + src[src_base + BL]) * SurroundMix) *
                        current_gain;
                    output_buffer[dst_base + 1] =
                        (src[src_base + FR] + center +
                         (src[src_base + SR] + src[src_base + BR]) * SurroundMix) *
                        current_gain;
                    any_nonzero |= output_buffer[dst_base] != 0.0f ||
                                   output_buffer[dst_base + 1] != 0.0f;
                }
            } else {
                const s16* src = static_cast<const s16*>(ptr);
                for (u32 frame = 0; frame < buffer_frames; frame++) {
                    const u32 src_base = frame * 8;
                    const u32 dst_base = frame * 2;
                    const float center =
                        static_cast<float>(src[src_base + FC]) * CenterMix;
                    output_buffer[dst_base] =
                        (static_cast<float>(src[src_base + FL]) + center +
                         static_cast<float>(
                             src[src_base + SL] + src[src_base + BL]) *
                             SurroundMix) *
                        INV_VOLUME_0DB * current_gain;
                    output_buffer[dst_base + 1] =
                        (static_cast<float>(src[src_base + FR]) + center +
                         static_cast<float>(
                             src[src_base + SR] + src[src_base + BR]) *
                             SurroundMix) *
                        INV_VOLUME_0DB * current_gain;
                    any_nonzero |= output_buffer[dst_base] != 0.0f ||
                                   output_buffer[dst_base + 1] != 0.0f;
                }
            }
        } else if (is_float) {
            const float* src = static_cast<const float*>(ptr);
            for (u32 frame = 0; frame < buffer_frames; frame++) {
                const u32 src_base = frame * guest_channels;
                const u32 dst_base = frame * output_channels;
                output_buffer[dst_base] = src[src_base] * current_gain;
                if (output_channels == 2) {
                    output_buffer[dst_base + 1] =
                        src[src_base + (guest_channels > 1 ? 1 : 0)] * current_gain;
                }
                any_nonzero |= output_buffer[dst_base] != 0.0f ||
                               (output_channels == 2 &&
                                output_buffer[dst_base + 1] != 0.0f);
            }
        } else {
            const s16* src = static_cast<const s16*>(ptr);
            for (u32 frame = 0; frame < buffer_frames; frame++) {
                const u32 src_base = frame * guest_channels;
                const u32 dst_base = frame * output_channels;
                output_buffer[dst_base] =
                    src[src_base] * INV_VOLUME_0DB * current_gain;
                if (output_channels == 2) {
                    output_buffer[dst_base + 1] =
                        src[src_base + (guest_channels > 1 ? 1 : 0)] *
                        INV_VOLUME_0DB * current_gain;
                }
                any_nonzero |= output_buffer[dst_base] != 0.0f ||
                               (output_channels == 2 &&
                                output_buffer[dst_base + 1] != 0.0f);
            }
        }
        g_audio_conversion_calls.fetch_add(1, std::memory_order_relaxed);
        g_audio_converted_frames.fetch_add(
            buffer_frames, std::memory_order_relaxed);
        g_audio_scalar_frames.fetch_add(
            buffer_frames, std::memory_order_relaxed);
        g_audio_conversion_nanoseconds.fetch_add(
            ElapsedNanoseconds(conversion_start), std::memory_order_relaxed);
        return any_nonzero;
    }
#endif

    bool ConvertToOutput(const void* ptr) {
        const AudioBufferFormat format{
            .sample_type = is_float ? AudioSampleType::Float32 : AudioSampleType::S16,
            .channels = static_cast<u8>(guest_channels),
            .standard_8ch = is_std,
        };
        if (!uniform_channel_gain) {
            return ConvertAudioBufferToFloatPerChannel(ptr, buffer_frames, format, output_channels,
                                                       channel_gains, output_buffer,
                                                       per_channel_conversion_guard)
                .any_nonzero;
        }
        const auto result = ConvertAudioBufferToFloat(ptr, buffer_frames, format, output_channels,
                                                      gain.load(std::memory_order_acquire),
                                                      output_buffer, conversion_guard);
        return result.any_nonzero;
    }

    const u32 buffer_frames;
    const u32 sample_rate;
    const u32 guest_channels;
    const u32 guest_sample_size;
    const u32 output_channels;
    const bool is_float;
    const bool is_std;
    const bool audio_output_disabled;
    std::vector<float> output_buffer;
    std::unique_ptr<AndroidRawPcmCapture> raw_pcm_capture;
    bool silence_dither_phase = false;
    AAudioStream* stream{nullptr};
    std::atomic<AAudioStream*> published_stream{};
    std::atomic<bool> recovery_shutting_down{};
    std::atomic<bool> recovery_pending{};
    std::atomic<aaudio_result_t> recovery_error{AAUDIO_OK};
    std::atomic<u64> callback_producer_heartbeat{};
    std::atomic<u64> callback_consumer_heartbeat{};
    std::binary_semaphore recovery_signal{0};
    std::jthread recovery_thread;
    u64 callback_recovery_generation{};
    u64 callback_recovery_count{};
    u64 callback_recovery_failures{};
    u64 callback_watchdog_stalls{};
    bool callback_transport{};
    u32 callback_fifo_capacity_frames{};
    u32 callback_prime_frames{};
    std::vector<float> callback_fifo;
    std::atomic<u64> callback_read_frame{};
    std::atomic<u64> callback_write_frame{};
    std::atomic<u64> callback_preroll_frames{};
    std::atomic<u64> callback_underflow_frames{};
    std::atomic<u64> callback_overflow_blocks{};
    std::atomic<bool> callback_primed{};
    std::atomic<float> gain{Config::getVolumeSlider() * 0.01f};
    std::array<float, 8> channel_gains{};
    bool uniform_channel_gain{true};
    std::atomic_int output_log_budget{4};
    std::atomic_int content_log_budget{4};
    std::atomic_int raw_probe_budget{4};
    std::atomic<u32> raw_probe_sequence{};
    u64 raw_previous_hash{};
    u64 raw_two_back_hash{};
    u64 post_previous_hash{};
    u64 post_two_back_hash{};
    bool diagnostic_probe_active{};
    bool diagnostic_probe_exhausted{};
    u32 output_call_count{};
    int32_t last_xrun_count{};
    AudioConversionGuard conversion_guard{};
    AudioConversionGuard normalized_conversion_guard{};
    AudioConversionGuard per_channel_conversion_guard{};
    AudioConversionGuard normalized_per_channel_conversion_guard{};
};
#endif

std::unique_ptr<PortBackend> SDLAudioOut::Open(PortOut& port) {
#ifdef __ANDROID__
    port.strict_output_credit = AndroidAudioCallbackTransportRequested() &&
                                AndroidAudioConsumedCreditRequested();
    if (port.strict_output_credit) {
        AndroidAudioLog("[EXECUTOR_AUDIO_CREDIT] policy=consume-complete");
    }
    if (AndroidSdlAudioRequested()) {
        static std::atomic_flag logged = ATOMIC_FLAG_INIT;
        if (!logged.test_and_set(std::memory_order_relaxed)) {
            AndroidAudioLog("[EXECUTOR_AUDIO_BACKEND] requested=sdl effective=sdl");
        }
        return std::make_unique<SDLPortBackend>(port);
    }
    const bool audio_out_disabled = AndroidAudioOutputDisabledRequested();
    auto android_backend = std::make_unique<AndroidAAudioPortBackend>(port);
    if (android_backend->IsReady() || audio_out_disabled) {
        return android_backend;
    }
    AndroidAudioLog("[EXECUTOR_AUDIO_AAUDIO_OPEN] falling back to SDL");
#endif
    return std::make_unique<SDLPortBackend>(port);
}

} // namespace Libraries::AudioOut
