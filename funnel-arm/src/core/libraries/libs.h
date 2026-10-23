// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/loader/elf.h"
#include "core/loader/symbols_resolver.h"
#include "core/tls.h"
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <cstddef>
#include <type_traits>
#include "core/aerolib/hle_abi_overrides.h"

extern "C" void executor_lsx4_android_register_guest_readable_range(
    const void* base, std::size_t size, const char* label);

namespace Core::Loader {
// LIB_OBJ explicitly publishes process-lifetime host storage to guest code.
// Preserve its typed extent so checked/semantic reads admit exactly that object,
// just as direct native loads do. This registry is not a read-only permission.
template <typename Pointer>
auto RegisterKnownHleObject(Pointer&& object, const char* nid) {
    using ObjectPointer = std::remove_cvref_t<Pointer>;
    static_assert(std::is_pointer_v<ObjectPointer>,
                  "LIB_OBJ needs a typed pointer; use &array to export a complete array");
    using Object = std::remove_pointer_t<ObjectPointer>;
    static_assert(std::is_object_v<Object>, "LIB_OBJ needs a complete object pointee");
    if constexpr (std::is_pointer_v<ObjectPointer> && std::is_object_v<Object>) {
        executor_lsx4_android_register_guest_readable_range(object, sizeof(Object), nid);
    }
    return object;
}
}

#define LSX4_REGISTER_TYPED_HLE_OBJECT(nid, object) \
    Core::Loader::RegisterKnownHleObject((object), (nid))
#define LSX4_REGISTER_TYPED_HLE(nid, address, function)                                             \
    Core::AeroLib::RegisterAndroidX64TypedHleTarget(                                               \
        nid, address, &Core::HostCallWrapperImpl<decltype(&(function)), function>::wrap)
#define LSX4_REGISTER_RAW_HLE(nid, address, function, invoker)                                      \
    Core::AeroLib::RegisterAndroidX64RawHleTarget<                                                  \
        decltype(&Core::HostCallWrapperImpl<decltype(&(function)), function>::wrap), invoker>(      \
        nid, address, &Core::HostCallWrapperImpl<decltype(&(function)), function>::wrap)
#else
#define LSX4_REGISTER_TYPED_HLE_OBJECT(nid, object) (object)
#define LSX4_REGISTER_TYPED_HLE(nid, address, function) ((void)0)
#define LSX4_REGISTER_RAW_HLE(nid, address, function, invoker) ((void)0)
#endif

#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    {                                                                                              \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Function;                                              \
        auto func = reinterpret_cast<u64>(HOST_CALL(function));                                    \
        LSX4_REGISTER_TYPED_HLE(nid, func, function);                                               \
        sym->AddSymbol(sr, func);                                                                  \
    }

#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    {                                                                                              \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Object;                                                \
        const auto object_address = LSX4_REGISTER_TYPED_HLE_OBJECT(nid, obj);                       \
        sym->AddSymbol(sr, reinterpret_cast<u64>(object_address));                                 \
    }

#define LIB_FUNCTION_RAW(nid, lib, libversion, mod, function, invoker)                             \
    {                                                                                              \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Function;                                              \
        auto func = reinterpret_cast<u64>(HOST_CALL(function));                                    \
        LSX4_REGISTER_RAW_HLE(nid, func, function, invoker);                                       \
        sym->AddSymbol(sr, func);                                                                  \
    }

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym);

}
