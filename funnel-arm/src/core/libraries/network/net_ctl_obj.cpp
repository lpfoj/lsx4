// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/config.h"
#include "common/logging/log.h"
#include "core/libraries/guest_callback.h"
#include "core/libraries/network/net_ctl_codes.h"
#include "core/libraries/network/net_ctl_obj.h"
#include "core/tls.h"

namespace Libraries::NetCtl {

NetCtlInternal::NetCtlInternal() = default;

NetCtlInternal::~NetCtlInternal() = default;

s32 NetCtlInternal::RegisterCallback(OrbisNetCtlCallback func, void* arg) {
    std::scoped_lock lock{m_mutex};

    const auto it = std::ranges::find(callbacks, nullptr, &NetCtlCallback::func);
    if (it == callbacks.end()) {
        return ORBIS_NET_CTL_ERROR_CALLBACK_MAX;
    }

    const int next_id = std::distance(callbacks.begin(), it);
    callbacks[next_id].func = func;
    callbacks[next_id].arg = arg;
    callback_pending[next_id] = true;
    return next_id;
}

s32 NetCtlInternal::RegisterNpToolkitCallback(OrbisNetCtlCallbackForNpToolkit func, void* arg) {
    std::scoped_lock lock{m_mutex};

    const auto it = std::ranges::find(nptool_callbacks, nullptr, &NetCtlCallbackForNpToolkit::func);
    if (it == nptool_callbacks.end()) {
        return ORBIS_NET_CTL_ERROR_CALLBACK_MAX;
    }

    const int next_id = std::distance(nptool_callbacks.begin(), it);
    nptool_callbacks[next_id].func = func;
    nptool_callbacks[next_id].arg = arg;
    nptool_callback_pending[next_id] = true;
    return next_id;
}

static void InvokeNetCtlCallback(const OrbisNetCtlCallback func, const int event, void* arg,
                                 const char* reason) {
    const int bridge = Libraries::RunGuestCallback4(
        reinterpret_cast<void*>(func), static_cast<u64>(event), reinterpret_cast<u64>(arg), 0, 0,
        2, nullptr, reason);
    if (bridge < 0) {
        LOG_ERROR(Lib_NetCtl, "Guest NetCtl callback failed: reason = {}, callback = {}, rc = {}",
                  reason, reinterpret_cast<void*>(func), bridge);
    } else if (bridge == 0) {
        func(event, arg);
    }
}

void NetCtlInternal::CheckCallback() {
    std::array<NetCtlCallback, 8> pending{};
    const bool connected = Config::getIsConnectedToNetwork();
    {
        std::scoped_lock lock{m_mutex};
        if (connection_state_initialized && connected != last_connected) {
            for (size_t i = 0; i < callbacks.size(); ++i) {
                callback_pending[i] = callbacks[i].func != nullptr;
                nptool_callback_pending[i] = nptool_callbacks[i].func != nullptr;
            }
        }
        connection_state_initialized = true;
        last_connected = connected;
        for (size_t i = 0; i < callbacks.size(); ++i) {
            if (callback_pending[i] && callbacks[i].func != nullptr) {
                pending[i] = callbacks[i];
            }
            callback_pending[i] = false;
        }
    }

    const auto event = connected ? ORBIS_NET_CTL_EVENT_TYPE_IPOBTAINED
                                 : ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED;
    for (const auto [func, arg] : pending) {
        if (func != nullptr) {
            InvokeNetCtlCallback(func, event, arg, "netctl_state");
        }
    }
}

void NetCtlInternal::CheckNpToolkitCallback() {
    std::array<NetCtlCallbackForNpToolkit, 8> pending{};
    const bool connected = Config::getIsConnectedToNetwork();
    {
        std::scoped_lock lock{m_mutex};
        if (connection_state_initialized && connected != last_connected) {
            for (size_t i = 0; i < callbacks.size(); ++i) {
                callback_pending[i] = callbacks[i].func != nullptr;
                nptool_callback_pending[i] = nptool_callbacks[i].func != nullptr;
            }
        }
        connection_state_initialized = true;
        last_connected = connected;
        for (size_t i = 0; i < nptool_callbacks.size(); ++i) {
            if (nptool_callback_pending[i] && nptool_callbacks[i].func != nullptr) {
                pending[i] = nptool_callbacks[i];
            }
            nptool_callback_pending[i] = false;
        }
    }

    const auto event = connected ? ORBIS_NET_CTL_EVENT_TYPE_IPOBTAINED
                                 : ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED;
    for (const auto [func, arg] : pending) {
        if (func != nullptr) {
            InvokeNetCtlCallback(func, event, arg, "netctl_np_toolkit_state");
        }
    }
}

}
