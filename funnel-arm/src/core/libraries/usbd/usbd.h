// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct timeval;

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Usbd {

// Android does not expose host USB devices directly to the native runtime. The
// no-device backend still models the libSceUsbd event-loop contract so games do
// not fall through to the generic success stub and spin while polling it.
s32 PS4_SYSV_ABI sceUsbdInit();
void PS4_SYSV_ABI sceUsbdExit();
s64 PS4_SYSV_ABI sceUsbdGetDeviceList(void*** list);
void PS4_SYSV_ABI sceUsbdFreeDeviceList(void** list, s32 unref_devices);
s32 PS4_SYSV_ABI sceUsbdHandleEvents();
s32 PS4_SYSV_ABI sceUsbdHandleEventsLocked(timeval* timeout);
s32 PS4_SYSV_ABI sceUsbdHandleEventsTimeout(timeval* timeout);
s32 PS4_SYSV_ABI sceUsbdEventHandlerActive();
s32 PS4_SYSV_ABI sceUsbdEventHandlingOk();
void PS4_SYSV_ABI sceUsbdLockEvents();
s32 PS4_SYSV_ABI sceUsbdTryLockEvents();
void PS4_SYSV_ABI sceUsbdUnlockEvents();
void PS4_SYSV_ABI sceUsbdLockEventWaiters();
void PS4_SYSV_ABI sceUsbdUnlockEventWaiters();
s32 PS4_SYSV_ABI sceUsbdWaitForEvent(timeval* timeout);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Usbd
