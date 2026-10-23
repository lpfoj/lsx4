// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestAudio.h"
#include <cmath>
namespace Lsx4::Apple {
GuestAudio::~GuestAudio(){
    // Jobs own converted PCM; drain them before destroying the output device.
    for(auto& port:ports_)if(port && port->pending && port->pending->playback.valid())port->pending->playback.wait();
    if(sink_)for(const auto& port:ports_)if(port)sink_->Close(port->handle);
}
std::optional<std::uint64_t> GuestAudio::FinishOutput(const PendingOutput& job) {
    if(job->consumed)return job->value;
    if(job->playback.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return std::nullopt;
    const bool success=job->playback.get();job->consumed=true;
    if(success) {
        ++diagnostics.completedBuffers;diagnostics.completedFrames+=job->frames;
        diagnostics.nonSilentBuffers+=job->nonSilent;job->value=job->frames*job->channels;
    } else {++diagnostics.deviceFailures;job->value=0x80260012;}
    auto& port=ports_[job->handle&255];
    if(port && port->pending==job)port->pending.reset();
    return job->value;
}
bool GuestAudio::WaitForOutput() {
    for(const auto& port:ports_)if(port && port->pending) {
        port->pending->playback.wait_for(std::chrono::milliseconds(2));return true;
    }
    return false;
}
std::uint64_t GuestAudio::PendingCount() const {
    std::uint64_t count=0;for(const auto& port:ports_)count+=port && bool(port->pending);return count;
}
std::optional<std::uint64_t> GuestAudio::Call(std::string_view identity,const Translation::CpuFrame& frame,GuestAddressSpace& memory,bool asynchronous) {
    using namespace Libraries::AudioOut;
    constexpr std::string_view suffix="#libSceAudioOut#libSceAudioOut";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    const auto a=frame.integer[7],b=frame.integer[6],c=frame.integer[2],d=frame.integer[1],e=frame.integer[8],f=frame.integer[9];
    if(nid=="JfEPXVxhFqA") {
        if(initialized_)return 0;
        if(!sink_ || !sink_->Initialize()) {++diagnostics.deviceFailures;return 0x80260012;}
        initialized_=true;return 0;
    }
    const bool known=nid=="ekNvsT22rsY" || nid=="s1--uE9mBFw" || nid=="b+uAV89IlxE" ||
        nid=="GrQ9s4IrNaQ" || nid=="QOQtbeDqsT4";
    if(!known)return std::nullopt;
    if(!initialized_)return 0x8026000f;
    if(nid=="ekNvsT22rsY") {
        const auto type=std::uint32_t(b),frames=std::uint32_t(d),parameters=std::uint32_t(f);
        if(!frames || frames>2048 || frames%256)return 0x80260006;
        if(std::uint32_t(e)!=48000)return 0x80260008;
        if(std::uint32_t(c)!=0)return 0x80260015;
        unsigned first,last;
        switch(type) {
            case 0:first=0;last=7;break;case 1:first=8;last=8;break;case 2:first=9;last=12;break;
            case 3:first=13;last=16;break;case 4:first=17;last=20;break;
            case 126:first=22;last=22;break;case 127:first=23;last=23;break;
            default:return 0x8026000a;
        }
        const auto format=parameters&255,attribute=(parameters>>16)&15;
        if(format>7 || attribute>2 || (parameters&0x7ff0ff00u) || (attribute==2 && type!=3))return 0x80260007;
        // Restricted and mix-to-main ports require the shared multi-port
        // scheduler; retain an explicit boundary until that route is connected.
        if(attribute)return std::nullopt;
        unsigned id=first;while(id<=last && ports_[id])++id;if(id>last)return 0x80260005;
        Port port;port.type=type;port.frames=frames;port.handle=0x20000000|(type<<16)|id;
        port.format={format==3 || format==4 || format==5 || format==7?AudioSampleType::Float32:AudioSampleType::S16,
                     std::uint8_t(format==0 || format==3?1:format==1 || format==4?2:8),format>=6};
        if(!sink_->Open(port.handle)) {++diagnostics.deviceFailures;return 0x80260012;}
        ports_[id]=port;return port.handle;
    }
    const auto handle=std::uint32_t(a),id=handle&255;
    if(id>=ports_.size() || (handle&0x3f000000)!=0x20000000)return 0x80260003;
    if(!ports_[id] || ports_[id]->handle!=handle)return 0x80260001;
    auto& port=*ports_[id];
    if(nid=="s1--uE9mBFw") {
        // A concurrent close cannot destroy a player still consuming owned PCM.
        if(port.pending) {const auto job=port.pending;if(!FinishOutput(job))return 0x80260002;}
        sink_->Close(handle);ports_[id].reset();return 0;
    }
    if(nid=="b+uAV89IlxE") {
        if(b&~255ull)return 0x80260013;
        if(!c || !memory.CanRead(c,32))return 0x80260004;
        auto gains=port.gains;
        for(unsigned channel=0;channel<8;++channel)if(b&(1u<<channel)) {
            std::int32_t volume;
            if(!memory.Read(c+channel*4,{reinterpret_cast<std::uint8_t*>(&volume),4}))return 0x80260004;
            if(volume<0 || volume>32768)return 0x80260009;
            gains[channel]=float(volume)/32768.f;
        }
        port.gains=gains;return 0;
    }
    if(nid=="GrQ9s4IrNaQ") {
        if(!b || !memory.CanWrite(b,32))return 0x80260004;
        std::array<std::uint8_t,32> state{};
        state[0]=port.type==2 || port.type==3?64:port.type==4?4:port.type==127?128:1;
        state[2]=port.type==127?0:port.type>=2 && port.type<=4?1:std::min<unsigned>(2,port.format.channels);
        state[4]=port.type==4?127:255;state[5]=port.type==4?0:255;
        memory.Write(b,state);return 0;
    }
    if(port.pending)return 0x80260002; // One outstanding buffer per port.
    if(!b)return 0;
    const auto bytes=port.frames*port.format.channels*(port.format.sample_type==AudioSampleType::Float32?4:2);
    std::vector<std::uint8_t> pcm(bytes);
    if(!memory.Read(b,pcm))return 0x80260004;
    std::vector<float> stereo(port.frames*2);
    (void)ConvertAudioBufferToFloatPerChannel(pcm.data(),port.frames,port.format,2,port.gains,stereo,port.conversion);
    if(asynchronous) {
        auto job=std::make_shared<OutputJob>();job->handle=handle;job->frames=port.frames;job->channels=port.format.channels;
        job->nonSilent=std::ranges::any_of(stereo,[](float value){return std::isfinite(value) && value!=0;});
        // Only the bounded sink call leaves the executor. Guest memory, ports,
        // conversion state and diagnostics remain owned by the guest executor.
        try {
            job->playback=std::async(std::launch::async,[sink=sink_.get(),handle,pcm=std::move(stereo)] {
                try {return sink->Output(handle,pcm);}catch(...){return false;}
            });
        } catch(const std::system_error&) {return 0x80260011;}
        port.pending=job;pendingOutput_=job;return 0;
    }
    if(!sink_->Output(handle,stereo)) {++diagnostics.deviceFailures;return 0x80260012;}
    ++diagnostics.completedBuffers;diagnostics.completedFrames+=port.frames;
    diagnostics.nonSilentBuffers+=std::ranges::any_of(stereo,[](float value){return std::isfinite(value) && value!=0;});
    return port.frames*port.format.channels;
}
}
