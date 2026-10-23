// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Executor::AndroidPerformanceHint {

// Selects the display-rate 60 Hz ADPF workload target. The native launch
// policy enables it by default; an explicit diagnostic marker can still
// select the relaxed 30 Hz / 33.333 ms target.
void SetFullRate60HzTarget(bool enabled) noexcept;

void SetEnabled(bool enabled) noexcept;
[[nodiscard]] bool IsEnabled() noexcept;

// Keeps the ADPF session's Android thread membership current without
// reporting a work duration. This is safe to call for retained presents.
void MaintainSessionThreads() noexcept;

// Marks the boundary between complete guest producer cycles. The Android
// implementation measures consecutive boundaries from the same calling TID.
void ReportProducerCycleComplete() noexcept;

// Compatibility entry point for the PS5 path until it has an equivalent
// producer-cycle boundary. The PS4 renderer must not report Present cadence.
void ReportFramePresented() noexcept;

} // namespace Executor::AndroidPerformanceHint
