// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "common/types.h"

namespace Core::AeroLib {

extern "C" bool ExecutorJitReadGuestBytes(std::uint64_t address, void* destination,
                                            std::size_t size) __attribute__((weak));
extern "C" bool ExecutorJitWriteGuestBytes(std::uint64_t address, const void* source,
                                             std::size_t size) __attribute__((weak));

enum class AndroidX64SysVClass : u8 {
    NoClass = 0,
    Integer = 1,
    Sse = 2,
    SseUp = 3,
    X87 = 4,
    X87Up = 5,
    ComplexX87 = 6,
    Memory = 7,
    Unsupported = 0xff,
};

inline constexpr u8 AndroidX64SretPresent = 1U << 0;
inline constexpr u8 AndroidX64SretRaxReturnsAddress = 1U << 1;
inline constexpr u8 AndroidX64SretHostWroteMemory = 1U << 2;
inline constexpr u32 AndroidX64CallVariadic = 1U << 0;
inline constexpr u32 AndroidX64CallNoReturn = 1U << 1;

struct AndroidX64HleXmm128 {
    u64 low{};
    u64 high{};
};

struct AndroidX64HleRawCall {
    u64 native_function{};
    u64 guest_rip{};
    u64 guest_rsp{};
    u64 input_rax{};
    std::array<u64, 6> gpr{};
    std::array<AndroidX64HleXmm128, 8> xmm{};
    u32 call_flags{};
    u32 sysv_sse_arg_count{};
};

struct AndroidX64HleRawResult {
    u64 rax{};
    u64 rdx{};
    AndroidX64HleXmm128 xmm0{};
    AndroidX64HleXmm128 xmm1{};
    u64 sret_address{};
    u32 sret_size{};
    std::array<u8, 2> return_class{};
    u8 return_eightbyte_count{};
    u8 sret_flags{};
    u64 abi_descriptor_id{};
};

struct AndroidX64HleArgumentAbi {
    u16 size{};
    u16 alignment{};
    std::array<u8, 2> classes{};
    u8 eightbyte_count{};
    u8 reserved{};
};

using AndroidX64HleInvokeFn = bool (*)(u64 native_function,
                                       const AndroidX64HleRawCall& call,
                                       AndroidX64HleRawResult& result);

struct AndroidX64HleAbiDescriptor {
    u64 descriptor_id{};
    const char* signature{};
    AndroidX64HleInvokeFn invoke{};
    std::array<AndroidX64HleArgumentAbi, 32> arguments{};
    u32 argument_count{};
    u32 stack_argument_bytes{};
    u32 call_flags{};
    u8 gpr_argument_count{};
    u8 sse_argument_count{};
    std::array<u8, 2> return_class{};
    u8 return_eightbyte_count{};
    u8 sret_flags{};
    u16 return_size{};
    bool supported{};
};

// Small SysV aggregates need their two eightbyte classes described explicitly:
// C++ has no portable member reflection from which INTEGER/SSE can be derived.
// Unknown aggregates remain fail-closed instead of being guessed as INTEGER.
template <typename T>
struct AndroidX64HleTypeAbiOverride {
    static constexpr bool enabled = false;
    static constexpr bool memory = false;
    static constexpr u8 count = 0;
    static constexpr std::array<AndroidX64SysVClass, 2> classes{
        AndroidX64SysVClass::Unsupported, AndroidX64SysVClass::NoClass};
};

void RegisterAndroidX64HleAbiDescriptor(u64 native_function, const char* name,
                                        const AndroidX64HleAbiDescriptor& descriptor);
bool ResolveAndroidX64HleAbiDescriptor(u64 native_function,
                                       AndroidX64HleAbiDescriptor* descriptor);
bool InvokeAndroidX64HleTypedTargetResolved(
    const AndroidX64HleAbiDescriptor& descriptor,
    const AndroidX64HleRawCall& call, AndroidX64HleRawResult* result);
bool InvokeAndroidX64HleTypedTarget(const AndroidX64HleRawCall& call,
                                    AndroidX64HleRawResult* result);
u64 BeginAndroidX64ResolvedHleGeneration();
bool SealAndroidX64ResolvedHleGeneration(std::string* error);
bool ValidateAndroidX64ResolvedHleAbis(std::string* error);
void MarkAndroidX64ResolvedHleTarget(u64 native_function, const char* name);

namespace HleAbiDetail {

constexpr u64 Fnv1aByte(u64 hash, const u8 value) {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

constexpr u64 Fnv1aU16(u64 hash, const u16 value) {
    hash = Fnv1aByte(hash, static_cast<u8>(value));
    return Fnv1aByte(hash, static_cast<u8>(value >> 8));
}

constexpr u64 Fnv1aU32(u64 hash, const u32 value) {
    for (u32 shift = 0; shift != 32; shift += 8) {
        hash = Fnv1aByte(hash, static_cast<u8>(value >> shift));
    }
    return hash;
}

constexpr u64 CanonicalDescriptorId(const AndroidX64HleAbiDescriptor& descriptor) {
    // Versioned wire-layout hash.  Diagnostics may retain the C++ spelling,
    // but interoperability and alias checks must depend only on the SysV ABI.
    u64 hash = UINT64_C(1469598103934665603);
    for (const u8 byte : std::array<u8, 8>{'L', 'S', 'X', '4', 'H', 'L', 'E', 3}) {
        hash = Fnv1aByte(hash, byte);
    }
    hash = Fnv1aU32(hash, descriptor.argument_count);
    hash = Fnv1aU32(hash, descriptor.stack_argument_bytes);
    hash = Fnv1aU32(hash, descriptor.call_flags);
    hash = Fnv1aByte(hash, descriptor.gpr_argument_count);
    hash = Fnv1aByte(hash, descriptor.sse_argument_count);
    hash = Fnv1aByte(hash, descriptor.return_class[0]);
    hash = Fnv1aByte(hash, descriptor.return_class[1]);
    hash = Fnv1aByte(hash, descriptor.return_eightbyte_count);
    hash = Fnv1aByte(hash, descriptor.sret_flags);
    hash = Fnv1aU16(hash, descriptor.return_size);
    for (u32 index = 0; index < descriptor.argument_count &&
                        index < descriptor.arguments.size();
         ++index) {
        const auto& argument = descriptor.arguments[index];
        hash = Fnv1aU16(hash, argument.size);
        hash = Fnv1aU16(hash, argument.alignment);
        hash = Fnv1aByte(hash, argument.classes[0]);
        hash = Fnv1aByte(hash, argument.classes[1]);
        hash = Fnv1aByte(hash, argument.eightbyte_count);
    }
    return hash != 0 ? hash : 1;
}

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr bool CheckedAdd(u64 lhs, std::size_t rhs, u64* result) {
    if (result == nullptr || static_cast<u64>(rhs) > std::numeric_limits<u64>::max() - lhs) {
        return false;
    }
    *result = lhs + static_cast<u64>(rhs);
    return true;
}

template <typename T>
struct TypeAbi {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    using Override = AndroidX64HleTypeAbiOverride<U>;
    static constexpr bool is_reference = std::is_reference_v<T>;
    static constexpr bool is_integer =
        std::is_integral_v<U> || std::is_enum_v<U> || std::is_pointer_v<U>;
    static constexpr bool is_float = std::is_same_v<U, float> || std::is_same_v<U, double>;
    static constexpr bool override_valid = [] {
        if constexpr (!Override::enabled) {
            return true;
        } else if constexpr (!std::is_trivially_copyable_v<U> ||
                             !std::is_default_constructible_v<U> ||
                             sizeof(U) > std::numeric_limits<u16>::max() || alignof(U) > 16) {
            return false;
        } else if constexpr (Override::memory) {
            return Override::count == 1 &&
                   Override::classes[0] == AndroidX64SysVClass::Memory &&
                   Override::classes[1] == AndroidX64SysVClass::NoClass;
        } else {
            constexpr u8 expected_count = static_cast<u8>((sizeof(U) + 7) / 8);
            if constexpr (Override::count == 0 || Override::count > 2 ||
                          Override::count != expected_count) {
                return false;
            }
            constexpr auto first = Override::classes[0];
            constexpr auto second = Override::classes[1];
            constexpr bool first_valid = first == AndroidX64SysVClass::Integer ||
                                         first == AndroidX64SysVClass::Sse;
            if constexpr (!first_valid) {
                return false;
            }
            if constexpr (Override::count == 1) {
                return second == AndroidX64SysVClass::NoClass;
            }
            constexpr bool second_valid = second == AndroidX64SysVClass::Integer ||
                                          second == AndroidX64SysVClass::Sse ||
                                          second == AndroidX64SysVClass::SseUp ||
                                          second == AndroidX64SysVClass::NoClass;
            return second_valid &&
                   (second != AndroidX64SysVClass::SseUp ||
                    first == AndroidX64SysVClass::Sse);
        }
    }();
    static constexpr bool is_large_memory =
        (std::is_class_v<U> || std::is_union_v<U> || std::is_array_v<U>) &&
        sizeof(U) > 16 && std::is_trivially_copyable_v<U> &&
        std::is_default_constructible_v<U>;
    static constexpr bool supported =
        !is_reference && sizeof(U) <= std::numeric_limits<u16>::max() && alignof(U) <= 16 &&
        ((Override::enabled && override_valid) || (is_integer && sizeof(U) <= 16) || is_float ||
         is_large_memory);
    static constexpr bool memory = Override::enabled ? Override::memory : is_large_memory;
    static constexpr u8 count = Override::enabled
                                    ? Override::count
                                    : (memory ? 1 : static_cast<u8>((sizeof(U) + 7) / 8));
    static constexpr std::array<AndroidX64SysVClass, 2> classes = [] {
        std::array<AndroidX64SysVClass, 2> out{
            AndroidX64SysVClass::NoClass, AndroidX64SysVClass::NoClass};
        if constexpr (Override::enabled) {
            out = Override::classes;
        } else if constexpr (!supported) {
            out[0] = AndroidX64SysVClass::Unsupported;
        } else if constexpr (memory) {
            out[0] = AndroidX64SysVClass::Memory;
        } else if constexpr (is_float) {
            out[0] = AndroidX64SysVClass::Sse;
        } else {
            out[0] = AndroidX64SysVClass::Integer;
            if constexpr (sizeof(U) > 8) out[1] = AndroidX64SysVClass::Integer;
        }
        return out;
    }();
};

template <>
struct TypeAbi<void> {
    static constexpr bool supported = true;
    static constexpr bool memory = false;
    static constexpr u8 count = 0;
    static constexpr std::array<AndroidX64SysVClass, 2> classes{
        AndroidX64SysVClass::NoClass, AndroidX64SysVClass::NoClass};
};

struct ArgumentCursor {
    std::size_t gpr{};
    std::size_t sse{};
    std::size_t stack{};
};

template <typename T>
bool LoadArgument(const AndroidX64HleRawCall& call, ArgumentCursor& cursor, T* output) {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    using Abi = TypeAbi<U>;
    if constexpr (!Abi::supported) {
        return false;
    } else {
        constexpr std::size_t size = sizeof(U);
        const std::size_t integer_parts =
            (Abi::classes[0] == AndroidX64SysVClass::Integer ? 1 : 0) +
            (Abi::classes[1] == AndroidX64SysVClass::Integer ? 1 : 0);
        const std::size_t sse_parts =
            (Abi::classes[0] == AndroidX64SysVClass::Sse ? 1 : 0) +
            (Abi::classes[1] == AndroidX64SysVClass::Sse ? 1 : 0);
        const bool registers_fit = !Abi::memory &&
                                   cursor.gpr + integer_parts <= call.gpr.size() &&
                                   cursor.sse + sse_parts <= call.xmm.size();
        std::array<u8, sizeof(U)> bytes{};
        if (registers_fit) {
            std::size_t byte_offset{};
            std::size_t active_sse = cursor.sse;
            for (std::size_t part = 0; part < Abi::count; ++part) {
                const std::size_t part_size =
                    (size - byte_offset) < 8 ? (size - byte_offset) : 8;
                if (Abi::classes[part] == AndroidX64SysVClass::Integer) {
                    std::memcpy(bytes.data() + byte_offset, &call.gpr[cursor.gpr++], part_size);
                } else if (Abi::classes[part] == AndroidX64SysVClass::Sse) {
                    std::memcpy(bytes.data() + byte_offset, &call.xmm[cursor.sse].low, part_size);
                    active_sse = cursor.sse++;
                } else if (Abi::classes[part] == AndroidX64SysVClass::SseUp) {
                    std::memcpy(bytes.data() + byte_offset, &call.xmm[active_sse].high, part_size);
                } else if (Abi::classes[part] == AndroidX64SysVClass::NoClass) {
                    // Padding has no source register and remains zero-initialized.
                } else {
                    return false;
                }
                byte_offset += part_size;
            }
        } else {
            const std::size_t alignment = alignof(U) > 8 ? alignof(U) : 8;
            cursor.stack = AlignUp(cursor.stack, alignment);
            u64 stack_base{};
            u64 stack_address{};
            if (call.guest_rsp == 0 ||
                !CheckedAdd(call.guest_rsp, sizeof(u64), &stack_base) ||
                !CheckedAdd(stack_base, cursor.stack, &stack_address) ||
                ExecutorJitReadGuestBytes == nullptr ||
                !ExecutorJitReadGuestBytes(stack_address, bytes.data(), size)) {
                return false;
            }
            cursor.stack += AlignUp(size, 8);
        }
        std::memcpy(output, bytes.data(), size);
        return true;
    }
}

template <typename R>
bool StoreReturn(const R& value, AndroidX64HleRawResult& result) {
    using U = std::remove_cv_t<R>;
    using Abi = TypeAbi<U>;
    result.return_eightbyte_count = Abi::count;
    result.return_class[0] = static_cast<u8>(Abi::classes[0]);
    result.return_class[1] = static_cast<u8>(Abi::classes[1]);
    if constexpr (Abi::memory) {
        result.sret_size = sizeof(U);
        result.sret_flags = AndroidX64SretPresent |
                            AndroidX64SretRaxReturnsAddress |
                            AndroidX64SretHostWroteMemory;
        if (result.sret_address == 0 || ExecutorJitWriteGuestBytes == nullptr ||
            !ExecutorJitWriteGuestBytes(result.sret_address, &value, sizeof(U))) {
            return false;
        }
        result.rax = result.sret_address;
    } else {
        std::array<u8, sizeof(U)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(U));
        std::size_t byte_offset{};
        std::size_t integer_result{};
        std::size_t sse_result{};
        for (std::size_t part = 0; part < Abi::count; ++part) {
            const std::size_t part_size =
                (sizeof(U) - byte_offset) < 8 ? (sizeof(U) - byte_offset) : 8;
            if (Abi::classes[part] == AndroidX64SysVClass::Integer) {
                u64 word{};
                std::memcpy(&word, bytes.data() + byte_offset, part_size);
                (integer_result++ == 0 ? result.rax : result.rdx) = word;
            } else if (Abi::classes[part] == AndroidX64SysVClass::Sse) {
                auto& target = sse_result++ == 0 ? result.xmm0 : result.xmm1;
                std::memcpy(&target.low, bytes.data() + byte_offset, part_size);
            } else if (Abi::classes[part] == AndroidX64SysVClass::SseUp) {
                auto& target = sse_result <= 1 ? result.xmm0 : result.xmm1;
                std::memcpy(&target.high, bytes.data() + byte_offset, part_size);
            }
            byte_offset += part_size;
        }
    }
    return true;
}

template <typename Fn>
struct FunctionTraits;

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> {
    using Return = R;
    using Arguments = std::tuple<std::remove_cv_t<std::remove_reference_t<Args>>...>;
    static constexpr std::size_t argument_count = sizeof...(Args);

    template <std::size_t... I>
    static bool LoadArguments(const AndroidX64HleRawCall& call, ArgumentCursor& cursor,
                              Arguments& arguments, std::index_sequence<I...>) {
        bool ok = true;
        ((ok = ok && LoadArgument(call, cursor, &std::get<I>(arguments))), ...);
        return ok;
    }

    static bool Invoke(u64 native_function, const AndroidX64HleRawCall& call,
                       AndroidX64HleRawResult& result) {
        if constexpr (argument_count > 32 || !TypeAbi<R>::supported ||
                      !(TypeAbi<Args>::supported && ...)) {
            return false;
        } else {
            ArgumentCursor cursor{};
            if constexpr (TypeAbi<R>::memory) {
                if (call.gpr[0] == 0) return false;
                result.sret_address = call.gpr[0];
                cursor.gpr = 1;
            }
            Arguments arguments{};
            if (!LoadArguments(call, cursor, arguments,
                               std::index_sequence_for<Args...>{})) {
                return false;
            }
            auto fn = reinterpret_cast<R (*)(Args...)>(native_function);
            if constexpr (std::is_void_v<R>) {
                std::apply(fn, arguments);
                result.return_class = {0, 0};
                result.return_eightbyte_count = 0;
            } else {
                R value = std::apply(fn, arguments);
                if (!StoreReturn(value, result)) return false;
            }
            return true;
        }
    }
};

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...) noexcept> : FunctionTraits<R (*)(Args...)> {};

template <typename Fn>
constexpr AndroidX64HleAbiDescriptor MakeDescriptor() {
    using Traits = FunctionTraits<Fn>;
    using R = typename Traits::Return;
    AndroidX64HleAbiDescriptor descriptor{};
    descriptor.signature = __PRETTY_FUNCTION__;
    descriptor.invoke = &Traits::Invoke;
    descriptor.argument_count = static_cast<u32>(Traits::argument_count);
    descriptor.return_class[0] = static_cast<u8>(TypeAbi<R>::classes[0]);
    descriptor.return_class[1] = static_cast<u8>(TypeAbi<R>::classes[1]);
    descriptor.return_eightbyte_count = TypeAbi<R>::count;
    descriptor.sret_flags = TypeAbi<R>::memory ? AndroidX64SretPresent : 0;
    if constexpr (std::is_void_v<R>) {
        descriptor.return_size = 0;
    } else {
        descriptor.return_size = sizeof(R);
    }
    descriptor.supported = Traits::argument_count <= descriptor.arguments.size() &&
                           TypeAbi<R>::supported;
    std::size_t index{};
    std::size_t gpr = TypeAbi<R>::memory ? 1 : 0;
    std::size_t sse{};
    std::size_t stack{};
    auto add_argument = [&]<typename T>() {
        using Abi = TypeAbi<T>;
        descriptor.supported = descriptor.supported && Abi::supported;
        if (index < descriptor.arguments.size()) {
            auto& argument = descriptor.arguments[index++];
            argument.size = sizeof(T);
            argument.alignment = alignof(T);
            argument.classes[0] = static_cast<u8>(Abi::classes[0]);
            argument.classes[1] = static_cast<u8>(Abi::classes[1]);
            argument.eightbyte_count = Abi::count;
        }
        const std::size_t need_gpr =
            (Abi::classes[0] == AndroidX64SysVClass::Integer ? 1 : 0) +
            (Abi::classes[1] == AndroidX64SysVClass::Integer ? 1 : 0);
        const std::size_t need_sse =
            (Abi::classes[0] == AndroidX64SysVClass::Sse ? 1 : 0) +
            (Abi::classes[1] == AndroidX64SysVClass::Sse ? 1 : 0);
        if (!Abi::memory && gpr + need_gpr <= 6 && sse + need_sse <= 8) {
            gpr += need_gpr;
            sse += need_sse;
        } else {
            const std::size_t alignment = alignof(T) > 8 ? alignof(T) : 8;
            stack = AlignUp(stack, alignment);
            stack += AlignUp(sizeof(T), 8);
        }
    };
    []<typename... Args>(std::tuple<Args...>*, auto& add) {
        (add.template operator()<Args>(), ...);
    }(static_cast<typename Traits::Arguments*>(nullptr), add_argument);
    descriptor.gpr_argument_count = static_cast<u8>(gpr);
    descriptor.sse_argument_count = static_cast<u8>(sse);
    descriptor.stack_argument_bytes = static_cast<u32>(stack);
    descriptor.descriptor_id = CanonicalDescriptorId(descriptor);
    return descriptor;
}

} // namespace HleAbiDetail

