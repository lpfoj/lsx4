// SPDX-License-Identifier: MIT
/*
$info$
tags: glue|thunks
$end_info$
*/

#pragma once

#include <cstdint>

namespace FEXCore::IR {
struct SHA256Sum;
}

namespace FEXCore {
namespace Core {
struct CpuStateFrame;
}

typedef void ThunkedFunction(void* ArgsRv);

enum class ThunkControlResult : uint64_t {
  Continue = 0,
  ResumeContext = 1,
};

using ContextThunkedFunction = ThunkControlResult(void* ArgsRv, Core::CpuStateFrame* Frame);

class ThunkHandler {
public:
  virtual ~ThunkHandler() = default;
  virtual ThunkedFunction* LookupThunk(const IR::SHA256Sum& sha256) = 0;
  virtual ContextThunkedFunction* LookupContextThunk(const IR::SHA256Sum&) {
    return nullptr;
  }
};
} // namespace FEXCore
