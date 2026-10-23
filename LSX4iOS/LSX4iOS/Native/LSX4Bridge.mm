// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestPad.h"
#import "LSX4Bridge.h"
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include "AppleCodeMemory.h"
#include "AppleVulkanProbe.h"
#include "GuestImage.h"
#include "GuestRuntime.h"
#include "AppleFrameSink.h"
#include "GuestSaveData.h"
#include "GuestAudio.h"
#include "GuestSystemService.h"
#include "SharedCpuProbe.h"
#include "common/x86_decoder.h"
#include "executor/dynamic_translation/native_encoder.h"
#include "executor/dynamic_translation/operation_ir.h"
#include "executor/dynamic_translation/integer_math.h"

using namespace Lsx4::Translation;

extern "C" __attribute__((noinline, used, visibility("default")))
void LSX4DebugReportReady() { asm volatile("" ::: "memory"); }

static bool DebuggerAttached() {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc process{};
    size_t size = sizeof(process);
    return sysctl(mib, 4, &process, &size, nullptr, 0) == 0 &&
           (process.kp_proc.p_flag & P_TRACED) != 0;
}

static NSDictionary *Result(NSString *status, NSString *message) {
    return @{ @"status": status, @"message": message };
}
static std::mutex guestExecutionMutex;
static NSDictionary* ExecutionReport(const Lsx4::Apple::GuestRunResult& result,
                                    std::chrono::steady_clock::time_point start) {
    const auto json=result.Json();
    NSData* data=[NSData dataWithBytes:json.data() length:json.size()];
    NSError* error=nil;
    NSDictionary* report=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if(!report)return Result(@"failed",error.localizedDescription?:@"Cannot encode guest execution report.");
    NSMutableDictionary* timed=[report mutableCopy];
    timed[@"executionWallSeconds"]=@(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    return timed;
}

@implementation LSX4GameSession {
    Lsx4::Apple::GuestRunControl _control;
    std::atomic<bool> _started;
}
- (instancetype)init {
    if((self=[super init])) {
        _started.store(false);_control.captureGpuDigests=false;_control.captureInstructionTrace=false;
    }
    return self;
}
- (void)cancel {_control.Cancel();}
- (NSDictionary<NSString*,id>*)runFolder:(NSString*)folder {
    if(_started.exchange(true))return @{@"stop":@"session-already-used"};
    std::unique_lock lock(guestExecutionMutex,std::try_to_lock);
    if(!lock.owns_lock())return @{@"stop":@"another-game-running"};
    const auto start=std::chrono::steady_clock::now();
    const auto* control=&_control;
    auto sink=[control](const Lsx4::Apple::GuestFrame& frame) {
        return Lsx4::Apple::PresentAppleFrameControlled(frame,control);
    };
    // The initial one-instruction JIT is slower than prepared interpretation
    // on the physical iPhone. Keep it opt-in for development comparisons until
    // a block compiler demonstrates a win on matched work.
    const bool native=[NSProcessInfo.processInfo.environment[@"LSX4_GAME_NATIVE"] isEqualToString:@"1"] && DebuggerAttached();
    const auto result=Lsx4::Apple::RunGuestTitle(folder.fileSystemRepresentation,UINT64_MAX,
        native,sink,UINT64_MAX,true,false,control);
    return ExecutionReport(result,start);
}
@end

static bool StoragePersistenceProbe() {
    using namespace Lsx4::Apple;
    const auto root=std::filesystem::path(NSTemporaryDirectory().fileSystemRepresentation)/
        (std::string("LSX4-storage-")+NSUUID.UUID.UUIDString.UTF8String);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}
    } cleanup{root};
    try {
        std::filesystem::create_directories(root/"app");
        // Two separate service/filesystem lifetimes prove persistence outside
        // the first instance. The fixture is isolated from all real game saves.
        for(bool writing:{true,false}) {
            GuestAddressSpace memory;memory.Map({0x1000,6,std::vector<std::uint8_t>(4096)});
            GuestFilesystem filesystem(root/"app");GuestSaveData saves(root,"CUSA00000");
            auto call=[&](const std::string& nid,std::uint64_t a=0,std::uint64_t b=0) {
                return saves.Call(nid+"#libSceSaveData#libSceSaveData",a,b,memory,filesystem).value_or(UINT32_MAX);
            };
            auto file=[&](const std::string& nid,std::uint64_t a,std::uint64_t b=0,std::uint64_t c=0) {
                return filesystem.Call(nid+"#libkernel#libkernel",a,b,c,0,memory).value_or(UINT64_MAX);
            };
            auto string=[&](std::uint64_t address,std::string value) {
                std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);return memory.Write(address,bytes);
            };
            if(call("TywrFKCoLGY")!=0 || !string(0x1100,"probe"))return false;
            memory.Put32(0x1000,1000);memory.Put(0x1008,0x1100);memory.Put(0x1010,96);
            memory.Put32(0x1018,writing?34:1);
            if(call("0z45PIH+SNI",0x1000,0x1200)!=0 || !string(0x1300,"/savedata0/progress.bin"))return false;
            if(!writing && file("1G3lF1Gg1k8",0x1300,0x401)!=0x8002001e)return false;
            const auto fd=file("1G3lF1Gg1k8",0x1300,writing?0x202:0,0600);
            if(fd!=3)return false;
            if(writing)memory.Put(0x1400,0x123456789abcdef0);
            if(file(writing?"4wSze92BhLI":"Cg4srZ6TKbU",fd,0x1400,8)!=8)return false;
            if(memory.Get(0x1400)!=0x123456789abcdef0 || file("UK2Tl2DWUns",fd)!=0)return false;
            if(call("BMR4F-Uek3E",0x1200)!=0 || call("yKDy8S5yLA0")!=0)return false;
        }
        return true;
    } catch(const std::exception&) {return false;}
}

