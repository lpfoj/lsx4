// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/vector_semantics.h"
#include <optional>
#include <stdexcept>
namespace Lsx4::Apple {
struct GuestSIMDFault : std::runtime_error {GuestSIMDFault():std::runtime_error("Unmasked guest SIMD floating-point exception (#XM)") {}};
std::optional<Translation::VectorSemanticResult> ExecuteFloatConversion(const Translation::Instruction& instruction,
    Translation::CpuFrame& frame,const Translation::GuestMemoryPort& memory);
}
