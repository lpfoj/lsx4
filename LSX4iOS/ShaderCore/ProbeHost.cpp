// SPDX-License-Identifier: GPL-2.0-or-later
// Host policy for the bounded Apple shader routes and standalone probes.
// Callers validate guest code and resources before translation. No replay,
// captures, direct guest pointers, or diagnostic shader substitutions.
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include <cstdio>
#include <stdexcept>
void assert_fail_impl(const char* expression,const char* file,int line) {
    throw std::runtime_error(std::string(file)+":"+std::to_string(line)+": "+expression);
}
[[noreturn]] void unreachable_impl(){throw std::runtime_error("Unreachable shader compiler path");}
namespace Common::Log {
void FmtLogMessageImpl(Class,Level,const char*,unsigned int,const char*,const char* format,const fmt::format_args& args) {
    const auto message=fmt::vformat(format,args);std::fprintf(stderr,"%s\n",message.c_str());
}
}
namespace Config {
bool dumpShaders(){return false;}
bool directMemoryAccess(){return false;}
}
namespace Common::FS {
std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};
}
const std::filesystem::path& GetUserPath(PathType){throw std::runtime_error("Shader dump path is not configured for this probe");}
}
namespace Libraries::GnmDriver {
bool ExecutorReplayActive(){return false;}
bool ExecutorReplayIsRelocated(u64){return false;}
u64 ExecutorReplayRelocate(u64 address){return address;}
bool ExecutorGnmCaptureActive(){return false;}
bool ExecutorReplayForcePsConst(){return false;}
bool ExecutorDiagnosticForcePsConstSolid(){return false;}
void ExecutorGnmCaptureRecordResource(u32,u64,u64){throw std::runtime_error("Capture called while disabled");}
}
namespace VideoCore {bool IsManagedMobileGpuOptimization(){return false;}}