@implementation LSX4Bridge
+ (void)updateApplicationActive:(BOOL)active {
    Lsx4::Apple::PlatformApplicationState().SetActive(active);
}
+ (BOOL)isDebuggerAttached { return DebuggerAttached(); }
+ (void)diagnosticsReportReady { LSX4DebugReportReady(); }
+ (NSDictionary<NSString *, id> *)diagnostics {
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
    const auto vulkan=Lsx4::Apple::ProbeVulkanTransfer();
    bool audioCompleted=false;
    {
        auto sink=Lsx4::Apple::CreatePlatformAudioSink();
        if(sink && sink->Initialize() && sink->Open(0x20000000)) {
            const std::array<float,1024> silence{};
            audioCompleted=sink->Output(0x20000000,silence);
            sink->Close(0x20000000);
        }
    }
    ArmWordStream stream;
    stream.PutConstant(ArmRegister::X0, 42);
    stream.Return();
    bool encoder = stream.Finalize() && stream.Words().size() == 2 &&
                   stream.Words()[0] == 0xd2800540u && stream.Words()[1] == 0xd65f03c0u;
    OperationIr ir(0x1000);
    auto left = ir.AddValue(IrAction::Constant, IrType::Integer64, {}, 20);
    auto right = ir.AddValue(IrAction::Constant, IrType::Integer64, {}, 22);
    const IrValue inputs[] = {left, right};
    auto sum = ir.AddValue(IrAction::Add, IrType::Integer64, inputs);
    auto integer = AddWithCarry(0xff, 0, true, 8);
    bool math = integer.effective && integer.value == 0 && (integer.flag_bits & 1) != 0;
    return @{
        @"metal": gpu.name ?: @"Недоступен",
        @"metalDeviceClass": gpu ? NSStringFromClass(gpu.class) : @"Unavailable",
        @"metalAvailable": @(gpu != nil),
        @"audioSilentBufferCompleted": @(audioCompleted),
        @"vulkanTransfer": @{
            @"passed": @(vulkan.passed),
            @"device": [NSString stringWithUTF8String:vulkan.device.c_str()],
            @"apiVersion": @(vulkan.apiVersion),
            @"verifiedPixels": @(vulkan.verifiedPixels),
            @"shaderExecuted": @(vulkan.shaderExecuted),
            @"shaderWords": @(vulkan.shaderWords),
            @"error": [NSString stringWithUTF8String:vulkan.error.c_str()]
        },
        @"pageBytes": @(getpagesize()),
        @"physicalMemory": @([NSProcessInfo processInfo].physicalMemory),
        @"debugger": @(DebuggerAttached()),
        @"encoder": @(encoder),
        @"ir": @(sum.Exists() && ir.Validate()),
        @"integerMath": @(math),
        @"scalarExecution": @(LSX4SharedCpuProbe()),
        @"saveDataPersistence": @(StoragePersistenceProbe()),
        @"simulator": @((bool)TARGET_OS_SIMULATOR),
        @"gameRuntime": @NO
    };
}