template <typename Fn>
void RegisterAndroidX64TypedHleTarget(const char* name, u64 native_function, Fn) {
    static constexpr AndroidX64HleAbiDescriptor descriptor =
        HleAbiDetail::MakeDescriptor<Fn>();
    RegisterAndroidX64HleAbiDescriptor(native_function, name, descriptor);
}

template <typename Fn, AndroidX64HleInvokeFn Invoker>
void RegisterAndroidX64RawHleTarget(const char* name, u64 native_function, Fn) {
    using Traits = HleAbiDetail::FunctionTraits<Fn>;
    using R = typename Traits::Return;
    static constexpr AndroidX64HleAbiDescriptor descriptor = [] {
        AndroidX64HleAbiDescriptor value{};
        value.signature = __PRETTY_FUNCTION__;
        value.invoke = Invoker;
        value.call_flags = AndroidX64CallVariadic;
        value.return_class[0] = static_cast<u8>(HleAbiDetail::TypeAbi<R>::classes[0]);
        value.return_class[1] = static_cast<u8>(HleAbiDetail::TypeAbi<R>::classes[1]);
        value.return_eightbyte_count = HleAbiDetail::TypeAbi<R>::count;
        value.sret_flags = HleAbiDetail::TypeAbi<R>::memory
                               ? AndroidX64SretPresent
                               : 0;
        if constexpr (!std::is_void_v<R>) {
            value.return_size = sizeof(R);
        }
        value.supported = HleAbiDetail::TypeAbi<R>::supported;
        value.descriptor_id = HleAbiDetail::CanonicalDescriptorId(value);
        return value;
    }();
    RegisterAndroidX64HleAbiDescriptor(native_function, name, descriptor);
}

} // namespace Core::AeroLib

#endif // __ANDROID__
