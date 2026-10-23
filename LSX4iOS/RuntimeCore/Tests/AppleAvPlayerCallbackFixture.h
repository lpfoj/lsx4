// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/avplayer/avplayer_state.h"
#include "core/linker.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

extern "C" bool executor_lsx4_android_is_guest_address(void*);

namespace AppleAvPlayerCallbackFixture {
using namespace Libraries::AvPlayer;
struct Observation {
    std::atomic<unsigned> calls{};
    AvPlayerEvents event{};
    s32 source{};
    u32 warning{};
    bool controller_thread{};
};
static void PS4_SYSV_ABI Observe(void* opaque, AvPlayerEvents event, s32 source, void* data) {
    auto& observed = *static_cast<Observation*>(opaque);
    char name[64]{};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    observed.event = event;
    observed.source = source;
    observed.warning = data ? *static_cast<u32*>(data) : 0;
    observed.controller_thread = std::strcmp(name, "shadPS4:AvController") == 0;
    observed.calls.fetch_add(1, std::memory_order_release);
}
static void Run(Core::MemoryManager* memory, const std::filesystem::path& root) {
    // Kernel::Thread allocates real guest TLS from module zero. Supply a tiny
    // owned ELF instead of bypassing thread creation or using a commercial game.
    const auto token = Core::AeroLib::GetAndroidX64HleStubForNative("apple-avplayer-observe", &Observe);
    std::vector<u8> code{0x48, 0xb8};
    AppleCurrentHleTokenFixture::Put64(code, token);
    code.insert(code.end(), {0xff, 0xe0}); // x64 MOV RAX, HLE token; JMP RAX.
    elf_header header{};
    std::memcpy(header.e_ident.magic, "\177ELF", 4);
    header.e_ident.ei_class = ELF_CLASS_64;
    header.e_ident.ei_data = ELF_DATA_2LSB;
    header.e_ident.ei_version = ELF_VERSION_CURRENT;
    header.e_ident.ei_osabi = ELF_OSABI_FREEBSD;
    header.e_type = ET_SCE_DYNEXEC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(elf_program_header);
    header.e_phnum = 1;
    elf_program_header segment{};
    segment.p_type = PT_LOAD;
    segment.p_flags = static_cast<elf_program_flags>(7);
    segment.p_offset = 0x4000;
    segment.p_filesz = code.size();
    segment.p_memsz = segment.p_align = 0x4000;
    std::vector<u8> elf(0x4000 + code.size());
    std::memcpy(elf.data(), &header, sizeof(header));
    std::memcpy(elf.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(elf.data() + 0x4000, code.data(), code.size());
    const auto path = root / "avplayer-callback-fixture.elf";
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<char*>(elf.data()), elf.size()); }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    Require(linker->GetModuleCount() == 0 && linker->LoadModule(path) == 0,
            "AvPlayer fixture could not load its primary TLS module");
    linker->PrepareStaticTlsForMainModule();
    Libraries::Kernel::Pthread main_thread{};
    struct Current {
        Libraries::Kernel::Pthread* prior = Libraries::Kernel::g_curthread;
        explicit Current(Libraries::Kernel::Pthread* thread) {
            Libraries::Kernel::g_curthread = thread;
            Libraries::Kernel::ThrState::Instance()->active_threads.fetch_add(1);
        }
        ~Current() {
            Libraries::Kernel::ThrState::Instance()->active_threads.fetch_sub(1);
            Libraries::Kernel::g_curthread = prior;
        }
    } current{&main_thread};
    void* mapping = reinterpret_cast<void*>(linker->GetModule(0)->GetBaseAddress());
    void* dynamic{};
    struct DynamicCleanup {
        Core::MemoryManager* memory;
        void*& address;
        ~DynamicCleanup() { if (address) memory->UnmapMemory(reinterpret_cast<VAddr>(address), 16384); }
    } dynamic_cleanup{memory, dynamic};
    struct Cleanup {
        Executor::CpuTuning::ProfileV1 profile = Executor::CpuTuning::Get();
        ~Cleanup() { Executor::CpuTuning::SetForTesting(profile); }
    } cleanup;
    Require(!executor_lsx4_android_is_guest_address(reinterpret_cast<void*>(&Observe)) &&
            !executor_lsx4_android_is_guest_address(reinterpret_cast<void*>(0x10000)),
            "Guest callback classifier claimed a native or unowned address");
    for (unsigned route = 0; route != 5; ++route) {
        if (route == 3) {
            Require(memory->MapMemory(&dynamic, memory->GetAddressSpace().UserVirtualBase() + (96ULL << 20),
                16384, Core::MemoryProt::CpuReadWrite, Core::MemoryMapFlags{}, Core::VMAType::Flexible,
                "apple-dynamic-callback-test", false) == 0, "Dynamic callback mapping failed");
            Require(!executor_lsx4_android_is_guest_address(dynamic), "RW memory became a guest function");
            std::memcpy(dynamic, code.data(), code.size());
            Require(memory->Protect(reinterpret_cast<VAddr>(dynamic), 16384,
                Core::MemoryProt::CpuReadWrite | Core::MemoryProt::CpuExec) == 0,
                "Dynamic callback executable publication failed");
            Require(executor_lsx4_android_is_guest_address(dynamic),
                    "Live anonymous guest executable callback was not recognized");
            mapping = dynamic;
        }
        auto profile = cleanup.profile;
        profile.execution_mode = (route == 2 || route == 4) ? Executor::CpuTuning::ExecutionMode::Interpreter :
            Executor::CpuTuning::ExecutionMode::Jit;
        profile.persistent_cache = Executor::CpuTuning::PersistentCacheMode::Off;
        Executor::CpuTuning::SetForTesting(profile);
        Observation observed;
        AvPlayerInitData init{};
        init.event_replacement.object_ptr = &observed;
        init.event_replacement.event_callback = route == 0 ? &Observe :
            reinterpret_cast<AvPlayerEventCallback>(mapping);
        {
            // Public decoder notification follows the production queue and the real
            // Kernel::Thread controller, including native DefaultEventCallback.
            AvPlayerState state(init);
            static_cast<AvPlayerStateCallback&>(state).OnWarning(0x87654321);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!observed.calls.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Require(observed.calls.load(std::memory_order_acquire) == 1 &&
                observed.event == AvPlayerEvents::WarningId && observed.source == 0 &&
                observed.warning == 0x87654321 && observed.controller_thread,
                "AvPlayer controller lost callback, ABI arguments, or host fallback");
        std::fprintf(stderr, "[APPLE_AVPLAYER_CALLBACK_TEST] route=%u calls=1 controller=1 arguments=passed\n", route);
    }
    Require(memory->Protect(reinterpret_cast<VAddr>(dynamic), 16384, Core::MemoryProt::CpuReadWrite) == 0 &&
            !executor_lsx4_android_is_guest_address(dynamic), "Removing execute permission kept callback admission");
    void* retired = dynamic;
    Require(memory->UnmapMemory(reinterpret_cast<VAddr>(dynamic), 16384) == 0,
            "Dynamic callback retirement failed");
    dynamic = nullptr;
    Require(!executor_lsx4_android_is_guest_address(retired), "Retired guest callback remained executable");
    std::fprintf(stderr, "[APPLE_DYNAMIC_CALLBACK_TEST] hostRejected=1 rwRejected=1 live=1 jit=1 interpreter=1 protectionRetired=1 unmapRetired=1\n");
}
}
