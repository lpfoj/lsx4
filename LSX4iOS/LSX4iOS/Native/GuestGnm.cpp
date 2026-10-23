// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestGnm.h"
#include "GuestVideoOut.h"
#include "core/libraries/gnmdriver/gnmdriver_init.h"
#include "../../.build/generated/GnmBuiltins.h"
namespace Lsx4::Apple {
namespace {
std::optional<std::uint32_t> DebugMarker(std::string_view nid,const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    const bool pop=nid=="7qZVNgEu+SY",push=nid=="W1Etj-jlW7Y",set=nid=="jiItzS6+22g";
    if(!pop && !push && !set)return std::nullopt;
    const auto destination=frame.integer[7],source=frame.integer[2];
    const auto words=std::uint32_t(frame.integer[6]);
    if(!destination || words<6 || words>0x4001 || (pop && words!=6) ||
       !memory.CanWrite(destination,words*4))return UINT32_MAX;
    std::vector<std::uint32_t> output(words,0);
    if(!pop) {
        if(!source)return UINT32_MAX;
        std::vector<std::uint8_t> marker;
        const auto capacity=(words-2)*4;
        while(marker.size()<capacity) {
            std::uint8_t byte;
            if(source>UINT64_MAX-marker.size() || !memory.Read(source+marker.size(),{&byte,1}))return UINT32_MAX;
            marker.push_back(byte);if(!byte)break;
        }
        if(marker.empty() || marker.back()!=0)return UINT32_MAX;
        const auto length=marker.size()-1;
        const auto packet=((length+8)>>2)+((length+12)>>3)*2;
        if(packet+2!=words)return UINT32_MAX;
        std::memcpy(output.data()+2,marker.data(),marker.size());
    }
    output[0]=0xc0001000u|((words-2)<<16);
    output[1]=pop?0x68750002:push?0x68750001:0x68750003;
    return memory.Write(destination,{reinterpret_cast<const std::uint8_t*>(output.data()),words*4})?0:UINT32_MAX;
}
std::optional<std::uint32_t> DrawDispatch(std::string_view nid,const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    const bool dispatch=nid=="0BzLGljcwBo",draw=nid=="HlTPoZ-oY7Y",automatic=nid=="GGsn7jMTxw4";
    if(!dispatch && !draw && !automatic)return std::nullopt;
    const unsigned words=dispatch?9:draw?10:7;
    const auto destination=frame.integer[7],indexAddress=frame.integer[1];
    const auto count=std::uint32_t(frame.integer[2]);
    const auto flags=std::uint32_t(frame.integer[dispatch?9:draw?8:1]);
    if(!destination || std::uint32_t(frame.integer[6])!=words || !memory.CanWrite(destination,words*4))return UINT32_MAX;
    if(dispatch) {
        if((count|std::uint32_t(frame.integer[1])|std::uint32_t(frame.integer[8]))&0x80000000u)return UINT32_MAX;
    } else if((flags&0x1ffffffeu) || (draw && (!indexAddress || (indexAddress&1))))return UINT32_MAX;
    std::array<std::uint32_t,10> output;
    auto bytes=std::span(reinterpret_cast<std::uint8_t*>(output.data()),words*4);
    if(!memory.Read(destination,bytes))return UINT32_MAX;
    unsigned cursor;
    // Same base-PS4 packet fields as the shared GNM builders. These functions
    // enqueue no host GPU work and preserve the caller's trailing NOP payload.
    if(dispatch) {
        output[0]=0xc0031502u|(flags&1);output[1]=count;
        output[2]=std::uint32_t(frame.integer[1]);output[3]=std::uint32_t(frame.integer[8]);
        output[4]=(flags&0x18)+1;cursor=5;
    } else if(draw) {
        output[0]=0xc0042700;output[1]=count;output[2]=std::uint32_t(indexAddress);
        output[3]=std::uint32_t(indexAddress>>32);output[4]=count;output[5]=0;cursor=6;
    } else {
        output[0]=0xc0012d00;output[1]=count;output[2]=2;cursor=3;
    }
    output[cursor]=0xc0021000;output[cursor+1]=0;
    return memory.Write(destination,bytes)?0:UINT32_MAX;
}
std::optional<std::uint32_t> ShaderRegisters(std::string_view nid,const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    const bool cs=nid=="KXltnCwEJHQ" || nid=="Kx-h-nWQJ8A",vs=nid=="gAhCn6UiU4Y";
    const bool ps=nid=="bQVd5YzCal0" || nid=="5uFKckiJYRM",ps350=nid=="5uFKckiJYRM";
    if(!cs && !vs && !ps)return std::nullopt;
    const unsigned words=cs?25:vs?29:40;
    const auto destination=frame.integer[7],source=frame.integer[2];
    const auto modifier=std::uint32_t(nid=="KXltnCwEJHQ" || ps?0:frame.integer[1]);
    const auto modifierMask=vs?0xfcfffc3fu:0xfffffc3fu;
    if(!destination || std::uint32_t(frame.integer[6])<words || (modifier&modifierMask) ||
       !memory.CanWrite(destination,words*4) || (!ps && !source))return UINT32_MAX;
    std::array<std::uint32_t,12> registers{};
    if(source && (!memory.Read(source,{reinterpret_cast<std::uint8_t*>(registers.data()),(ps?12u:7u)*4}) || registers[1]))return UINT32_MAX;
    std::array<std::uint32_t,40> output{};
    auto bytes=std::span(reinterpret_cast<std::uint8_t*>(output.data()),words*4);
    if(!memory.Read(destination,bytes))return UINT32_MAX;
    unsigned cursor=0;
    auto set=[&](std::uint32_t opcode,std::uint32_t reg,std::initializer_list<std::uint32_t> values) {
        output.at(cursor++)=0xc0000000u|(std::uint32_t(values.size())<<16)|(opcode<<8);
        output.at(cursor++)=reg;
        for(auto value:values)output.at(cursor++)=value;
    };
    auto sh=[&](std::uint32_t reg,std::initializer_list<std::uint32_t> values){set(0x76,reg,values);};
    auto context=[&](std::uint32_t reg,std::initializer_list<std::uint32_t> values){set(0x69,reg,values);};
    // PM4 register order and masks from the shared sceGnmSet*Shader functions.
    // This writes guest command bytes only; no shader execution is implied.
    const auto rsrc1=modifier?(registers[2]&modifierMask)|modifier:registers[2];
    if(cs) {
        sh(0x20c,{registers[0],0});sh(0x212,{rsrc1,registers[3]});
        sh(0x207,{registers[4],registers[5],registers[6]});
    } else if(vs) {
        sh(0x48,{registers[0],0});sh(0x4a,{rsrc1,registers[3]});
        context(0x207,{registers[6]});context(0x1b1,{registers[4]});context(0x1c3,{registers[5]});
    } else if(!source) {
        sh(8,{0,0});context(0x203,{0});if(ps350)context(0x8f,{15});
    } else {
        sh(8,{registers[0],0});sh(10,{registers[2],registers[3]});
        context(0x1c4,{registers[4],registers[5]});context(0x1b3,{registers[6],registers[7]});
        context(0x1b6,{registers[8]});context(0x1b8,{registers[9]});
        context(0x203,{registers[10]});context(0x8f,{registers[11]});
    }
    output.at(cursor)=0xc0001000u|((words-cursor-2)<<16);output.at(cursor+1)=0;
    return memory.Write(destination,bytes)?0:UINT32_MAX;
}
std::optional<std::uint32_t> EmbeddedShader(std::string_view nid,const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    const bool vs=nid=="+AFvOEXrKJk",ps=nid=="X9Omw9dwv5M";
    if(!vs && !ps)return std::nullopt;
    const auto id=std::uint32_t(frame.integer[2]);
    if(id>(vs?0u:1u))return 0x8eee00ff;
    const unsigned words=vs?29:40;
    if(!frame.integer[7] || std::uint32_t(frame.integer[6])<words ||
       !memory.CanWrite(frame.integer[7],words*4) || !memory.CanRead(frame.integer[7],words*4))return UINT32_MAX;
    const unsigned slot=vs?0:id+1;
    const std::uint64_t address=0x5000000000ull+slot*0x1000;
    using Words=std::span<const std::uint32_t>;
    const Words code=vs?Words(GnmBuiltins::VsCode):id==0?Words(GnmBuiltins::Ps0Code):Words(GnmBuiltins::Ps1Code);
    const Words regs=vs?Words(GnmBuiltins::VsRegs):id==0?Words(GnmBuiltins::Ps0Regs):Words(GnmBuiltins::Ps1Regs);
    std::vector<std::uint8_t> data(0x1000);
    std::memcpy(data.data(),code.data(),code.size_bytes());
    std::memcpy(data.data()+0x100,regs.data(),regs.size_bytes());
    const std::uint32_t low=std::uint32_t(address>>8),high=std::uint32_t(address>>40);
    std::memcpy(data.data()+0x100,&low,4);std::memcpy(data.data()+0x104,&high,4);
    // Built-in GCN code is guest data at a stable, 256-byte-aligned address.
    // Never publish a host pointer in an emulated GPU program register.
    if(memory.IsFree(address,data.size()))memory.Map({address,0x14,std::move(data)});
    else {
        std::vector<std::uint8_t> actual(data.size());
        if(!memory.Read(address,actual) || actual!=data)return UINT32_MAX;
    }
    auto bound=frame;bound.integer[2]=address+0x100;bound.integer[1]=0;
    return ShaderRegisters(vs?"gAhCn6UiU4Y":"bQVd5YzCal0",bound,memory);
}
}
std::optional<std::uint32_t> GuestGnm::Build(std::string_view identity,
    const Translation::CpuFrame& frame,GuestAddressSpace& memory,const GuestVideoOut* video) {
    using namespace Libraries::GnmDriver;
    constexpr std::string_view suffix="#libSceGnmDriver#libSceGnmDriver";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    if(auto embedded=EmbeddedShader(nid,frame,memory))return embedded;
    if(auto marker=DebugMarker(nid,frame,memory))return marker;
    if(auto draw=DrawDispatch(nid,frame,memory))return draw;
    if(auto shader=ShaderRegisters(nid,frame,memory))return shader;
    if(nid=="1qXLHIpROPE") {
        if(!video)return std::nullopt;
        const auto destination=frame.integer[7];
        if(std::uint32_t(frame.integer[6])!=7 || !destination || !memory.CanWrite(destination,28))return UINT32_MAX;
        const auto label=video->BufferLabel(std::uint32_t(frame.integer[2]),std::uint32_t(frame.integer[1]));
        if(!label)return UINT32_MAX;
        const std::array<std::uint32_t,7> wait{0xc0053c00,0x13,std::uint32_t(*label),std::uint32_t(*label>>32),0,UINT32_MAX,10};
        // Emit WAIT_REG_MEM only. Building a command must not clear a VideoOut
        // label or claim that the GPU has already completed the corresponding flip.
        return memory.Write(destination,{reinterpret_cast<const std::uint8_t*>(wait.data()),sizeof(wait)})?0:UINT32_MAX;
    }
    std::span<const std::uint32_t> sequence;
    if(nid=="Idffwf3yh8s")sequence=InitSequence;
    else if(nid=="QhnyReteJ1M")sequence=InitSequence175;
    else if(nid=="0H2vBYbTLHI")sequence=InitSequence200;
    else if(nid=="yb2cRhagD1I")sequence=InitSequence350;
    else return std::nullopt;
    // The iOS machine profile currently exposes the base PS4. Its original
    // register sequences are included directly from the shared GNM driver.
    const auto address=frame.integer[7];
    if(std::uint32_t(frame.integer[6])<HwInitPacketSize || !address ||
       !memory.CanWrite(address,HwInitPacketSize*4))return 0;
    std::array<std::uint32_t,HwInitPacketSize> commands;
    auto bytes=std::span(reinterpret_cast<std::uint8_t*>(commands.data()),sizeof(commands));
    if(!memory.Read(address,bytes))return 0;
    // ClearContextState sequence from the shared gnmdriver.cpp: context control,
    // CLEAR_STATE, then ACQUIRE_MEM. The adapter changes only pointer access.
    constexpr std::array<std::uint32_t,12> clear{
        0xc0012800,0x80000000,0x80000000,0xc0001200,0,0xc0055800,
        0x2ec47fc0,0xffffffff,0,0,0,10};
    if(sequence.size()<2 || clear.size()+sequence.size()>commands.size())return 0;
    std::copy(clear.begin(),clear.end(),commands.begin());
    std::copy(sequence.begin()+2,sequence.end(),commands.begin()+clear.size());
    const auto nop=clear.size()+sequence.size()-2;
    const auto payload=commands.size()-nop-1;
    // PM4 type 3 NOP; only its first data word is initialized by the common
    // writer. Preserve the remaining caller-owned payload bytes exactly.
    commands[nop]=0xc0001000u|(std::uint32_t(payload-1)<<16);
    commands[nop+1]=0;
    return memory.Write(address,bytes)?HwInitPacketSize:0;
}
}
