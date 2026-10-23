// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestRuntime.h"
#include <sstream>
namespace Lsx4::Apple {
namespace {
std::string Quote(const std::string& value) {
    std::string out="\"";
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') {out+='\\';out+=c;}
        else if(c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=c;
    }
    return out+'"';
}
}
std::string GuestRunResult::Json() const {
    std::ostringstream out;
    out<<"{\"stop\":"<<Quote(stop)<<",\"detail\":"<<Quote(detail)<<",\"phase\":"<<Quote(phase)
       <<",\"instructions\":"<<instructions<<",\"relocations\":"<<relocations
       <<",\"address\":"<<address<<",\"faultAddress\":"<<faultAddress
       <<",\"registeredDestructors\":"<<registeredDestructors
       <<",\"heapAPIRegistered\":"<<(heapAPIRegistered?"true":"false")<<",\"iosPlayable\":false";
    out<<",\"nativeInstructions\":"<<nativeInstructions<<",\"nativeCompilations\":"<<nativeCompilations
       <<",\"nativeMismatches\":"<<nativeMismatches
       <<",\"nativeCheckedInstructions\":"<<nativeCheckedInstructions
       <<",\"nativeValidation\":"<<Quote(nativeValidation);
    out<<",\"conditionWord\":"<<conditionWord<<",\"simdControl\":"<<simdControl;
    out<<",\"padSamplesRead\":"<<padSamplesRead<<",\"padNonNeutralSamplesRead\":"<<padNonNeutralSamplesRead<<",\"padButtonsReadMask\":"<<padButtonsReadMask;
    out<<",\"bytesRead\":"<<bytesRead<<",\"bytesWritten\":"<<bytesWritten;
    out<<",\"consoleBytes\":"<<consoleBytes<<",\"consoleOutput\":"<<Quote(consoleOutput);
    out<<",\"splashVisible\":"<<(splashVisible?"true":"false");
    out<<",\"audioBuffers\":"<<audioBuffers<<",\"audioFrames\":"<<audioFrames
       <<",\"audioNonSilentBuffers\":"<<audioNonSilentBuffers<<",\"audioDeviceFailures\":"<<audioDeviceFailures<<",\"audioPendingBuffers\":"<<audioPendingBuffers<<",\"asynchronousAudio\":"<<(asynchronousAudio?"true":"false");
    out<<",\"threadsCreated\":"<<threadsCreated<<",\"contextSwitches\":"<<contextSwitches<<",\"activeThread\":"<<activeThread;
    out<<",\"gpuSubmissions\":"<<gpuSubmissions<<",\"gpuPackets\":"<<gpuPackets
       <<",\"gpuDepthDraws\":"<<gpuDepthDraws<<",\"gpuColorDraws\":"<<gpuColorDraws<<",\"gpuEndOfPipes\":"<<gpuEndOfPipes<<",\"gpuFramesOutput\":"<<gpuFramesOutput<<",\"gpuSubmissionsDone\":"<<gpuSubmissionsDone<<",\"gpuFrameDigest\":"<<gpuFrameDigest<<",\"gpuVisibleDraws\":"<<gpuVisibleDraws
       <<",\"gpuSurfaceTransfers\":"<<gpuSurfaceTransfers
       <<",\"gpuComputeDispatches\":"<<gpuComputeDispatches<<",\"gpuBytesWritten\":"<<gpuBytesWritten
       <<",\"gpuWriteDigest\":"<<gpuWriteDigest
       <<",\"gpuDigestsRecorded\":"<<(gpuDigestsRecorded?"true":"false")
       <<",\"instructionTraceRecorded\":"<<(instructionTraceRecorded?"true":"false");
    for(const auto& [name,values]:std::initializer_list<std::pair<const char*,const std::vector<std::string>*>>{
            {"vectorRegisters",&vectorRegisters},{"openedPaths",&openedPaths},{"attemptedPaths",&attemptedPaths},{"trace",&trace},{"imports",&imports},{"modules",&modules},{"threads",&threads}}) {
        out<<",\""<<name<<"\":[";
        bool comma=false;for(const auto& value:*values) {if(comma)out<<',';out<<Quote(value);comma=true;}out<<']';
    }
    out<<",\"importCallCounts\":{";
    bool firstCount=true;
    for(const auto& [name,count]:importCallCounts){if(!firstCount)out<<',';out<<Quote(name)<<':'<<count;firstCount=false;}
    out<<'}';
    out<<",\"registers\":[";
    for(std::size_t i=0;i<registers.size();++i){if(i)out<<',';out<<registers[i];}
    return out.str()+"]}";
}
}
