// SPDX-License-Identifier: GPL-2.0-or-later
// The Android SwanStation .so cannot be loaded on iOS. Preserve package
// detection and report the same unavailable-core result as Android LoadCore.
// No PS1 frames or successful launches are manufactured by this backend.
#include "executor/ps1_classic_runtime.h"
#include "executor/ps1_classic_package.h"
#include "android/log.h"
#include <atomic>
namespace Executor::Ps1Classic {
namespace { std::atomic<bool> paused{}; }
bool IsPackage(const std::filesystem::path& path) { return Detail::IsPackageLayout(path); }
int Launch(const std::filesystem::path& path,const std::filesystem::path&) {
    if(!IsPackage(path)) return -1;
    __android_log_print(ANDROID_LOG_ERROR,"LSX4-PS1",
        "PS1 launch rejected: an Apple SwanStation backend has not been linked");
    return -3;
}
bool IsActive() { return false; }
void Stop() {}
void SetPaused(bool value) { paused.store(value); }
bool IsPaused() { return paused.load(); }
void AttachSurface(void*,int,int) {}
void SetButton(std::uint32_t,bool) {}
void SetAxis(int,int) {}
void SetTouchpad(bool,float,float) {}
std::uint64_t PresentCount() { return 0; }
}
