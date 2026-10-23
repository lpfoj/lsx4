// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Piglet {

void RegisterLib(Core::Loader::SymbolsResolver* sym);
bool PrewarmAndroidPigletDlsymHleTargets(std::size_t* materialized_count);
void* ResolveSymbolByName(const char* name);
void SetGuestSwapExpected(bool enabled, const char* reason);

}
