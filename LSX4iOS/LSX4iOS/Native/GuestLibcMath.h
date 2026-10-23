// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "executor/dynamic_translation/machine_state.h"
#include <string_view>
namespace Lsx4::Apple {
bool IsLibcMath(std::string_view identity);
bool ExecuteLibcMath(std::string_view identity,Translation::CpuFrame& frame);
}
