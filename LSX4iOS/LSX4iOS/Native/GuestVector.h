// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/vector_semantics.h"
namespace Lsx4::Apple {
Translation::VectorSemanticResult ExecuteGuestVector(const Translation::Instruction& instruction,
    Translation::CpuFrame& frame,const Translation::GuestMemoryPort& memory);
}