+ (NSDictionary<NSString *, id> *)runJITProbe {
#if TARGET_OS_SIMULATOR || !defined(__aarch64__)
    return Result(@"unavailable", @"Проверка JIT требует физический iPhone ARM64.");
#else
    // Unsigned code must only be attempted during this app's authorized Xcode
    // debug session. No entitlement forgery, attach bypass or persistent change.
    if (!DebuggerAttached()) {
        return Result(@"unavailable", @"Запустите эту сборку кнопкой Run в Xcode с подключённым отладчиком и повторите проверку.");
    }
    ArmWordStream stream;
    stream.PutConstant(ArmRegister::X0, 42);
    stream.Return();
    if (!stream.Finalize()) return Result(@"failed", @"Ошибка генерации ARM64.");
    Lsx4::Apple::CodeMemory memory;
    std::string error;
    if (!memory.Allocate(stream.ByteSize(), error)) {
        return Result(@"unavailable", [NSString stringWithUTF8String:error.c_str()]);
    }
    auto writable = memory.Writable();
    if (!stream.CopyTo({reinterpret_cast<uint8_t*>(writable.data()), writable.size()}) ||
        !memory.Publish(stream.ByteSize(), error)) {
        return Result(@"failed", [NSString stringWithUTF8String:error.c_str()]);
    }
    uint64_t value = reinterpret_cast<uint64_t (*)()>(const_cast<void*>(memory.Executable()))();
    // Check the second page too: a helper must prepare every page in a slab.
    Lsx4::Apple::CodeMemory twoPages;
    const size_t pageOffset = static_cast<size_t>(getpagesize());
    if (!twoPages.Allocate(pageOffset + stream.ByteSize(), error))
        return Result(@"failed", [NSString stringWithUTF8String:error.c_str()]);
    auto secondPage = twoPages.Writable().subspan(pageOffset);
    if (!stream.CopyTo({reinterpret_cast<uint8_t*>(secondPage.data()), secondPage.size()}) ||
        !twoPages.Publish(pageOffset + stream.ByteSize(), error))
        return Result(@"failed", [NSString stringWithUTF8String:error.c_str()]);
    auto secondEntry = static_cast<const uint8_t*>(twoPages.Executable()) + pageOffset;
    uint64_t secondValue = reinterpret_cast<uint64_t (*)()>(const_cast<uint8_t*>(secondEntry))();
    NSLog(@"LSX4_IOS_JIT_PROBE result=%llu expected=42", value);
    return @{@"status": value == 42 && secondValue == 42 ? @"passed" : @"failed",
             @"result": @(value), @"secondPageResult": @(secondValue),
             @"message": [NSString stringWithFormat:@"Общий ARM64-генератор LSX4: результат %llu; вторая страница %llu, ожидается 42. Это проверка JIT, не запуск игры.", value, secondValue]};
#endif
}

+ (NSDictionary<NSString *, id> *)inspectExecutable:(NSString *)path {
    NSError *readError = nil;
    NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:&readError];
    if (!attributes || [attributes fileSize] > 512ull * 1024 * 1024)
        return Result(@"failed", readError.localizedDescription ?: @"Executable exceeds the current 512 MiB loader limit.");
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:&readError];
    if (!data) return Result(@"failed", readError.localizedDescription ?: @"Cannot read executable.");
    try {
        auto image = Lsx4::Apple::GuestImage::Parse({static_cast<const uint8_t*>(data.bytes), data.length});
        uint64_t memoryBytes = 0;
        for (const auto& segment : image.segments) memoryBytes += segment.bytes.size();
        X86Decoder decoder{};
        if (!X86_SUCCESS(X86DecoderInit(&decoder, 64, 64))) return Result(@"failed", @"Shared x86 decoder ABI mismatch.");
        X86Formatter formatter{}; X86FormatterInit(&formatter, X86_FORMATTER_STYLE_INTEL);
        NSMutableArray *instructions = [NSMutableArray array];
        uint64_t address = image.entry;
        for (int i = 0; i < 8; ++i) {
            const auto bytes = image.BytesAt(address);
            X86DecodedInstruction instruction{};
            if (!X86_SUCCESS(X86DecoderDecodeInstruction(&decoder, nullptr, bytes.data(), bytes.size(), &instruction)))
                return Result(@"failed", @"Cannot decode the guest entry point.");
            char spelling[256]{};
            if (X86FormatterFormatInstruction(&formatter, &instruction, nullptr, 0, spelling,
                                             sizeof(spelling), address, nullptr) < 0)
                return Result(@"failed", @"Cannot format the guest entry point.");
            [instructions addObject:@{@"address": @(address), @"instruction": [NSString stringWithUTF8String:spelling]}];
            address += instruction.length;
        }
        return @{@"status": @"loaded", @"container": image.self ? @"SELF" : @"ELF64",
                 @"loadSegments": @(image.segments.size()), @"memoryBytes": @(memoryBytes),
                 @"entry": @(image.entry), @"entryInstructions": instructions,
                 @"gameRuntime": @NO, @"iosPlayable": @NO};
    } catch (const std::exception& error) {
        return Result(@"failed", [NSString stringWithUTF8String:error.what()]);
    }
}

