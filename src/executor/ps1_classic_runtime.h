#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace Executor::Ps1Classic {

bool IsPackage(const std::filesystem::path& executable);
int Launch(const std::filesystem::path& executable,
           const std::filesystem::path& runtime_root);
void Stop();
bool IsActive();
void SetPaused(bool paused);
bool IsPaused();
void AttachSurface(void* native_window, int width, int height);
void SetButton(std::uint32_t orbis_mask, bool pressed);
void SetAxis(int axis, int value);
void SetTouchpad(bool pressed, float x, float y);
std::uint64_t PresentCount();

}
