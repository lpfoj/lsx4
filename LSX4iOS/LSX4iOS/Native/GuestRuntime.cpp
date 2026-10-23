// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestRuntime.h"
#include "GuestAddressSpace.h"
#include "GuestVector.h"
#include "GuestFloatConversion.h"
#include "GuestLibcMath.h"
#include "GuestScalar.h"
#include "GuestPreparedScalar.h"
#include "GuestMutex.h"
#include "GuestFilesystem.h"
#include "GuestNpServices.h"
#include "GuestUserService.h"
#include "GuestSystemModules.h"
#include "GuestSystemService.h"
#include "GuestCommonDialog.h"
#include "GuestSaveData.h"
#include "GuestDirectMemory.h"
#include "GuestGnm.h"
#include "GuestPad.h"
#include "GuestAudio.h"
#include "GuestVideoOut.h"
#include "GuestGpu.h"
#include "GuestThreadAttributes.h"
#include "GuestThreads.h"
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "GuestNative.h"
#endif
#include "common/x86_decoder.h"
#include "core/aerolib/aerolib.h"
#include "executor/dynamic_translation/scalar_semantics.h"
#include "executor/dynamic_translation/vector_semantics.h"
#include "executor/dynamic_translation/x87_semantics.h"
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace Lsx4::Apple {
using namespace Lsx4::Translation;
namespace {
constexpr std::uint64_t ImageBase = 0x100000000;
constexpr std::uint64_t StackBase = 0x7fffff000000;
constexpr std::uint64_t StackBytes = 8 * 1024 * 1024;
constexpr std::uint64_t ExitAddress = 0x700000000000;
constexpr std::uint64_t ImportBase = ExitAddress + 0x1000;
constexpr std::uint64_t KernelData = 0x600000000000;
constexpr std::uint64_t TcbAddress = 0x600100000000;
constexpr std::uint64_t InitReturn = ExitAddress+16;



std::string DescribeImport(const GuestSymbol& symbol) {
    const auto nid=symbol.name.substr(0,symbol.name.find('#'));
    const auto* known=Core::AeroLib::FindByNid(nid.c_str());
    return known ? std::string(known->name)+" ["+symbol.name+"]" : symbol.name;
}
std::string SymbolIdentity(const GuestImage& image,const GuestSymbol& symbol) {
    const auto first=symbol.name.find('#'),last=symbol.name.rfind('#');
    if (first==std::string::npos) return symbol.name;
    if (first==last) throw std::runtime_error("Malformed imported symbol identity");
    auto decode=[](std::string_view text) {
        constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
        unsigned id=0;
        if (text.empty() || text.size()>3) throw std::runtime_error("Invalid symbol ID");
        for(char c:text) {const auto value=alphabet.find(c);
            if(value==std::string_view::npos) throw std::runtime_error("Invalid symbol ID character");
            id=(id<<6)|value;
        }
        if(id>UINT16_MAX) throw std::runtime_error("Symbol ID overflow");
        return id;
    };
    auto find=[](unsigned id,const auto& imports,const auto& exports) {
        for(const auto* list:{&imports,&exports})
            for(const auto& item:*list) if(item.id==id) return item.name;
        throw std::runtime_error("Symbol refers to missing module/library metadata");
    };
    const auto library=find(decode(std::string_view(symbol.name).substr(first+1,last-first-1)),
                            image.importedLibraries,image.exportedLibraries);
    const auto module=find(decode(std::string_view(symbol.name).substr(last+1)),
                           image.importedModules,image.exportedModules);
    return symbol.name.substr(0,first)+"#"+library+"#"+module;
}
struct Module {std::string name; GuestImage image; std::uint64_t base{};};
std::string Disassemble(std::span<const std::uint8_t> bytes,std::uint64_t address) {
    X86Decoder decoder{}; X86DecodedInstruction ins{};
    X86DecoderInit(&decoder,64,64);
    std::ostringstream line; line<<std::hex<<address<<": ";
    if (!X86_SUCCESS(X86DecoderDecodeInstruction(&decoder,nullptr,bytes.data(),bytes.size(),&ins)))
        return line.str()+"<decode failed>";
    X86Formatter formatter{};X86FormatterInit(&formatter,X86_FORMATTER_STYLE_INTEL);
    char text[256]{};
    if (X86FormatterFormatInstruction(&formatter,&ins,nullptr,0,text,sizeof(text),address,nullptr)<0)
        return line.str()+"<format failed>";
    return line.str()+text;
}
}

static GuestRunResult RunModules(std::vector<Module> modules,std::uint64_t limit,bool enableNative=false,const std::filesystem::path& folder={},const GuestFrameSink& frameSink={},std::uint64_t frameLimit=1,bool asynchronousAudio=false,bool validateNative=true,const GuestRunControl* control=nullptr) {
    GuestRunResult result;result.asynchronousAudio=asynchronousAudio;
    result.gpuDigestsRecorded=!control || control->captureGpuDigests;
    result.instructionTraceRecorded=!control || control->captureInstructionTrace;
    if(control && control->IsCancelled()) {result.stop="cancelled";return result;}
    GuestAddressSpace memory;
    CpuFrame frame{};
    struct TraceEntry {std::uint64_t address{};std::array<std::uint8_t,15> bytes{};std::uint8_t length{};};
    std::array<TraceEntry,32> trace;std::uint64_t traceIndex=0;
    struct CachedInstruction {Instruction instruction;bool vectorRoute{};GuestPreparedScalar scalar;
#if TARGET_OS_IPHONE
        GuestNative::Dispatch native;
#endif
    };
    std::unordered_map<std::uint64_t,CachedInstruction> decoded;decoded.reserve(4096);
    struct DecodeSlot {std::uint64_t address{};CachedInstruction* entry{};};
    std::array<DecodeSlot,4096> decodeSlots{};
    std::map<std::string,std::uint64_t> importedAddresses;
    std::unordered_map<std::uint64_t,std::string> boundaries;boundaries.reserve(1024);
    std::unordered_map<std::uint64_t,std::string> boundaryIdentities;boundaryIdentities.reserve(1024);
    struct Destructor {std::uint64_t function,argument,dso;};
    std::vector<Destructor> destructors;
    std::array<std::uint64_t,10> applicationHeap{};
    GuestFilesystem filesystem(folder);
    GuestNpServices npServices;
    GuestUserService userService;
    GuestSystemModules systemModules;
    GuestSystemService systemService(PlatformApplicationState());
    GuestCommonDialog commonDialog;
    GuestDirectMemory directMemory;
    GuestVideoOut videoOut;
    GuestEqueues equeues;
    GuestGnm gnm;
    GuestGpu gpu(result.gpuDigestsRecorded);
    GuestPad pad(PlatformPadInput());
    GuestAudio audio(CreatePlatformAudioSink());
    GuestSaveData saveData(folder.empty()?std::filesystem::path{}:folder.parent_path().parent_path(),folder.filename().string());
    systemModules.AddLoaded("libSceUserService");systemModules.AddLoaded("libSceNpManager");
    systemModules.AddLoaded("libSceCommonDialog");
    systemModules.AddLoaded("libSceSaveData");
    GuestMutexes mutexes;
    GuestThreadAttributes threadAttributes;
    std::unique_ptr<GuestThreads> threads;
    std::uint64_t nextHeapAddress=0x1000000000;
#if TARGET_OS_IPHONE
    GuestNative native(enableNative,validateNative);
    result.nativeValidation=enableNative?(validateNative?"every-instruction":"first-use"):"disabled";
#endif
    auto ip=modules.front().base+modules.front().image.entry;
    try {
        memory.Map({GuestMutexes::StorageBase,6,std::vector<std::uint8_t>(GuestMutexes::StorageBytes)});
        memory.Map({GuestThreadAttributes::StorageBase,6,std::vector<std::uint8_t>(GuestThreadAttributes::StorageBytes)});
        memory.Map({KernelData,6,std::vector<std::uint8_t>(0x4000)});
        // Same kernel export and value as the common desktop HLE. Guest data
        // is kept in guest memory; relocation must never return a host pointer.
        memory.Put(KernelData,0xDEADBEEF54321ABC);
        std::map<std::string,std::uint64_t> exports;
        exports.emplace("f7uOxY9mM1U#libkernel#libkernel",KernelData);
        for (const auto& module:modules) {
            result.modules.push_back(module.name);
            systemModules.AddLoaded(module.name);
            for(const auto& symbol:module.image.symbols) {
                const auto binding=symbol.info>>4;
                if(!symbol.section || (binding!=1 && binding!=2)) continue;
                exports.try_emplace(SymbolIdentity(module.image,symbol),
                    symbol.section==0xfff1 ? symbol.value : module.base+symbol.value);
            }
        }
        for(const auto& [identity,address]:exports)if(IsLibcMath(identity)) {
            boundaries.emplace(address,std::string("libc math HLE ")+identity);
            boundaryIdentities.emplace(address,identity);
        }
        // Standard libc byte operations use checked guest memory, just as the
        // common HLE uses host memcpy/memset. Hook their exported entry points
        // so calls from the bundled libc itself share the same implementation.
        for(const auto& identity:{"Q3VBxCXhUHs#libSceLibcInternal#libSceLibcInternal",
                                  "8zTFvBIAIN8#libSceLibcInternal#libSceLibcInternal",
                                  "Q3VBxCXhUHs#libc#libc","+P6FRGH4LfA#libc#libc","8zTFvBIAIN8#libc#libc"}) {
            if(auto symbol=exports.find(identity);symbol!=exports.end()) {
                boundaries.emplace(symbol->second,std::string("libc memory HLE ")+identity);
                boundaryIdentities.emplace(symbol->second,identity);
            }
        }
        for (auto& module:modules) {
            result.relocations+=module.image.relocations.size();
            module.image.Relocate(module.base,[&](const GuestSymbol& symbol)->std::optional<std::uint64_t> {
                const auto identity=SymbolIdentity(module.image,symbol);
                if (auto found=exports.find(identity);found!=exports.end()) return found->second;
                if ((symbol.info>>4)==2) return std::nullopt;
                auto [it,inserted]=importedAddresses.try_emplace(identity,ImportBase+importedAddresses.size()*16);
                if (inserted) {
                    boundaries.emplace(it->second,DescribeImport(symbol)+" "+identity);
                    boundaryIdentities.emplace(it->second,identity);
                }
                return it->second;
            }, &module-modules.data()+1);
            for (auto& segment:module.image.segments) {
                if (module.base>UINT64_MAX-segment.address) throw std::runtime_error("Guest load base overflow");
                segment.address+=module.base;memory.Map(std::move(segment));
            }
        }
        directMemory.Configure(modules.front().image.processParameters?modules.front().base+modules.front().image.processParameters:0,memory);
        memory.Map({StackBase,6,std::vector<std::uint8_t>(StackBytes)});
        memory.Map({TcbAddress,6,std::vector<std::uint8_t>(0x4000)});
        const auto dtv=TcbAddress+0x100;
        memory.Put(TcbAddress,TcbAddress);memory.Put(TcbAddress+8,dtv);
        memory.Put(TcbAddress+16,TcbAddress+0x1000);
        memory.Put(dtv,1);memory.Put(dtv+8,modules.size());
        for(std::size_t i=0;i<modules.size();++i) {
            const auto& image=modules[i].image;
            if(!image.tlsBytes) continue;
            const auto alignment=std::max<std::uint64_t>(image.tlsAlignment,32);
            const auto size=(image.tlsBytes+alignment-1)&~(alignment-1);
            const auto address=i ? TcbAddress+i*0x8000000 : TcbAddress-size;
            memory.Map({address,6,std::vector<std::uint8_t>(size)});
            if(!memory.Write(address,image.tlsInitial)) throw std::runtime_error("Cannot initialize guest TLS");
            memory.Put(dtv+8*(i+2),address);
        }
        // PS4 EntryParams from the common loader: argc, padding, argv[33], entry.
        const auto params=StackBase+StackBytes-4096;
        const auto argv0=params+512;
        const std::uint8_t executable[]={'e','b','o','o','t','.','b','i','n',0};
        if (!memory.Write(argv0,executable)) throw std::runtime_error("Cannot initialize guest argv");
        memory.Put(params,1); memory.Put(params+8,argv0);memory.Put(params+272,ip);
        const auto stack=params-0x108; // SysV entry RSP is 8 modulo 16.
        memory.Put(stack,1);memory.Put(stack+8,argv0);
        frame.integer[4]=stack; frame.integer[7]=params;frame.integer[6]=ExitAddress;
        frame.condition_word=2;
        frame.fs_origin=TcbAddress;
        const auto mainFrame=frame;
        const auto mainEntry=ip;
        std::vector<std::size_t> initialization;
        std::vector<bool> visited(modules.size());
        std::function<void(std::size_t)> visit=[&](std::size_t index) {
            if(visited[index]) return;
            visited[index]=true;
            for(const auto& dependency:modules[index].image.importedModules)
                for(std::size_t i=1;i<modules.size();++i)
                    for(const auto& exported:modules[i].image.exportedModules)
                        if(dependency.name==exported.name) visit(i);
            if(index && modules[index].image.hasInit) initialization.push_back(index);
        };
        visit(0);
        std::size_t initializing=0;
        auto beginPhase=[&] {
            if(initializing==initialization.size()) {
                memory.Put(stack,1);memory.Put(stack+8,argv0);
                frame=mainFrame;ip=mainEntry;result.phase="entry";return;
            }
            const auto& module=modules[initialization[initializing]];
            frame={};frame.fs_origin=TcbAddress;frame.condition_word=2;frame.integer[4]=stack;
            memory.Put(stack,InitReturn);
            frame.integer[2]=module.image.processParameters ? module.base+module.image.processParameters : 0;
            ip=module.base+module.image.init;result.phase="initialize "+module.name;
        };
        beginPhase();
        const auto port=memory.Port();
        std::vector<GuestTlsTemplate> tls;
        for(const auto& module:modules)tls.push_back({module.image.tlsBytes,module.image.tlsAlignment,module.image.tlsInitial});
        threads=std::make_unique<GuestThreads>(memory,std::move(tls));
        threads->StartMain(frame,ip,StackBase,StackBytes);
        auto* activeThread=&threads->Current();
        auto nextQuantum=GuestThreads::Quantum;
        bool yield=false;
        auto recordImport=[&](const std::string& name) {
            if(result.importCallCounts[name]++==0)result.imports.push_back(name);
        };
        auto completeWait=[&](GuestThreads::Thread& thread,std::uint64_t value,const std::string& name) {
            thread.frame.integer[0]=value;
            thread.ip=memory.Get(thread.frame.integer[4]);thread.frame.integer[4]+=8;
            recordImport(name);
        };
        while (result.instructions<limit) {
            if(yield || result.instructions>=nextQuantum) {
                if(control && control->IsCancelled()) {result.stop="cancelled";break;}
                if(!threads->Switch(frame,ip)) {
                    if(asynchronousAudio && audio.WaitForOutput()) {yield=true;continue;}
                    result.stop=threads->AllExited()?"threads-exited":"guest-thread-wait";
                    if(auto boundary=boundaries.find(ip);boundary!=boundaries.end())result.detail=boundary->second;
                    break;
                }
                activeThread=&threads->Current();yield=false;nextQuantum=result.instructions+GuestThreads::Quantum;
            }
            if(ip==GuestThreads::ThreadReturn) {
                threads->Exit(frame.integer[0]);yield=true;continue;
            }
            if(ip==InitReturn) {
                if(static_cast<std::int32_t>(frame.integer[0])<0) {result.stop="module-init-failed";break;}
                ++initializing;beginPhase();continue;
            }
            if (ip==ExitAddress) {result.stop="exited";break;}
            if (auto boundary=boundaries.find(ip);boundary!=boundaries.end()) {
                const auto& identity=boundaryIdentities.at(ip);
                auto returnValue=[&](std::uint64_t value) {
                    recordImport(boundary->second);
                    frame.integer[0]=value;ip=memory.Get(frame.integer[4]);frame.integer[4]+=8;
                };
                if(IsLibcMath(identity)) {
                    if(ExecuteLibcMath(identity,frame)){returnValue(frame.integer[0]);continue;}
                    // The real libc owns this export. On unsupported libm
                    // error handling, remove only the optimization and execute it.
                    if(memory.Fetch(ip).empty()){result.stop="unimplemented-import";result.detail=boundary->second;break;}
                    boundaryIdentities.erase(ip);boundaries.erase(ip);continue;
                }
                if(identity=="Q3VBxCXhUHs#libSceLibcInternal#libSceLibcInternal" ||
                   identity=="8zTFvBIAIN8#libSceLibcInternal#libSceLibcInternal" ||
                   identity=="Q3VBxCXhUHs#libc#libc" || identity=="+P6FRGH4LfA#libc#libc" || identity=="8zTFvBIAIN8#libc#libc") {
                    const auto destination=frame.integer[7],source=frame.integer[6],length=frame.integer[2];
                    const bool ok=!identity.starts_with("8zTF")?memory.Copy(destination,source,length):memory.Fill(destination,source,length);
                    if(!ok){result.stop="memory-fault";result.faultAddress=memory.lastFault;result.detail=boundary->second;break;}
                    returnValue(destination);continue;
                }
                if(identity=="9BcDykPmo1I#libkernel#libkernel") {
                    // Each Apple guest TCB reserves this cell for guest errno;
                    // neither Darwin errno nor a host TLS pointer is exposed.
                    returnValue(frame.fs_origin+0x3000);continue;
                }
                if(identity=="FxVZqBAA7ks#libkernel#libkernel" || identity=="FN4gaPmuFV8#libkernel#libkernel" || identity=="FN4gaPmuFV8#libScePosix#libkernel") {
                    returnValue(filesystem.PosixWrite(frame.integer[7],frame.integer[6],frame.integer[2],frame.fs_origin+0x3000,memory));continue;
                }
                if(auto file=filesystem.Call(identity,frame.integer[7],frame.integer[6],frame.integer[2],frame.integer[1],memory)) {
                    returnValue(*file);continue;
                }
                if(auto np=npServices.Call(identity,frame.integer[7],frame.integer[6],memory)) {
                    returnValue(*np);continue;
                }
                if(auto user=userService.Call(identity,frame.integer[7],frame.integer[6],frame.integer[2],memory)) {
                    returnValue(*user);continue;
                }
                if(auto module=systemModules.Call(identity,frame.integer[7])) {
                    returnValue(*module);continue;
                }
                if(auto system=systemService.Call(identity,frame.integer[7],memory)) {
                    returnValue(*system);continue;
                }
                if(auto dialog=commonDialog.Call(identity)) {returnValue(*dialog);continue;}
                if(auto save=saveData.Call(identity,frame.integer[7],frame.integer[6],memory,filesystem)) {
                    returnValue(*save);continue;
                }
                if(auto direct=directMemory.Call(identity,frame,memory)) {returnValue(*direct);continue;}
                if(auto queue=equeues.Call(identity,frame,memory)) {
                    if(queue->waiting) {
                        // Timer waits need a clock producer; do not silently turn
                        // a finite timeout into an indefinite wait.
                        if(frame.integer[8]) {result.stop="unimplemented-timed-wait";result.detail=boundary->second;break;}
                        threads->Block([&,identity,name=boundary->second](GuestThreads::Thread& thread) {
                            const auto value=equeues.Call(identity,thread.frame,memory);
                            if(!value || value->waiting)return false;
                            completeWait(thread,value->value,name);return true;
                        });yield=true;continue;
                    }
                    returnValue(queue->value);continue;
                }
                if(auto video=videoOut.Call(identity,frame,memory,equeues)) {returnValue(*video);continue;}
                if(auto packet=gnm.Build(identity,frame,memory,&videoOut)) {returnValue(*packet);continue;}
                if(auto graphic=gnm.Call(identity,frame,equeues)) {returnValue(*graphic);continue;}
                if(identity=="yvZ73uQUqrk#libSceGnmDriver#libSceGnmDriver") {
                    // This route waits for every GPU fence inside submission.
                    // SubmitDone can close the producer batch only when that
                    // processing completed; it never signals unfinished work.
                    if(!gpu.FinishSubmission()){result.stop="gpu-incomplete-submission";result.detail=boundary->second;break;}
                    returnValue(0);continue;
                }
                if(identity=="xbxNatawohc#libSceGnmDriver#libSceGnmDriver") {
                    auto stopped=gpu.SubmitAndFlip(frame,memory,equeues);
                    if(stopped.first=="gpu-unimplemented-presentation" && frameSink)stopped=gpu.Present(frame,memory,equeues,videoOut,frameSink);
                    if(control && control->IsCancelled()) {result.stop="cancelled";break;}
                    if(stopped.first=="gpu-frame-output" && gpu.metrics.framesOutput<frameLimit) {returnValue(0);continue;}
                    result.stop=stopped.first;result.detail=stopped.second;
                    result.imports.push_back(boundary->second);break;
                }
                if(auto controller=pad.Call(identity,frame,memory)) {returnValue(*controller);continue;}
                if(auto output=audio.Call(identity,frame,memory,asynchronousAudio)) {
                    if(auto pending=audio.TakePendingOutput()) {
                        const auto name=boundary->second;
                        threads->Block([&,pending,name](GuestThreads::Thread& thread) {
                            auto completed=audio.FinishOutput(pending);if(!completed)return false;
                            completeWait(thread,*completed,name);return true;
                        });yield=true;
                    } else returnValue(*output);
                    continue;
                }
                if(auto mutex=mutexes.Call(identity,frame.integer[7],frame.integer[6],activeThread->handle,port)) {
                    if(mutex->waiting) {
                        threads->Block([&,identity,name=boundary->second](GuestThreads::Thread& thread) {
                            const auto value=mutexes.Call(identity,thread.frame.integer[7],thread.frame.integer[6],thread.handle,port);
                            if(!value || value->waiting)return false;
                            completeWait(thread,value->code,name);return true;
                        });yield=true;continue;
                    }
                    returnValue(mutex->code);continue;
                }
                if(auto attribute=threadAttributes.Call(identity,frame.integer[7],frame.integer[6],frame.integer[2],port)) {
                    returnValue(*attribute);continue;
                }
                if(identity=="aI+OeCz8xrQ#libkernel#libkernel") {
                    returnValue(activeThread->handle);continue;
                }
                if(identity=="6UgtwV+0zb4#libkernel#libkernel") {
                    const auto attributes=threadAttributes.Get(frame.integer[6],port);
                    if(!attributes) {returnValue(0x80020016);continue;}
                    const auto code=threads->Create(frame.integer[7],*attributes,frame.integer[2],
                                                    frame.integer[1],frame.integer[8],frame);
                    returnValue(code);yield=code==0;continue;
                }
                if(identity=="3kg7rT0NQIs#libkernel#libkernel") {
                    recordImport(boundary->second);threads->Exit(frame.integer[7]);yield=true;continue;
                }
                if(identity=="onNY9Byn-W8#libkernel#libkernel") {
                    const auto joined=threads->Join(frame.integer[7],frame.integer[6]);
                    if(joined.waiting) {
                        threads->Block([&,name=boundary->second](GuestThreads::Thread& thread) {
                            const auto value=threads->Join(thread.frame.integer[7],thread.frame.integer[6]);
                            if(value.waiting)return false;
                            completeWait(thread,value.value,name);return true;
                        });yield=true;continue;
                    }
                    returnValue(joined.value);continue;
                }
                if(identity=="4qGrR6eoP9Y#libkernel#libkernel") {
                    returnValue(threads->Detach(frame.integer[7]));continue;
                }
                if(identity=="T72hz6ffq08#libkernel#libkernel" || identity=="6XG4B33N09g#libkernel#libkernel") {
                    returnValue(frame.integer[0]);yield=true;continue;
                }
                if(identity=="3PtV6p3QNX4#libkernel#libkernel") {
                    returnValue(frame.integer[7]==frame.integer[6]);continue;
                }
                if(identity=="WslcK1FQcGI#libkernel#libkernel") {
                    // The initial iOS machine profile is the base PS4, without
                    // advertising the Neo memory/GPU modes that are not ported.
                    returnValue(0);continue;
                }
                if(identity=="Vo5V8KAwCmk#libSceSystemService#libSceSystemService") {
                    // Same presentation-state transition as SystemService's
                    // g_splash_status in the shared HLE. This does not submit a
                    // game frame or report successful rendering.
                    result.splashVisible=false;returnValue(0);continue;
                }
                if(identity=="x1X76arYMxU#libkernel#libkernel") {
                    const auto* thread=threads->Find(frame.integer[7]);
                    if(!thread) {returnValue(0x80020003);continue;}
                    returnValue(threadAttributes.Set(frame.integer[6],thread->parameters,port)?0:0x80020016);
                    continue;
                }
                if(identity=="959qrazPIrg#libkernel#libkernel") {
                    const auto& main=modules.front();
                    returnValue(main.image.processParameters ? main.base+main.image.processParameters : 0);
                    continue;
                }
                if(identity=="p5EcQeEeJAE#libkernel#libkernel") {
                    // Same ten callbacks as Core::HeapAPI; retain guest entry
                    // addresses for callbacks through this CPU, never cast to host functions.
                    for(std::size_t i=0;i<applicationHeap.size();++i)
                        applicationHeap[i]=memory.Get(frame.integer[7]+i*8);
                    result.heapAPIRegistered=true;
                    returnValue(frame.integer[0]);continue; // void function, RAX unspecified
                }
                if(identity=="NWtTN10cJzE#libSceLibcInternalExt#libSceLibcInternal") {
                    const auto info=frame.integer[7];
                    if(memory.Get(info)!=32) {result.stop="hle-argument-fault";result.detail=boundary->second;break;}
                    // Common HeapInfoInfo layout and 64-slot mstate table.
                    memory.Put32(info+12,0);
                    memory.Put(info+16,KernelData+0x100);
                    memory.Put(info+24,KernelData+0x108);
                    returnValue(frame.integer[0]);continue;
                }
                if(identity=="mL8NDH86iQI#libkernel#libkernel" || identity=="IWIBBdTHit4#libkernel#libkernel") {
                    const auto output=frame.integer[7],length=frame.integer[6],protection=frame.integer[2],flags=frame.integer[1];
                    if(!memory.CanWrite(output,8)) {returnValue(0x8002000e);continue;}
                    if(!length || (length&0x3fff) || (protection&~0x37ull) || (flags&~0x421d93ull)) {
                        returnValue(0x80020016);continue;
                    }
                    if(flags&0x100) {result.stop="unimplemented-memory-reservation";result.detail=boundary->second;break;}
                    if(identity.starts_with("mL8")) {
                        bool terminated=false,valid=true;
                        for(unsigned i=0;i<32;++i) {
                            std::uint8_t c{};
                            if(!memory.Read(frame.integer[8]+i,{&c,1})){valid=false;break;}
                            if(!c){terminated=true;break;}
                        }
                        if(!valid || !terminated){returnValue(valid?0x8002003f:0x8002000e);continue;}
                    }
                    if(length>512*1024*1024 || memory.ResidentBytes()>512*1024*1024-length) {
                        returnValue(0x8002000c);continue;
                    }
                    auto address=memory.Get(output);
                    const bool fixed=(flags&0x10)!=0,noOverwrite=(flags&0x80)!=0;
                    if((fixed || noOverwrite) && (!address || (address&0x3fff))) {returnValue(0x80020016);continue;}
                    if(!fixed && (!address || (address&0x3fff) || !memory.IsFree(address,length))) {
                        if(noOverwrite) {returnValue(0x80020011);continue;}
                        address=nextHeapAddress;
                        while(!memory.IsFree(address,length)) {
                            if(address>0x2000000000-length)throw std::runtime_error("Guest heap address space exhausted");
                            address+=0x4000;
                        }
                    }
                    if(noOverwrite && !memory.IsFree(address,length)) {returnValue(0x80020011);continue;}
                    if(address>UINT64_MAX-length){returnValue(0x80020016);continue;}
                    const auto permissions=std::uint32_t((protection&3?4:0)|(protection&2)|(protection&4?1:0)|(protection&0x30));
                    GuestSegment allocation{address,permissions,std::vector<std::uint8_t>(length)};
                    if(fixed) memory.Unmap(address,length);
                    memory.Map(std::move(allocation));memory.Put(output,address);
                    nextHeapAddress=std::max(nextHeapAddress,address+length);
                    returnValue(0);continue;
                }
                if(identity=="cQke9UuBQOk#libkernel#libkernel" || identity=="teiItL2boFw#libkernel#libkernel") {
                    const auto address=frame.integer[7],length=frame.integer[6];
                    if(!length || (address&0x3fff) || length>UINT64_MAX-0x3fff || address>UINT64_MAX-length-0x3fff) {
                        returnValue(0x80020016);continue;
                    }
                    memory.Unmap(address,(length+0x3fff)&~0x3fffull);returnValue(0);continue;
                }
                if(identity=="fZo48un7LK4#libSceSystemService#libSceSystemService") {
                    std::uint32_t value{};
                    switch(frame.integer[7]) {
                    case 1:value=1;break; // Config::m_language default: English (US).
                    case 2:value=1;break; // DD/MM/YYYY, common emulator default.
                    case 3:value=1;break; // 24-hour time.
                    case 7:value=0;break; // Parental level off.
                    case 1000:value=1;break; // Cross confirms.
                    default:result.stop="unimplemented-system-parameter";result.detail=boundary->second;break;
                    }
                    if(!result.stop.empty())break;
                    memory.Put32(frame.integer[6],value);returnValue(0);continue;
                }
                if(identity=="vNe1w4diLCs#libkernel#libkernel") {
                    const auto module=memory.Get(frame.integer[7]);
                    const auto offset=memory.Get(frame.integer[7]+8);
                    if(!module || module>modules.size() || offset>=modules[module-1].image.tlsBytes) {
                        result.stop="hle-argument-fault";result.detail=boundary->second;break;
                    }
                    const auto threadDtv=memory.Get(frame.fs_origin+8);
                    returnValue(memory.Get(threadDtv+(module+1)*8)+offset);continue;
                }
                if(identity=="tsvEmnenz48#libSceLibcInternal#libSceLibcInternal") {
                    if(memory.Fetch(frame.integer[7]).empty()) {
                        result.stop="hle-argument-fault";result.detail=boundary->second;break;
                    }
                    destructors.push_back({frame.integer[7],frame.integer[6],frame.integer[2]});
                    returnValue(0);
                    continue;
                }
                result.stop="unimplemented-import";result.detail=boundary->second;
                result.imports.push_back(boundary->second);break;
            }
            const auto code=memory.Fetch(ip);
            if (code.empty()) {result.stop="execute-fault";result.faultAddress=ip;break;}
            // Cache decoding and formatting, but validate the actual encoding
            // on every fetch so writable guest code and remapping remain safe.
            auto& slot=decodeSlots[(ip>>1)&(decodeSlots.size()-1)];
            if(!slot.entry || slot.address!=ip) {
                if(decoded.size()>=65536 && !decoded.contains(ip)) {decoded.clear();decodeSlots={};}
                slot={ip,&decoded[ip]};
            }
            auto& cached=*slot.entry;
            if(!cached.instruction.length || code.size()<cached.instruction.length ||
               !std::equal(cached.instruction.encoding_bytes.begin(),cached.instruction.encoding_bytes.begin()+cached.instruction.length,code.begin())) {
                Instruction instruction{};cached.vectorRoute=false;
#if TARGET_OS_IPHONE
                cached.native={};
#endif
                if(DecodeInstruction(code,ip,instruction)!=DecodeOutcome::Complete) {
                    if(result.instructionTraceRecorded) {
                    auto& item=trace[traceIndex++%trace.size()];item.address=ip;item.length=std::min<std::size_t>(code.size(),15);
                    std::copy_n(code.begin(),item.length,item.bytes.begin());
                    }
                    result.stop="decode-failure";break;
                }
                cached.instruction=instruction;
                cached.scalar=control && !control->preparedScalar?GuestPreparedScalar{}:GuestPreparedScalar::Prepare(instruction);
            }
            const auto& instruction=cached.instruction;
            if(result.instructionTraceRecorded) {
                auto& traceItem=trace[traceIndex++%trace.size()];
                traceItem={ip,instruction.encoding_bytes,instruction.length};
            }
            ++result.instructions;
            ++activeThread->instructions;
#if TARGET_OS_IPHONE
            std::uint64_t nativeNext{};
            const auto nativeStep=native.Execute(instruction,frame,nativeNext,cached.native);
            if(nativeStep==NativeStep::Mismatch) {result.stop="native-state-mismatch";break;}
            if(nativeStep==NativeStep::Executed) {ip=nativeNext;continue;}
#endif
            memory.lastFault=0;
            // Once an encoding succeeds through vector semantics, avoid repeating
            // a scalar dispatch known to reject it. The bytes are still checked
            // above on every fetch; an unsupported vector result retries the
            // original route rather than assuming support for new input state.
            auto step=cached.scalar.Execute(frame,memory);
            if(step.stop==SemanticStop::Unsupported && cached.vectorRoute) {
                const auto vector=ExecuteGuestVector(instruction,frame,port);
                if(vector.stop==VectorStop::Continue)step={SemanticStop::Continue,vector.next_address};
                else if(vector.stop==VectorStop::MemoryFault)step={SemanticStop::MemoryFault,0,vector.fault_address};
                else cached.vectorRoute=false;
            }
            if(step.stop==SemanticStop::Unsupported)step=ExecuteGuestScalar(instruction,frame,port);
            if (step.stop==SemanticStop::Unsupported) {
                const auto vector=ExecuteGuestVector(instruction,frame,port);
                if (vector.stop==VectorStop::Continue) {step={SemanticStop::Continue,vector.next_address};cached.vectorRoute=true;}
                else if (vector.stop==VectorStop::MemoryFault) step={SemanticStop::MemoryFault,0,vector.fault_address};
                else {
                    const auto x87=ExecuteX87Instruction(instruction,frame,port);
                    if (x87.stop==X87Stop::Continue) step={SemanticStop::Continue,x87.next_address};
                    else if (x87.stop==X87Stop::MemoryFault) step={SemanticStop::MemoryFault,0,x87.fault_address};
                    else if (x87.stop==X87Stop::StackFault) {result.stop="x87-stack-fault";break;}
                }
            }
            if (step.stop==SemanticStop::MemoryFault) {
                result.stop="memory-fault";result.faultAddress=memory.lastFault?memory.lastFault:step.fault_address;
                if(auto boundary=boundaries.find(memory.lastFault);boundary!=boundaries.end()) {
                    result.stop="unimplemented-import-data";result.detail=boundary->second;
                }
                break;
            }
            if (step.stop==SemanticStop::Unsupported) {result.stop="unsupported-instruction";break;}
            if (step.stop==SemanticStop::Retry) {result.stop="atomic-retry";break;}
            ip=step.next_address;
        }
        if (result.stop.empty()) result.stop="instruction-limit";
    } catch(const GuestDivideError& error) {result.stop="divide-error";result.detail=error.what();}
    catch(const GuestSIMDFault& error) {result.stop="simd-exception";result.detail=error.what();}
    catch(const std::exception& error) {result.stop=result.instructions?"runtime-error":"loader-error";result.detail=error.what();}
    result.openedPaths=filesystem.openedPaths;result.attemptedPaths=filesystem.attemptedPaths;
    result.gpuSubmissions=gpu.metrics.submissions;result.gpuPackets=gpu.metrics.packets;
    result.gpuDepthDraws=gpu.metrics.depthDraws;result.gpuColorDraws=gpu.metrics.colorDraws;result.gpuEndOfPipes=gpu.metrics.endOfPipes;result.gpuFramesOutput=gpu.metrics.framesOutput;result.gpuSubmissionsDone=gpu.metrics.submissionsDone;result.gpuFrameDigest=gpu.metrics.frameDigest;result.gpuVisibleDraws=gpu.metrics.visibleDraws;result.gpuSurfaceTransfers=gpu.metrics.surfaceTransfers;
    result.gpuComputeDispatches=gpu.metrics.computeDispatches;result.gpuBytesWritten=gpu.metrics.bytesWritten;
    result.gpuWriteDigest=gpu.metrics.writeDigest;
    result.audioPendingBuffers=audio.PendingCount();
    result.padSamplesRead=pad.samplesRead;result.padNonNeutralSamplesRead=pad.nonNeutralSamplesRead;result.padButtonsReadMask=pad.buttonsReadMask;
    result.audioBuffers=audio.diagnostics.completedBuffers;result.audioFrames=audio.diagnostics.completedFrames;
    result.audioNonSilentBuffers=audio.diagnostics.nonSilentBuffers;result.audioDeviceFailures=audio.diagnostics.deviceFailures;
    result.consoleBytes=filesystem.consoleBytes;result.consoleOutput=filesystem.consoleOutput;
    result.bytesRead=filesystem.bytesRead;result.bytesWritten=filesystem.bytesWritten;
    result.address=ip;result.registers=frame.integer;
    if(threads) {
        result.activeThread=threads->Current().handle;
        result.contextSwitches=threads->Switches();result.threadsCreated=threads->All().size();
        for(const auto& [handle,thread]:threads->All()) {
            std::ostringstream line;line<<std::hex<<handle<<":"<<thread.name<<":"<<std::dec<<thread.instructions;
            result.threads.push_back(line.str());
        }
    }
    result.conditionWord=frame.condition_word;result.simdControl=frame.simd_control;
    constexpr char digits[]="0123456789abcdef";
    for(const auto& value:frame.vectors) {
        std::string text;text.reserve(64);
        for(auto byte:value){text+=digits[byte>>4];text+=digits[byte&15];}
        result.vectorRegisters.push_back(std::move(text));
    }
    result.registeredDestructors=destructors.size();
#if TARGET_OS_IPHONE
    result.nativeInstructions=native.executions;result.nativeCompilations=native.compilations;result.nativeMismatches=native.mismatches;result.nativeCheckedInstructions=native.checkedExecutions;
#endif
    const auto count=std::min<std::uint64_t>(traceIndex,trace.size());
    for(std::uint64_t i=0;i<count;++i) {
        const auto& item=trace[(traceIndex-count+i)%trace.size()];
        result.trace.push_back(Disassemble(std::span(item.bytes).first(item.length),item.address));
    }
    return result;
}

GuestRunResult RunGuestEntry(GuestImage image,std::uint64_t limit,const GuestRunControl* control) {
    const auto base=image.type==2 || image.type==0xfe00 ? 0 : ImageBase;
    std::vector<Module> modules;
    modules.push_back({"eboot.bin",std::move(image),base});
    return RunModules(std::move(modules),limit,false,{},{},1,false,true,control);
}

GuestRunResult RunGuestTitle(const std::filesystem::path& folder,std::uint64_t limit,bool native,GuestFrameSink frameSink,std::uint64_t frameLimit,bool asynchronousAudio,bool validateNative,const GuestRunControl* control) {
    try {
        if(control && control->IsCancelled()) {GuestRunResult result;result.stop="cancelled";return result;}
        auto load=[](const std::filesystem::path& path) {
            if (std::filesystem::file_size(path)>512ull*1024*1024) throw std::runtime_error("Executable exceeds loader size limit");
            std::ifstream input(path,std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open guest executable");
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
            return GuestImage::Parse(bytes);
        };
        auto main=load(folder/"eboot.bin");
        const auto base=main.type==2 || main.type==0xfe00 ? 0 : ImageBase;
        std::vector<Module> modules;
        modules.push_back({"eboot.bin",std::move(main),base});
        const auto moduleFolder=folder/"sce_module";
        if(std::filesystem::is_directory(moduleFolder)) {
            std::vector<std::filesystem::path> paths;
            for(const auto& item:std::filesystem::directory_iterator(moduleFolder))
                if(item.is_regular_file() && (item.path().extension()==".prx" || item.path().extension()==".sprx"))
                    paths.push_back(item.path());
            std::sort(paths.begin(),paths.end());
            for(const auto& path:paths) {
                if(control && control->IsCancelled()) {GuestRunResult result;result.stop="cancelled";return result;}
                auto image=load(path);
                // Separate 1 GiB guest windows; Map validates actual extents.
                modules.push_back({path.filename().string(),std::move(image),ImageBase+modules.size()*0x40000000});
            }
        }
        return RunModules(std::move(modules),limit,native,folder,frameSink,frameLimit,asynchronousAudio,validateNative,control);
    } catch(const std::exception& error) {
        GuestRunResult result;result.stop="loader-error";result.detail=error.what();return result;
    }
}
}
