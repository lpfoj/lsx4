// SPDX-License-Identifier: GPL-2.0-or-later
// Executes the real console initialization and memory manager without replacing
// any game HLEs with fixture functions. No gameplay/FPS claim is made here.
#include <cstdio>
#include "AndroidManagedLaunch.h"
#include "AndroidRenderLaunch.h"
#include "AppleManagedOptionControl.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <mach/mach.h>
#include "common/elf_info.h"
#include "common/config.h"
#include "common/logging/backend.h"
#include "core/aerolib/stubs.h"
#include "core/memory.h"
#include "core/file_sys/fs.h"
#include "common/singleton.h"
#include "executor/cpu_tuning.h"
#include "executor/dynamic_translation/host_isa_profile.h"
#include "../AppleHostServices.h"
#include "AppleAllocationDiagnostics.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <imgui.h>
extern "C" int executor_lsx4_runtime_initialize(const char*,const char*);
extern "C" const char* executor_lsx4_runtime_status();
extern "C" int executor_lsx4_runtime_launch_game(const char*);
extern "C" int executor_lsx4_runtime_launch_game_jit(const char*);
extern "C" int executor_lsx4_runtime_relocate_imports();
extern "C" int executor_lsx4_runtime_set_guest_cpu_backend_v2(int,const char*,const char*,const char*);
extern "C" void LSX4CheckAppleMetalValidationState();
extern "C" bool executor_lsx4_apple_initial_stack_fixture();
extern "C" int LSX4RunAppleEmptyFragmentFixture();
extern "C" int LSX4RunAppleRasterPrecisionFixture();
namespace Core::AeroLib {
u64 ExecutorMonoPathLog(u64,u64);
u64 ExecutorLibcMallocUsableSize(u64);
u64 ExecutorMspaceMallocUsableSize(u64,u64);
u64 ExecutorLibcMemset(u64,u64,u64);
extern "C" s64 ExecutorLibcSscanf(u64,u64,u64,u64,u64,u64);
extern "C" s64 ExecutorLibcVsscanf(u64,u64,u64);
extern "C" u64 ExecutorLibcFopen(u64,u64);
extern "C" u64 ExecutorLibcFclose(u64);
extern "C" u64 ExecutorLibcFwrite(u64,u64,u64,u64);
extern "C" s64 ExecutorLibcFseek(u64,s64,s32);
extern "C" s64 ExecutorLibcFscanf(u64,u64,u64,u64,u64,u64);
}
extern "C" const char* executor_lsx4_runtime_guest_cpu_backend_status();
extern "C" bool LSX4FexPreflightBeforePool() {
 const char* home=std::getenv("HOME"); const char* game=std::getenv("LSX4_CORE_GAME");
 if(!home || !game) return false;
 const auto root=std::filesystem::path(home)/"Documents/shared-runtime";
 std::filesystem::create_directories(root);
 std::freopen((root/"probe-stderr.log").c_str(),"w",stderr);std::setvbuf(stderr,nullptr,_IONBF,0);
 if(const char* id=std::getenv("LSX4_PROBE_RUN_ID"))std::fprintf(stderr,"[APPLE_PROBE_RUN] id=%s\n",id);
 const auto title=std::filesystem::path(game).parent_path().filename().string();
 const int selected=executor_lsx4_runtime_set_guest_cpu_backend_v2(1,"@apple-embedded-fex-v3",root.c_str(),title.c_str());
 std::fprintf(stderr,"[APPLE_FEX_PREFLIGHT] selected=%d status=%s\n",selected,executor_lsx4_runtime_guest_cpu_backend_status());
 return selected==1;
}
static void ReportAppleHostIsa() {
    const auto& isa=Executor::Jit::GetHostIsaProfile();
    std::fprintf(stderr,"[APPLE_HOST_ISA] features=0x%llx os=0x%llx sveBytes=%u\n",
        static_cast<unsigned long long>(isa.features),
        static_cast<unsigned long long>(isa.os_features),isa.sve_vector_bytes);
}
static std::string report;
const char* LSX4SharedCoreProbeReport() { return report.c_str(); }
static void Require(bool result,const char* message) { if(!result) throw std::runtime_error(message); }
#include "JitMemoryRouteFixture.h"
#include "FragmentedGuestArenaFixture.h"
#include "ScopedBackingFixture.h"
#include "ExportedHostDataFixture.h"
#include "AppleCurrentHleTokenFixture.h"
#include "ScalarI64BoundaryFixture.h"
#include "AppleCurrentSignalFixture.h"
#include "AppleGpuDetileFixture.h"
#include "AppleGpuAliasFixture.h"
#include "AppleVideoOutSourceFixture.h"
#include "AppleCondWakeFixture.h"
#include "AppleAvPlayerCallbackFixture.h"
#include "AppleNgsVoiceFixture.h"
int LSX4RunAerolibGuestHeapProbe();
static void TestScalarI64Boundary(Core::MemoryManager* memory) {
    constexpr std::size_t bytes=16u<<10;
    void* mapping{};
    Require(memory->MapMemory(&mapping,Core::DEFAULT_MAPPING_BASE,bytes,
        Core::MemoryProt::CpuReadWrite,Core::MemoryMapFlags{},Core::VMAType::Flexible,
        "apple-scalar-boundary-test",false)==0,"Scalar boundary fixture mapping failed");
    struct Cleanup {
        Core::MemoryManager* memory;void* mapping;
        Executor::CpuTuning::ProfileV1 profile=Executor::CpuTuning::Get();
        ~Cleanup(){Executor::CpuTuning::SetForTesting(profile);
            memory->UnmapMemory(reinterpret_cast<VAddr>(mapping),bytes);}
    } cleanup{memory,mapping};
    for(unsigned interpreter=0;interpreter<2;++interpreter) {
        auto profile=cleanup.profile;
        profile.execution_mode=interpreter?Executor::CpuTuning::ExecutionMode::Interpreter:
            Executor::CpuTuning::ExecutionMode::Jit;
        profile.persistent_cache=Executor::CpuTuning::PersistentCacheMode::Off;
        Executor::CpuTuning::SetForTesting(profile);
        ScalarI64BoundaryFixture::Run(static_cast<u8*>(mapping)+interpreter*4096,Require);
    }
}
static void TestScopedBacking(Core::MemoryManager* memory) {
    constexpr u64 page=16ULL<<10,physicalBytes=128ULL<<20;
    const auto physical=memory->Allocate(0,memory->GetTotalDirectSize(),physicalBytes,page,3);
    Require(physical!=static_cast<PAddr>(-1),"Scoped backing fixture allocation failed");
    struct Cleanup {
        Core::MemoryManager* memory;
        PAddr physical;
        VAddr virtualBase{};
        ~Cleanup() {
            if(virtualBase) memory->UnmapMemory(virtualBase,5*page);
            memory->Free(physical,physicalBytes,true);
        }
    } cleanup{memory,physical};
    auto& space=memory->GetAddressSpace();
    ScopedBackingFixture::RunLeases(space,physical,Require);

    // Two adjacent virtual spans overlap physically: the second acquired
    // backing view covers the first. A writer must retain its earlier lease
    // while staging the second span, then preserve the original write order.
    // The final reserved page is a valid owned VA with no physical backing.
    void* window{};
    Require(memory->MapMemory(&window,Core::DEFAULT_MAPPING_BASE,5*page,
        Core::MemoryProt::NoAccess,Core::MemoryMapFlags{},Core::VMAType::Reserved,
        "scoped-write-window",false)==0,"Scoped write virtual reservation failed");
    cleanup.virtualBase=reinterpret_cast<VAddr>(window);
    const auto segmentPhysical=physical+(96ULL<<20);
    const auto fixed=Core::MemoryMapFlags::Fixed|Core::MemoryMapFlags::NoCoalesce;
    void* first{};void* second{};
    Require(memory->MapMemory(&first,cleanup.virtualBase,page,Core::MemoryProt::CpuReadWrite,
        fixed,Core::VMAType::Direct,"scoped-write-first",true,segmentPhysical+page,page)==0,
        "Scoped write first mapping failed");
    Require(memory->MapMemory(&second,cleanup.virtualBase+page,3*page,Core::MemoryProt::CpuReadWrite,
        fixed,Core::VMAType::Direct,"scoped-write-covering",false,segmentPhysical,page)==0,
        "Scoped write covering alias failed");
    Require(first==window && reinterpret_cast<VAddr>(second)==cleanup.virtualBase+page,
        "Scoped write fixture lost adjacent virtual spans");
    std::memset(second,0x5a,3*page);
    std::weak_ptr<void> narrowOwner;
    {
        auto narrow=space.AcquireBackingSpan(segmentPhysical+page,page);
        narrowOwner=narrow.owner;
    }
    std::vector<u8> source(4*page);
    for(std::size_t index=0;index<source.size();++index)
        source[index]=static_cast<u8>((index/page)*0x31+index%251);
    Require(memory->TryWriteBacking(first,source.data(),source.size()),
        "Staged multi-span backing write rejected valid physical coverage");
    Require(std::memcmp(second,source.data()+page,3*page)==0 &&
            std::memcmp(first,source.data()+2*page,page)==0,
        "Staged backing leases lost aliases or changed overlapping write order");
    Require(narrowOwner.expired(),"Completed staged write retained a superseded backing lease");

    const std::vector<u8> rejected(5*page,0xee);
    Require(!memory->TryWriteBacking(first,rejected.data(),rejected.size()),
        "Staged backing write accepted a reserved tail without physical backing");
    Require(std::memcmp(second,source.data()+page,3*page)==0 &&
            std::memcmp(first,source.data()+2*page,page)==0,
        "Rejected multi-span write modified bytes before validating its complete range");
    {
        auto guard=memory->AcquireSnapshotReadGuard();
        const std::vector<u8> guarded(4*page,0x3d);
        Require(memory->TryWriteBacking(guard,first,guarded.data(),guarded.size()),
                "Snapshot-owned GPU publication failed");
        Require(std::memcmp(second,guarded.data()+page,3*page)==0 &&
                std::memcmp(first,guarded.data()+2*page,page)==0,
                "Snapshot-owned publication lost alias bytes");
        Require(!memory->TryWriteBacking(guard,first,rejected.data(),rejected.size()) &&
                std::memcmp(second,guarded.data()+page,3*page)==0,
                "Snapshot-owned invalid tail published partial bytes");
        auto moved=std::move(guard);
        Require(!memory->TryWriteBacking(guard,first,source.data(),source.size()),
                "Moved-from snapshot guard authorized a publication");
        Require(memory->TryWriteBacking(moved,first,source.data(),source.size()),
                "Moved snapshot guard lost its publication lease");
    }
    std::fprintf(stderr,"[APPLE_GUARDED_BACKING_WRITE_TEST] noUpgrade=1 aliasBytes=1 "
        "invalidTailUnchanged=1 movedFromRejected=1 movedLeaseValid=1\n");
    std::fprintf(stderr,"[APPLE_SCOPED_BACKING_WRITE_TEST] stagedOverlap=1 lastLeaseRelease=1 "
        "writeOrder=1 invalidTailUnchanged=1\n");
}
static void TestFontPixelRetirement() {
    struct Context {
        ImGuiContext* previous=ImGui::GetCurrentContext();
        ImGuiContext* owned=ImGui::CreateContext();
        ~Context() { ImGui::DestroyContext(owned);ImGui::SetCurrentContext(previous); }
    } context;
    auto* atlas=ImGui::GetIO().Fonts;
    ImFontConfig config{};config.SizePixels=32;
    auto* font=atlas->AddFontDefault(&config);
    config.SizePixels=128;atlas->AddFontDefault(&config);
    unsigned char* pixels{};int width{},height{};
    atlas->GetTexDataAsRGBA32(&pixels,&width,&height);
    Require(pixels && width>0 && height>0,"Font atlas build failed");
    const std::vector<unsigned char> expected(pixels,pixels+std::size_t(width)*height*4);
    const auto glyph=*font->FindGlyph('A');
    atlas->ClearTexData();
    Require(!atlas->TexPixelsAlpha8 && !atlas->TexPixelsRGBA32 && atlas->Fonts.Size==2 &&
            atlas->Sources.Size==2 && font->FindGlyph('A')->AdvanceX==glyph.AdvanceX,
            "Font pixel retirement discarded retained font definitions");
    unsigned char* rebuilt{};int rebuiltWidth{},rebuiltHeight{};
    atlas->GetTexDataAsRGBA32(&rebuilt,&rebuiltWidth,&rebuiltHeight);
    Require(rebuiltWidth==width && rebuiltHeight==height && rebuilt &&
            std::memcmp(rebuilt,expected.data(),expected.size())==0,
            "Font atlas recreation changed pixels");
    atlas->ClearTexData();
    std::fprintf(stderr,"[APPLE_FONT_RETIREMENT_TEST] passed fonts=2 rebuildBytes=%zu equalPixels=1\n",
                 expected.size());
}
static void TestBdaStorageLifetime() {
    Vulkan::Instance instance(false,false,true);
    Require(static_cast<bool>(instance.GetDevice()),"BDA fixture has no GPU device");
    Vulkan::Scheduler scheduler(instance);
    TestAppleGpuDetile(instance,scheduler);
    constexpr u64 bytes=512ULL<<20;
    constexpr u32 block=4096;
    VideoCore::BdaPageTableBuffer table(instance,scheduler,bytes);
    Require(table.SizeBytes()==bytes && table.Offset(bytes-block)==bytes-block &&
            table.CpuAddr()==0 && table.ResidentBytes()==0,
            "Unused BDA table consumed memory or changed address coverage");
    table.Fill(0,0,0);
    Require(table.EnsureResident(0,0) && table.ResidentBytes()==0,
            "Empty BDA operations allocated physical storage");
    table.Fill(0,block,0x1234abcd);
    table.Fill(bytes-block,block,0x5678ef90);
    const auto handle=table.Handle();
    Require(handle && table.Handle()==handle && table.ResidentBytes()>0,
            "BDA first writes failed to publish stable storage");
    VideoCore::Buffer download(instance,scheduler,VideoCore::MemoryUsage::Download,
                               0,VideoCore::AllFlags,2*block);
    auto cmdbuf=scheduler.CommandBuffer();
    const vk::BufferCopy regions[]={{0,0,block},{bytes-block,block,block}};
    cmdbuf.copyBuffer(handle,download.Handle(),2,regions);
    const auto barrier=download.GetBarrier(vk::AccessFlagBits2::eHostRead,
                                           vk::PipelineStageFlagBits2::eHost);
    Require(barrier.has_value(),"BDA readback missing host visibility barrier");
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&*barrier});
    scheduler.Finish();
    download.InvalidateMappedRange(0,2*block);
    Require(download.mapped_data.size()>=2*block,"BDA readback is not mapped");
    for(u32 offset=0;offset<2*block;offset+=sizeof(u32)) {
        u32 observed{};std::memcpy(&observed,download.mapped_data.data()+offset,sizeof(observed));
        Require(observed==(offset<block?0x1234abcdu:0x5678ef90u),
                "BDA GPU readback lost first or last logical page");
    }
    VideoCore::BdaPageTableBuffer descriptorFirst(instance,scheduler,block);
    Require(static_cast<bool>(descriptorFirst.Handle()),"BDA descriptor-first allocation failed");
    VideoCore::BdaPageTableBuffer residentFirst(instance,scheduler,block);
    Require(residentFirst.EnsureResident(0,block) && residentFirst.ResidentBytes()>0 &&
            static_cast<bool>(residentFirst.Handle()),"BDA residency-first allocation failed");
    std::fprintf(stderr,"[APPLE_BDA_STORAGE_TEST] passed logicalBytes=%llu residentBytes=%llu "
        "unusedBytes=0 firstAndLastPageReadback=1 activationRoutes=3\n",bytes,table.ResidentBytes());
}
static std::uint64_t ResidentFootprint() {
    task_vm_info_data_t info{};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    Require(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&info),&count)==KERN_SUCCESS,
        "Unable to read test memory footprint");
    return info.phys_footprint;
}
static void DumpRequestedMemoryAfterFailure() {
    const char* setting=std::getenv("LSX4_PROBE_FAILURE_MEMORY");
    if(!setting) return;
    char* end=nullptr;const auto address=std::strtoull(setting,&end,0);
    if(!address || !end || *end) return;
    std::uint64_t values[16]{};
    if(!Core::Memory::Instance()->TryCopyBackingMemory(address,values,sizeof(values))) {
        std::uint64_t first{},last{};
        if(!Lsx4::Apple::QueryReadableHostRange(address,first,last) || last-address<sizeof(values)) {
            std::fprintf(stderr,"[APPLE_FAILURE_MEMORY] address=0x%llx unavailable\n",address);return;
        }
        std::memcpy(values,reinterpret_cast<const void*>(address),sizeof(values));
    }
    for(unsigned i=0;i<16;++i) std::fprintf(stderr,"[APPLE_FAILURE_MEMORY] address=0x%llx value=0x%llx\n",
        address+i*8,values[i]);
}
int LSX4RunSharedCoreProbe(bool interpreter) {
    try {
        InstallAppleAllocationFailureDiagnostics();
        const char* container=std::getenv("HOME");
        Require(container!=nullptr,"App container is unavailable");
        const char* explicitRoot=std::getenv("LSX4_PROBE_ROOT");
        const auto root=explicitRoot?std::filesystem::path(explicitRoot):
            std::filesystem::path(container)/"Documents"/"shared-runtime";
        std::filesystem::create_directories(root);
        if (std::getenv("LSX4_PROBE_JIT_PERF_MAP")) {
            const char* runId=std::getenv("LSX4_PROBE_RUN_ID");
            Require(runId && *runId,"JIT perf map requires a launch identity");
            const auto mapPath=root/(std::string("jit-perf-")+runId+".map");
            setenv("EXECUTOR_JIT_HELPER_PERF_MAP_PATH",mapPath.c_str(),1);
            setenv("EXECUTOR_JIT_HELPER_PERF_MAP","1",1);
        }
        if(std::getenv("LSX4_PROBE_SHADER_DUMP")) {
            const char* runId=std::getenv("LSX4_PROBE_RUN_ID");
            Require(runId && *runId,"Shader dumps require a launch identity");
            const auto dumpPath=root/(std::string("shader-dumps-")+runId);
            std::filesystem::create_directories(dumpPath);
            setenv("EXECUTOR_SHADER_DUMP_DIR",dumpPath.c_str(),1);
            setenv("EXECUTOR_DUMP_SHADERS","1",1);
        }
        if(std::getenv("LSX4_PROBE_GPU_TEXTURE_DUMP")) {
            const char* runId=std::getenv("LSX4_PROBE_RUN_ID");
            Require(runId && *runId,"GPU texture dumps require a launch identity");
            const auto dumpPath=root/(std::string("gpu-textures-")+runId);
            std::filesystem::create_directories(dumpPath);
            setenv("EXECUTOR_GPU_CONTENT_DUMP_DIR",dumpPath.c_str(),1);
        }
        if(!std::getenv("LSX4_CORE_GAME") && std::getenv("LSX4_PROBE_RUN_ID")) {
            std::freopen((root/"fixture-stderr.log").c_str(),"w",stderr);
            std::setvbuf(stderr,nullptr,_IONBF,0);
            ReportAppleHostIsa();
            std::fprintf(stderr,"[APPLE_PROBE_RUN] id=%s\n",std::getenv("LSX4_PROBE_RUN_ID"));
        }
        if(const char* game=std::getenv("LSX4_CORE_GAME")) {
            std::freopen((root/"probe-stderr.log").c_str(),"w",stderr);
            std::setvbuf(stderr,nullptr,_IONBF,0);
            ReportAppleHostIsa();
            // One launch identifier lets the host distinguish a fresh fatal
            // diagnostic from a retained report after a transport timeout.
            if(const char* runId=std::getenv("LSX4_PROBE_RUN_ID")) {
                std::fprintf(stderr,"[APPLE_PROBE_RUN] id=%s\n",runId);
            }
            // Match Android's launch order: publish the common CPU/HLE policy
            // before loading modules or starting a guest worker. Current is the
            // available Apple backend; importing a profile does not imply FEX.
            const auto title=std::filesystem::path(game).parent_path().filename().string();
            Require(AndroidRenderLaunch::WriteBeforeCpu(root,title.c_str()),"Android presentation config write failed");
            Require(AndroidManagedLaunch::ApplyBeforeCpu(title.c_str()),"Android managed launch configuration failed");
            Require(AppleManagedOptionControl::Apply(),"Invalid managed option diagnostic");
            const bool fex = std::getenv("LSX4_APPLE_CPU_BACKEND") && std::strcmp(std::getenv("LSX4_APPLE_CPU_BACKEND"), "fex") == 0;
            Require(executor_lsx4_runtime_set_guest_cpu_backend_v2(fex ? 1 : 0,
                    fex ? "@apple-embedded-fex-v3" : "",root.c_str(),title.c_str()) == (fex ? 1 : 0),
                    "Apple requested backend configuration failed");
            std::fprintf(stderr,"[APPLE_BACKEND_REQUEST] backend=%s experimental=1\n", fex ? "fex" : "current");
            const auto profile=Executor::CpuTuning::Get();
            std::fprintf(stderr,"[APPLE_CPU_PROFILE] identity=0x%llx workers=%u "
                "execution=%u memory=%u block=%u linking=%u cache=%u hle=%u\n",
                static_cast<unsigned long long>(profile.identity),profile.max_guest_workers,
                static_cast<unsigned>(profile.execution_mode),static_cast<unsigned>(profile.memory_manager),
                static_cast<unsigned>(profile.jit_block_size),profile.block_linking,
                static_cast<unsigned>(profile.persistent_cache),static_cast<unsigned>(profile.hle_mode));
        }
        if(!std::getenv("LSX4_CORE_GAME") && !interpreter &&
           !std::getenv("LSX4_CORE_AEROLIB_HEAP_FIXTURE")) {
            // Select Current through the same public API and order as a game.
            // HLE dispatch admission must not depend on a fixture-only override.
            Require(executor_lsx4_runtime_set_guest_cpu_backend_v2(0,"",root.c_str(),"")==0,
                    "Current backend fixture configuration failed");
        }
        if(!std::getenv("LSX4_CORE_GAME")) {
            // A prior game's persisted policy may normalize Interpreter back to
            // IrJit (ignore_bad_accesses). Fixtures exercise both routes against
            // a known profile; game launches above retain their Android policy.
            Executor::CpuTuning::ProfileV1 fixtureProfile{};
            if(std::getenv("LSX4_TEST_UNLINKED_NATIVE")) fixtureProfile.block_linking=0;
            Executor::CpuTuning::SetForTesting(fixtureProfile);
            std::fprintf(stderr,"[APPLE_FIXTURE_PROFILE] isolated=1 ignoreBadAccesses=0 linking=%u\n",
                         fixtureProfile.block_linking);
        }
        LSX4CheckAppleMetalValidationState();
        Require(executor_lsx4_runtime_initialize(root.c_str(),"")==0,"Full runtime initialize failed");
        if(std::getenv("LSX4_CORE_AEROLIB_HEAP_FIXTURE")) {
            Require(!std::getenv("LSX4_CORE_GAME"),"Heap fixture requires a fresh process without a game");
            Common::ElfInfo::Instance().InitializeForAndroidProbe(root,Common::ElfInfo::FW_45);
            Require(LSX4RunAerolibGuestHeapProbe()==0,"Actual Aerolib fallback heap contract failed");
            report="{\"status\":\"passed\",\"fullRuntimeInitialized\":true,\"actualAerolibFallback\":true,"
                "\"initialChunkBytes\":16777216,\"growthPreservesLiveObjects\":true,\"callocZeroed\":true,"
                "\"reallocGrowthAndShrink\":true,\"overflowFailurePreservesOldAllocation\":true,"
                "\"gameplayTested\":false}";
            return 0;
        }
        if(const char* markers=std::getenv("LSX4_PROBE_GPU_MARKERS");
           markers && std::strcmp(markers,"1")==0) {
            // Opt-in command-buffer diagnostics only; the ordinary app does not
            // format per-draw labels or enable expensive Vulkan object names.
            Config::setVkHostMarkersEnabled(true);
            Config::setVkGuestMarkersEnabled(true);
            std::fprintf(stderr,"[APPLE_GPU_MARKERS] host=1 guest=1\n");
        }
        if(std::getenv("LSX4_PROBE_COMMON_LOG")) {
            Config::setLogFilter("*:Info");Config::setLogType("sync");
            Config::setLoggingEnabled(true);
            Common::Log::Initialize((root/"common-runtime.log").string());
        }
        if(std::getenv("LSX4_PROBE_AUDIO_MUTE")) {
            Config::setVolumeSlider(0);
            std::fprintf(stderr,"[APPLE_AUDIO] output_gain=0 processing_enabled=1\n");
        } else if(const char* volume=std::getenv("LSX4_PROBE_AUDIO_VOLUME")) {
            char* end=nullptr; const long value=std::strtol(volume,&end,10);
            Require(end && !*end && value>=0 && value<=100,"Invalid audio volume");
            Config::setVolumeSlider(static_cast<int>(value));
        }
        if (interpreter)
            Executor::CpuTuning::MutableProfile().execution_mode=Executor::CpuTuning::ExecutionMode::Interpreter;
        if (const char* game=std::getenv("LSX4_CORE_GAME")) {
            const auto executable=root.parent_path()/game;
            const char* phase=std::getenv("LSX4_CORE_GAME_STAGE");
            if(phase && std::string(phase)=="run") {
                Require(AndroidManagedLaunch::ApplyScaleBeforeGame(executable.parent_path().filename().c_str()),"Android scene scale configuration failed");
                const int ran=executor_lsx4_runtime_launch_game_jit(executable.c_str());
                if(ran<0) DumpRequestedMemoryAfterFailure();
                report=executor_lsx4_runtime_status();return ran<0?1:0;
            }
            const int loaded=executor_lsx4_runtime_launch_game(executable.c_str());
            if(loaded<0) { report=executor_lsx4_runtime_status();return 1; }
            if(phase && std::string(phase)=="relocate") {
                const int relocated=executor_lsx4_runtime_relocate_imports();
                if(relocated<0) { report=executor_lsx4_runtime_status();return 1; }
            }
            report=executor_lsx4_runtime_status();
            return 0;
        }
        Require(LSX4RunAppleRasterPrecisionFixture()==0,"Native Metal raster precision diagnostic failed");
        Require(LSX4RunAppleEmptyFragmentFixture()==0,"Native Metal empty-fragment/stencil contract failed");
        TestBdaStorageLifetime();
        TestFontPixelRetirement();
        Common::ElfInfo::Instance().InitializeForAndroidProbe(root,Common::ElfInfo::FW_45);
        // Exercise the real common HLE reader: opaque ABI integers and guard
        // pages are not C strings, even when they pass a numeric low cutoff.
        Require(Core::AeroLib::ExecutorMonoPathLog(0x10000,0)==0,
                "Opaque HLE integer was treated as a guest string");
        const auto hostPage=static_cast<std::size_t>(getpagesize());
        void* strings=mmap(nullptr,hostPage*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        Require(strings!=MAP_FAILED,"HLE string fixture mapping failed");
        std::memcpy(strings,"probe",6);
        static_cast<char*>(strings)[hostPage-1]='x';
        Require(mprotect(static_cast<char*>(strings)+hostPage,hostPage,PROT_NONE)==0,
                "HLE string fixture guard failed");
        Require(Core::AeroLib::ExecutorMonoPathLog(reinterpret_cast<u64>(strings),
                    reinterpret_cast<u64>(strings)+hostPage-1)==0,
                "HLE string reader crossed a guard page");
        Require(Core::AeroLib::ExecutorMonoPathLog(reinterpret_cast<u64>(strings)+hostPage,0)==0,
                "HLE string reader accepted a protected page");
        Require(munmap(strings,hostPage*2)==0,"HLE string fixture cleanup failed");
        Require(Core::AeroLib::ExecutorMonoPathLog(reinterpret_cast<u64>(strings),0)==0,
                "HLE string reader accepted an unmapped page");
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=hle-opaque-pointer-contract-passed\n");
        Require(Core::AeroLib::ExecutorConfigureLibcAllocatorOwner(false),"HLE allocator ownership rejected");
        auto* memory=Core::Memory::Instance();
        memory->SetupMemoryRegions(448ULL<<20,false,false);
        TestScalarI64Boundary(memory);
        TestAppleGpuAliasReadback(memory);
        TestAppleVideoOutSource(memory);
        if(std::getenv("LSX4_CORE_GPU_ALIAS_FIXTURE")) {
            report="{\"status\":\"passed\",\"gpuAliasSnapshotReadback\":true,\"gameplayTested\":false}";
            return 0;
        }
        ExportedHostDataFixture::Run();
        AppleCondWakeFixture::Run();
        AppleNgsVoiceFixture::Run();
        const auto heap=memory->ExecutorLibcMspaceBase();
        Require(memory->IsValidMapping(heap,Core::EXECUTOR_LIBC_MSPACE_CAPACITY),"Relocated libc heap outside owned arena");
        std::uint64_t hostHeapFirst{},hostHeapLast{};
        Require(Lsx4::Apple::QueryHostRange(heap,PROT_READ|PROT_WRITE,hostHeapFirst,hostHeapLast),
                "HLE heap metadata says writable but Apple mapping is inaccessible");
        Require(Lsx4::Apple::QueryHostRange(heap+Core::EXECUTOR_LIBC_MSPACE_CAPACITY-1,
                    PROT_READ|PROT_WRITE,hostHeapFirst,hostHeapLast),
                "HLE heap growth capacity lost the Android read/write mapping contract");
        auto* heapByte=reinterpret_cast<volatile unsigned char*>(heap+16384);
        *heapByte=0x5a;
        Require(*heapByte==0x5a,"HLE heap host store/read round trip failed");
        *heapByte=0;
        const auto allocation=Core::AeroLib::ExecutorLibcMalloc(129);
        Require(allocation!=0,"Common HLE allocator failed");
        const auto usable=Core::AeroLib::ExecutorLibcMallocUsableSize(allocation);
        Require(usable>=129,"Apple malloc_usable_size lost common allocation metadata");
        Require(Core::AeroLib::ExecutorMspaceMallocUsableSize(0,allocation)==usable,
                "Apple mspace usable-size disagrees with common allocation owner");
        Require(Core::AeroLib::ExecutorLibcMallocUsableSize(0)==0 &&
                    Core::AeroLib::ExecutorLibcMallocUsableSize(0x10000)==0,
                "Unknown pointers must not be passed to Apple's native malloc_size");
        static volatile unsigned char hostImageCanary=0x6b;
        Core::AeroLib::ExecutorLibcMemset(reinterpret_cast<u64>(&hostImageCanary),0,1);
        Require(hostImageCanary==0x6b,"Guest libc overwrote the host runtime image");
        Core::AeroLib::ExecutorLibcMemset(allocation,0xa5,129);
        Require(*reinterpret_cast<volatile unsigned char*>(allocation)==0xa5 &&
                    *reinterpret_cast<volatile unsigned char*>(allocation+128)==0xa5,
                "Host-image protection rejected a real guest allocation");
        // The scanner creates a host-owned SysV va_list. Its metadata is not
        // a guest VMA, even though every output pointer below is guest-owned.
        std::memcpy(reinterpret_cast<void*>(allocation),"bridge_slot_314159",19);
        std::memcpy(reinterpret_cast<void*>(allocation+32),"bridge_slot_%u",15);
        auto* scanned=reinterpret_cast<unsigned*>(allocation+96);
        *scanned=0xdeadbeef;
        const auto scanResult=Core::AeroLib::ExecutorLibcSscanf(
            allocation,allocation+32,allocation+96,0,0,0);
        std::fprintf(stderr,"[APPLE_SCAN_TEST] direct=%lld value=%u\n",scanResult,*scanned);
        Require(scanResult==1 && *scanned==314159,"Host-owned scanf arguments lost at guest memory boundary");
        const auto scanMemory=Core::AeroLib::ExecutorLibcMalloc(1024);
        Require(scanMemory!=0,"Scan fixture allocation failed");
        std::memset(reinterpret_cast<void*>(scanMemory),0,1024);
        std::memcpy(reinterpret_cast<void*>(scanMemory),"73 -29 ff abc",14);
        std::memcpy(reinterpret_cast<void*>(scanMemory+64),"%u %d %x %3s",13);
        Require(Core::AeroLib::ExecutorLibcSscanf(scanMemory,scanMemory+64,
                    scanMemory+128,scanMemory+132,scanMemory+136,scanMemory+140)==4 &&
                *reinterpret_cast<unsigned*>(scanMemory+128)==73 &&
                *reinterpret_cast<int*>(scanMemory+132)==-29 &&
                *reinterpret_cast<unsigned*>(scanMemory+136)==255 &&
                std::strcmp(reinterpret_cast<char*>(scanMemory+140),"abc")==0,
                "Host scanf cursor failed to advance across register arguments");
        Require(Core::AeroLib::ExecutorLibcSscanf(allocation,allocation+32,0x10000,0,0,0)!=1,
                "Host-owned scanf descriptor admitted an invalid guest output");
        // The v-variant still consumes and advances a guest-owned SysV list,
        // including arguments beyond the six integer register slots.
        struct ScanVa { unsigned gp,fp; u64 overflow,registers; };
        auto* va=reinterpret_cast<ScanVa*>(scanMemory+256);
        *va={40,48,scanMemory+384,scanMemory+320};
        *reinterpret_cast<u64*>(scanMemory+360)=scanMemory+128;
        *reinterpret_cast<u64*>(scanMemory+384)=scanMemory+132;
        *reinterpret_cast<u64*>(scanMemory+392)=scanMemory+136;
        *reinterpret_cast<u64*>(scanMemory+400)=scanMemory+140;
        std::memset(reinterpret_cast<void*>(scanMemory+128),0,16);
        Require(Core::AeroLib::ExecutorLibcVsscanf(scanMemory,scanMemory+64,scanMemory+256)==4 &&
                va->gp==48 && va->overflow==scanMemory+408 &&
                *reinterpret_cast<unsigned*>(scanMemory+128)==73 &&
                *reinterpret_cast<int*>(scanMemory+132)==-29 &&
                *reinterpret_cast<unsigned*>(scanMemory+136)==255 &&
                std::strcmp(reinterpret_cast<char*>(scanMemory+140),"abc")==0,
                "Guest scanf cursor lost register/overflow argument ownership");
        Require(Core::AeroLib::ExecutorLibcVsscanf(scanMemory,scanMemory+64,0x10000)!=4,
                "Guest scanf cursor accepted an unowned va_list");
        // The file scanner must use the same cursor and preserve stream position.
        const auto scanDirectory=root/"scan-contract";
        std::filesystem::create_directories(scanDirectory);
        auto* mounts=Common::Singleton<Core::FileSys::MntPoints>::Instance();
        mounts->Mount(scanDirectory,"/scan-contract",false);
        Common::Singleton<Core::FileSys::HandleTable>::Instance()->CreateStdHandles();
        std::strcpy(reinterpret_cast<char*>(scanMemory+448),"/scan-contract/input.txt");
        std::memcpy(reinterpret_cast<void*>(scanMemory+480),"w+",3);
        const auto scanFile=Core::AeroLib::ExecutorLibcFopen(scanMemory+448,scanMemory+480);
        Require(scanFile!=0,"Scanner file fixture open failed");
        Require(Core::AeroLib::ExecutorLibcFwrite(scanMemory,1,13,scanFile)==13 &&
                Core::AeroLib::ExecutorLibcFseek(scanFile,0,SEEK_SET)==0,
                "Scanner file fixture write/seek failed");
        std::memset(reinterpret_cast<void*>(scanMemory+128),0,16);
        const auto fileAssignments=Core::AeroLib::ExecutorLibcFscanf(scanFile,scanMemory+64,
            scanMemory+128,scanMemory+132,scanMemory+136,scanMemory+140);
        Require(Core::AeroLib::ExecutorLibcFclose(scanFile)==0,"Scanner file fixture close failed");
        Require(fileAssignments==4 && *reinterpret_cast<unsigned*>(scanMemory+128)==73 &&
                *reinterpret_cast<int*>(scanMemory+132)==-29 &&
                *reinterpret_cast<unsigned*>(scanMemory+136)==255 &&
                std::strcmp(reinterpret_cast<char*>(scanMemory+140),"abc")==0,
                "File scanner failed to preserve host argument ownership");
        mounts->Unmount(scanDirectory,"/scan-contract");
        Core::AeroLib::ExecutorLibcFree(scanMemory);
        std::fprintf(stderr,"[APPLE_SCAN_TEST] direct=passed guest_va=passed file=passed invalid=passed\n");
        Core::AeroLib::ExecutorLibcFree(allocation);
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=hle-allocation-size-contract-passed\n");
        Require(!memory->IsValidMapping(0x10,16384),"Unowned low address unexpectedly accepted");
        TestScopedBacking(memory);
        constexpr u64 page=16384;
        const auto physical=memory->Allocate(0,memory->GetTotalDirectSize(),page,page,0);
        Require(physical!=static_cast<PAddr>(-1),"Shared direct allocation failed");
        void* first{};void* second{};
        Require(memory->MapMemory(&first,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-alias-a",true,physical,page)==0,"First shared alias failed");
        Require(memory->MapMemory(&second,0x800000000ULL,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-alias-rejected",true,physical,page)!=0,
            "Checked direct map unexpectedly allowed an alias");
        Require(memory->MapMemory(&second,0x800000000ULL,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-alias-b",false,physical,page)==0,"Second shared alias failed");
        Require(executor_lsx4_apple_initial_stack_fixture(),"Initial host-owned stack span/read/write contract failed");
        Require(Executor::Jit::ExerciseOwnedGuestStackAccess(),
                "Owned stack span/lease/imported VMA contract failed");
        std::fprintf(stderr,"[APPLE_OWNED_STACK_TEST] bounds=1 overflow=1 nested=1 released=1 importedRejected=1\n");
        bool nativeFaultDescriptorCapacity=false;
        bool nativeSemanticDescriptorStorage=false;
        if(!interpreter) {
            Require(Executor::Jit::ExerciseScalarMoveMemoryContract(reinterpret_cast<u8*>(first),page),
                    "scalar SIMD memory load/store architectural contract");
            Require(Executor::Jit::ExerciseInstructionHelperCallBoundary(reinterpret_cast<u8*>(first),page),
                    "instruction helper guest CALL register state");
            Require(Executor::Jit::ExerciseStaticEdgeAdapter(reinterpret_cast<u8*>(first),page),
                    "Generated static edge to resident adapter contract failed");
            AppleCurrentHleTokenFixture::Run(memory);
            AppleCurrentSignalFixture::Run(memory);
            Require(Executor::Jit::ExerciseNativeSemanticDescriptorStorage(
                reinterpret_cast<u8*>(first),page),"Native semantic descriptor storage/lifetime fixture failed");
            nativeSemanticDescriptorStorage=true;
            Require(Executor::Jit::ExerciseNativeFaultDescriptorCapacity(
                reinterpret_cast<u8*>(first),page),"Native fault descriptor capacity/lifetime fixture failed");
            nativeFaultDescriptorCapacity=true;
        }
        constexpr u64 marker=0x0123456789abcdefULL;
        std::memcpy(first,&marker,sizeof(marker));
        u64 observed{};std::memcpy(&observed,second,sizeof(observed));
        Require(observed==marker,"Shared virtual aliases disagree");
        observed=0;
        Require(memory->TryCopyBackingMemory(reinterpret_cast<VAddr>(second),&observed,sizeof(observed)) && observed==marker,
            "Shared GPU backing read disagrees");
        JitMemoryRouteFixture::Run(memory,first,second,page);
        Require(memory->UnmapMemory(reinterpret_cast<VAddr>(first),page)==0,"First unmap failed");
        Require(JitMemoryRouteFixture::Rejects([&]{
            (void)Executor::Jit::LoadGuestScalar(reinterpret_cast<VAddr>(first),8);
        }),"JIT load accepted an unmapped VMA");
        std::memcpy(&observed,second,sizeof(observed));
        Require(observed==marker,"Unmapping first alias destroyed second");
        Require(memory->UnmapMemory(reinterpret_cast<VAddr>(second),page)==0,"Second unmap failed");
        Require(memory->Free(physical,page,true)==0,"Direct free failed");
        // Exercise the actual multi-GiB reservation used by small GameMaker
        // titles. Touch only its ends and verify both backing/alias semantics;
        // reserving it must not eagerly commit or clear gigabytes of RAM.
        constexpr u64 largeBytes=3ULL<<30;
        const auto footprintBefore=ResidentFootprint();
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-allocate\n");
        const auto largePhysical=memory->Allocate(0,memory->GetTotalDirectSize(),largeBytes,0x200000,3);
        Require(largePhysical!=static_cast<PAddr>(-1),"Large sparse physical allocation failed");
        void* large{};void* endAlias{};
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-map\n");
        Require(memory->MapMemory(&large,Core::DEFAULT_MAPPING_BASE,largeBytes,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-large-sparse",true,largePhysical,0x200000)==0,
            "Owned arena cannot map a 3 GiB GPU pool");
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-touch\n");
        auto* last=static_cast<unsigned char*>(large)+largeBytes-page;
        std::memcpy(large,&marker,sizeof(marker));std::memcpy(last,&marker,sizeof(marker));
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-alias\n");
        Require(memory->MapMemory(&endAlias,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-large-end-alias",false,
            largePhysical+largeBytes-page,page)==0,"Large end alias mapping failed");
        observed=0;std::memcpy(&observed,endAlias,sizeof(observed));
        Require(observed==marker,"Large pool end alias disagrees");
        observed=0;
        Require(memory->TryCopyBackingMemory(reinterpret_cast<VAddr>(large),&observed,sizeof(observed)) &&
            observed==marker,"Large pool backing read disagrees");
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-footprint\n");
        const auto footprintAfter=ResidentFootprint();
        const auto footprintGrowth=footprintAfter>footprintBefore?footprintAfter-footprintBefore:0;
        Require(footprintGrowth<(64ULL<<20),"Sparse pool eagerly consumed physical memory");
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-unmap-alias\n");
        Require(memory->UnmapMemory(reinterpret_cast<VAddr>(endAlias),page)==0,"Large alias unmap failed");
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-unmap\n");
        Require(memory->UnmapMemory(reinterpret_cast<VAddr>(large),largeBytes)==0,"Large pool unmap failed");
        std::fprintf(stderr,"[APPLE_MEMORY_TEST] phase=large-free\n");
        Require(memory->Free(largePhysical,largeBytes,true)==0,"Large pool free failed");
        // Physical free must also tear down live overlapping virtual aliases.
        const auto repeatPhysical=memory->Allocate(0,memory->GetTotalDirectSize(),largeBytes,page,3);
        Require(repeatPhysical!=static_cast<PAddr>(-1),"Large pool could not be allocated again");
        Require(memory->MapMemory(&large,Core::DEFAULT_MAPPING_BASE,largeBytes,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-large-repeat",true,repeatPhysical,page)==0,
            "Large pool could not be mapped again");
        Require(memory->MapMemory(&endAlias,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Direct,"apple-large-repeat-alias",false,
            repeatPhysical+largeBytes-page,page)==0,"Repeated end alias failed");
        Require(memory->Free(repeatPhysical,largeBytes,true)==0,"Physical free with live aliases failed");
        Require(!memory->TryCopyBackingMemory(reinterpret_cast<VAddr>(large),&observed,sizeof(observed)) &&
                !memory->TryCopyBackingMemory(reinterpret_cast<VAddr>(endAlias),&observed,sizeof(observed)),
                "Physical free retained a virtual backing alias");
        // Validate scoped physical backing zeroing used by flexible unmap.
        void* flexible{};
        Require(memory->MapMemory(&flexible,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Flexible,"apple-flexible",false)==0,"Flexible map failed");
        std::memcpy(flexible,&marker,sizeof(marker));
        Require(memory->UnmapMemory(reinterpret_cast<VAddr>(flexible),page)==0,"Flexible unmap failed");
        Require(memory->MapMemory(&flexible,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
            Core::MemoryMapFlags{},Core::VMAType::Flexible,"apple-flexible-repeat",false)==0,"Flexible remap failed");
        std::memcpy(&observed,flexible,sizeof(observed));
        Require(observed==0,"Flexible backing retained released contents");
        Require(memory->UnmapMemory(reinterpret_cast<VAddr>(flexible),page)==0,"Flexible repeat unmap failed");
        FragmentedGuestArenaFixture::Run(memory,heap,Core::EXECUTOR_LIBC_MSPACE_CAPACITY);
        AppleAvPlayerCallbackFixture::Run(memory,root);
        char detail[1024];
        std::snprintf(detail,sizeof(detail),"{\"status\":\"passed\",\"fullRuntimeInitialized\":true,\"appleGpuDetilePixels\":true,\"appleEmptyFragmentStencil\":true,\"appleScalarI64Bounds\":true,\"appleCondWakeContract\":true,\"appleNgsVoiceLifecycle\":true,\"appleAvPlayerCallbacks\":true,\"appleDynamicGuestCallbacks\":true,\"appleCurrentSignalResume\":true,\"exportedHostDataBounds\":true,\"sharedMemoryAliasing\":true,\"scopedBackingLifetime\":true,\"stagedBackingWriteValidation\":true,\"appleCurrentHleTokenRoutes\":%s,\"nativeSemanticDescriptorStorage\":%s,\"nativeFaultDescriptorCapacity\":%s,\"heapBase\":%llu,\"heapCapacity\":%llu,\"largeSparseBytes\":%llu,\"largeSparseFootprintGrowth\":%llu,\"physicalFreeWithLiveAliases\":true,\"flexibleZeroAfterUnmap\":true,\"gameplayTested\":false}",
            nativeFaultDescriptorCapacity?"true":"false",nativeSemanticDescriptorStorage?"true":"false",nativeFaultDescriptorCapacity?"true":"false",
            static_cast<unsigned long long>(heap),static_cast<unsigned long long>(Core::EXECUTOR_LIBC_MSPACE_CAPACITY),
            static_cast<unsigned long long>(largeBytes),static_cast<unsigned long long>(footprintGrowth));
        report=detail;return 0;
    } catch(const std::exception& failure) {
        report=failure.what();return 1;
    }
}
