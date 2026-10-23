// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdexcept>
#include "executor/dynamic_translation/scalar_semantics.h"
namespace Lsx4::Apple {
struct GuestDivideError : std::runtime_error {GuestDivideError():std::runtime_error("Guest x86 divide error (#DE)") {}};
Translation::SemanticResult ExecuteGuestScalar(const Translation::Instruction& instruction,
    Translation::CpuFrame& frame,const Translation::GuestMemoryPort& memory);
}
