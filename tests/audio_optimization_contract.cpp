// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "core/libraries/audio/audio_mix.h"

using namespace Libraries::AudioOut;

namespace {

bool Near(const float lhs, const float rhs) {
  return std::abs(lhs - rhs) <= 1.0e-5f * std::max(1.0f, std::abs(rhs));
}

bool CheckChannelContracts() {
  std::array<s16, 8> surround{};
  std::array<float, 2> stereo{};
  std::array<float, 8> canonical{};
  AudioConversionGuard guard{};

  surround[4] = std::numeric_limits<s16>::max();
  auto result = ConvertAudioBufferToFloat(surround.data(), 1,
                                          {.sample_type = AudioSampleType::S16,
                                           .channels = 8,
                                           .standard_8ch = false},
                                          2, 1.0f, stereo, guard);
  if (!result.any_nonzero || !Near(stereo[0], 0.7070852f) ||
      !Near(stereo[1], 0.0f)) {
    return false;
  }

  // Standard Orbis order places semantic SL at physical lane 6.
  surround.fill(0);
  surround[6] = std::numeric_limits<s16>::max();
  guard = {};
  result = ConvertAudioBufferToFloat(surround.data(), 1,
                                     {.sample_type = AudioSampleType::S16,
                                      .channels = 8,
                                      .standard_8ch = true},
                                     2, 1.0f, stereo, guard);
  if (!result.any_nonzero || !Near(stereo[0], 0.7070852f) ||
      !Near(stereo[1], 0.0f)) {
    return false;
  }

  guard = {};
  result = ConvertAudioBufferToFloat(surround.data(), 1,
                                     {.sample_type = AudioSampleType::S16,
                                      .channels = 8,
                                      .standard_8ch = true},
                                     8, 1.0f, canonical, guard);
  return result.any_nonzero && Near(canonical[4], 32767.0f / 32768.0f) &&
         Near(canonical[6], 0.0f);
}

bool CheckImmediateToggle() {
  constexpr u32 Frames = 256;
  std::array<s16, Frames * 2> source{};
  std::array<float, Frames * 2> output{};
  for (size_t index = 0; index < source.size(); ++index) {
    source[index] = static_cast<s16>(static_cast<s32>(index % 30001u) - 15000);
  }
  const AudioBufferFormat format{
      .sample_type = AudioSampleType::S16,
      .channels = 2,
  };
  AudioConversionGuard guard{};

  SetManagedAudioSimdEnabled(true);
  const auto enabled = ConvertAudioBufferToFloat(source.data(), Frames, format,
                                                 2, 1.0f, output, guard);
#if defined(__aarch64__) || defined(_M_ARM64)
  if (!enabled.used_simd) {
    return false;
  }
#endif
  const u64 enabled_generation = guard.generation;

  SetManagedAudioSimdEnabled(false);
  const auto disabled = ConvertAudioBufferToFloat(source.data(), Frames, format,
                                                  2, 1.0f, output, guard);
  return !disabled.used_simd && guard.generation != enabled_generation;
}

bool CheckPerChannelVolumeBeforeStdDownmix() {
  // Std physical order is FL/FR/FC/LFE/BL/BR/SL/SR. The gain array is
  // canonical/semantic, matching sceAudioOutSetVolume channel flags.
  const std::array<float, 8> source{0.25f, -0.5f, 0.75f, 0.875f,
                                    -0.625f, 0.5f, 0.375f, -0.25f};
  const std::array<float, 8> gains{1.0f, 1.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 2> stereo{};
  AudioConversionGuard guard{};
  SetManagedAudioSimdEnabled(true);
  const auto result = ConvertAudioBufferToFloatPerChannel(
      source.data(), 1,
      {.sample_type = AudioSampleType::Float32,
       .channels = 8,
       .standard_8ch = true},
      2, gains, stereo, guard);
#if defined(__aarch64__) || defined(_M_ARM64)
  if (!result.used_simd || !result.differential_guard_passed) {
    return false;
  }
#else
  if (result.used_simd) {
    return false;
  }
#endif
  if (!result.any_nonzero || !Near(stereo[0], source[0]) ||
      !Near(stereo[1], source[1])) {
    return false;
  }

  // Non-Std uses physical SL/SR at lanes 4/5; independent semantic gains
  // must remain attached to the same speakers in both layout variants.
  const std::array<float, 8> non_std_source{0.0f, 0.0f, 0.0f, 0.0f,
                                            0.5f, -0.25f, 0.0f, 0.0f};
  const std::array<float, 8> surround_gains{0.0f, 0.0f, 0.0f, 0.0f,
                                            0.25f, 0.5f, 0.0f, 0.0f};
  guard = {};
  const auto non_std = ConvertAudioBufferToFloatPerChannel(
      non_std_source.data(), 1,
      {.sample_type = AudioSampleType::Float32,
       .channels = 8,
       .standard_8ch = false},
      2, surround_gains, stereo, guard);
  return non_std.any_nonzero && Near(stereo[0], 0.5f * 0.25f * 0.70710678f) &&
         Near(stereo[1], -0.25f * 0.5f * 0.70710678f);
}

bool CheckCrossFormatMix() {
  constexpr u32 Frames = 256;
  std::array<s16, Frames * 8> main_surround{};
  std::array<float, Frames * 2> personal_stereo{};
  std::array<float, Frames * 2> normalized_main{};
  std::array<float, Frames * 2> normalized_personal{};
  for (u32 frame = 0; frame < Frames; ++frame) {
    main_surround[frame * 8] = 8192;
    main_surround[frame * 8 + 1] = -8192;
    personal_stereo[frame * 2] = 0.25f;
    personal_stereo[frame * 2 + 1] = 0.5f;
  }

  AudioConversionGuard main_guard{};
  AudioConversionGuard personal_guard{};
  AudioConversionGuard mix_guard{};
  (void)ConvertAudioBufferToFloat(main_surround.data(), Frames,
                                  {.sample_type = AudioSampleType::S16,
                                   .channels = 8,
                                   .standard_8ch = false},
                                  2, 1.0f, normalized_main, main_guard);
  (void)ConvertAudioBufferToFloat(personal_stereo.data(), Frames,
                                  {.sample_type = AudioSampleType::Float32,
                                   .channels = 2,
                                   .standard_8ch = false},
                                  2, 0.5f, normalized_personal, personal_guard);
  (void)MixAudioBufferInPlace(normalized_main.data(),
                              normalized_personal.data(), Frames,
                              {.sample_type = AudioSampleType::Float32,
                               .channels = 2,
                               .standard_8ch = false},
                              1.0f, mix_guard);

  return Near(normalized_main[0], 0.375f) && Near(normalized_main[1], 0.0f);
}

bool CheckStreamContracts() {
  AudioStreamGuard valid{};
  ObserveAudioStreamSubmit(valid, 1, 100);
  const auto first = ObserveAudioStreamConsume(valid, 1, 100, 150, 100);
  ObserveAudioStreamSubmit(valid, 2, 200);
  const auto second = ObserveAudioStreamConsume(valid, 2, 200, 250, 100);
  if (!first.continuity_ok || !first.timestamp_order_ok || first.underrun ||
      !second.continuity_ok || !second.timestamp_order_ok || second.underrun) {
    return false;
  }

  AudioStreamGuard broken_order{};
  ObserveAudioStreamSubmit(broken_order, 1, 100);
  const auto order = ObserveAudioStreamConsume(broken_order, 2, 100, 90, 100);
  if (order.continuity_ok || order.timestamp_order_ok) {
    return false;
  }

  AudioStreamGuard late{};
  ObserveAudioStreamSubmit(late, 1, 100);
  const auto underrun = ObserveAudioStreamConsume(late, 1, 100, 401, 100);
  return underrun.continuity_ok && underrun.timestamp_order_ok &&
         underrun.underrun;
}

} // namespace

int main() {
  ResetAudioOptimizationStats();
  SetManagedAudioSimdEnabled(true);
  if (!RunAudioOptimizationRegressionGuards()) {
    return 1;
  }
  if (!CheckChannelContracts()) {
    return 2;
  }
  if (!CheckImmediateToggle()) {
    return 3;
  }
  if (!CheckPerChannelVolumeBeforeStdDownmix()) {
    return 4;
  }
  SetManagedAudioSimdEnabled(true);
  if (!CheckCrossFormatMix()) {
    return 5;
  }
  if (!CheckStreamContracts()) {
    return 6;
  }
  const auto stats = GetAudioOptimizationStats();
  if (stats.regression_guard_passes == 0 || stats.channel_guard_failures != 0 ||
      stats.continuity_failures == 0 || stats.timestamp_order_failures == 0 ||
      stats.scheduler_underruns == 0 || stats.toggle_generation < 3) {
    return 7;
  }
#if defined(__ANDROID__)
  // Android production runs keep pure conversion timing/counting off unless
  // the explicit app-private diagnostic marker is present. Functional guard,
  // continuity and error counters above remain live.
  if (stats.conversion_calls != 0 || stats.converted_frames != 0 ||
      stats.simd_frames != 0 || stats.scalar_frames != 0 ||
      stats.conversion_nanoseconds != 0) {
    return 8;
  }
#else
  if (stats.conversion_calls == 0 || stats.converted_frames == 0) {
    return 8;
  }
#endif
  return 0;
}