+ (void)updatePad:(uint32_t)buttons axes:(NSData*)data connected:(BOOL)connected timestamp:(uint64_t)timestamp
      touchActive:(BOOL)touchActive touchX:(uint16_t)touchX touchY:(uint16_t)touchY {
    if(data.length!=6)return;
    std::array<std::uint8_t,6> axes{};[data getBytes:axes.data() length:axes.size()];
    std::optional<Libraries::Pad::OrbisPadTouch> touch;
    if(touchActive)touch=Libraries::Pad::OrbisPadTouch{touchX,touchY,1,{}};
    Lsx4::Apple::PlatformPadInput().Update(buttons,axes,connected,timestamp,touch);
}

+ (NSDictionary<NSString *, id> *)runGuestTitle:(NSString *)folder {
    std::unique_lock lock(guestExecutionMutex,std::try_to_lock);
    if(!lock.owns_lock())return @{@"stop":@"another-game-running"};
    std::uint64_t limit=Lsx4::Apple::TitleInstructionLimit,frameLimit=1;
#if DEBUG
    NSString* requestedFrames=NSProcessInfo.processInfo.environment[@"LSX4_TEST_FRAME_LIMIT"];
    if(requestedFrames) {
        NSScanner* scanner=[NSScanner scannerWithString:requestedFrames];unsigned long long parsed=0;
        if(![scanner scanUnsignedLongLong:&parsed] || !scanner.isAtEnd || parsed<1 || parsed>10000)
            return Result(@"failed",@"Invalid diagnostic frame limit.");
        frameLimit=parsed;
    }
    NSString *requested=NSProcessInfo.processInfo.environment[@"LSX4_TEST_INSTRUCTION_LIMIT"];
    if (requested) {
        NSScanner *scanner=[NSScanner scannerWithString:requested];
        unsigned long long parsed=0;
        if (![scanner scanUnsignedLongLong:&parsed] || !scanner.isAtEnd || parsed<1 || parsed>10000000000ull)
            return Result(@"failed", @"Invalid diagnostic instruction limit.");
        limit=parsed;
    }
#endif
    const auto executionStart=std::chrono::steady_clock::now();
    Lsx4::Apple::GuestRunControl control;
    control.captureInstructionTrace=![NSProcessInfo.processInfo.environment[@"LSX4_INSTRUCTION_TRACE"] isEqualToString:@"0"];
    control.captureGpuDigests=![NSProcessInfo.processInfo.environment[@"LSX4_GPU_DIGESTS"] isEqualToString:@"0"];
    const bool native=DebuggerAttached() && ![NSProcessInfo.processInfo.environment[@"LSX4_DISABLE_NATIVE"] isEqualToString:@"1"];
    const auto result=Lsx4::Apple::RunGuestTitle(folder.fileSystemRepresentation,limit,native,Lsx4::Apple::PresentAppleFrame,frameLimit,[NSProcessInfo.processInfo.environment[@"LSX4_ASYNC_AUDIO"] isEqualToString:@"1"],![NSProcessInfo.processInfo.environment[@"LSX4_NATIVE_VALIDATE"] isEqualToString:@"first-use"],&control);
    return ExecutionReport(result,executionStart);
}
@end
