// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestGpu.h"
#include "AppleVulkanCompute.h"
#include "AppleGraphicsShader.h"
#include "AppleVulkanGraphics.h"
#include "AppleVulkanSurface.h"
#include "AppleVertexFetch.h"
#include <atomic>
#include <sstream>
namespace Lsx4::Apple {
namespace {
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::uint64_t Address(std::uint32_t low,std::uint32_t high){return low|(std::uint64_t(high)<<32);}
std::vector<std::uint32_t> ReadShader(std::uint64_t address,GuestAddressSpace& memory) {
    std::array<std::uint32_t,2> prefix{};
    Require(memory.ReadGpu(address,{reinterpret_cast<std::uint8_t*>(prefix.data()),8}),"GPU shader header unavailable");
    Require(prefix[0]==0xbeeb03ff && prefix[1]<8192,"Invalid GPU shader header");
    std::array<std::uint32_t,7> metadata{};
    Require(memory.ReadGpu(address+(std::uint64_t(prefix[1])+1)*8,{reinterpret_cast<std::uint8_t*>(metadata.data()),28}),"GPU metadata unavailable");
    Require(metadata[0]==0x5362724f && (metadata[1]&0xffffff)==0x726468,"Invalid GPU metadata");
    const auto bytes=metadata[2]>>8;
    Require(bytes && bytes<=65536 && !(bytes&3),"Invalid GPU program length");
    std::vector<std::uint32_t> code(bytes/4);
    Require(memory.ReadGpu(address,{reinterpret_cast<std::uint8_t*>(code.data()),bytes}),"GPU code unavailable");
    return code;
}
}
void GuestGpu::Write(std::uint64_t address,std::span<const std::uint8_t> bytes,GuestAddressSpace& memory) {
    Require(memory.WriteGpu(address,bytes),"GPU write range unavailable");
    if(captureDigests_) {
        auto hash=metrics.bytesWritten?metrics.writeDigest:14695981039346656037ull;
        for(auto byte:bytes){hash^=byte;hash*=1099511628211ull;}
        metrics.writeDigest=hash;
    }
    metrics.bytesWritten+=bytes.size();
}
void GuestGpu::Compute(std::span<const std::uint32_t> packet,GuestAddressSpace& memory) {
    Require(packet.size()==5 && !(packet[0]&1) && packet[4]==1,"Unsupported compute dispatch flags");
    if(!packet[1] || !packet[2] || !packet[3])return;
    const auto address=Address(registers_[0x2e0c],registers_[0x2e0d]&255)<<8;
    Require(address!=0,"Missing compute shader address");
    const auto code=ReadShader(address,memory);
    ComputeShaderInput input;input.address=address;input.code=code;
    input.rsrc1=registers_[0x2e12];input.rsrc2=registers_[0x2e13];
    for(unsigned i=0;i<3;++i) {
        Require(!(registers_[0x2e07+i]>>16),"Partial compute workgroups require an adapter");
        input.workgroup[i]=registers_[0x2e07+i];
    }
    for(unsigned i=0;i<16;++i)input.userData[i]=registers_[0x2e40+i];
    const auto shader=CompileComputeShader(input);
    std::vector<std::vector<std::uint8_t>> data;std::uint64_t total=0;
    for(const auto& buffer:shader.buffers) {
        Require(buffer.size<=128ull*1024*1024 && total<=128ull*1024*1024-buffer.size,"Compute buffer budget exceeded");
        total+=buffer.size;
        if(!buffer.inlineBytes.empty())data.push_back(buffer.inlineBytes);
        else {
            Require(memory.CanGpuRead(buffer.address,buffer.size) && (!buffer.written || memory.CanGpuWrite(buffer.address,buffer.size)),"Compute buffer GPU permissions unavailable");
            data.emplace_back(buffer.size);
            Require(memory.ReadGpu(buffer.address,data.back()),"Compute input unavailable");
        }
    }
    DispatchVulkanCompute(shader,{packet[1],packet[2],packet[3]},data);
    // No guest thread runs while this synchronous route waits. Validate all
    // outputs before publishing any of the completed GPU writes.
    for(unsigned i=0;i<data.size();++i)if(shader.buffers[i].written)
        Require(memory.CanGpuWrite(shader.buffers[i].address,data[i].size()),"Compute output mapping changed");
    for(unsigned i=0;i<data.size();++i)if(shader.buffers[i].written)Write(shader.buffers[i].address,data[i],memory);
    ++metrics.computeDispatches;
}
void GuestGpu::DrawDepth(std::span<const std::uint32_t> packet,GuestAddressSpace& memory) {
    Require(packet.size()==3 && packet[2]==2 && packet[1] && packet[1]<=1048576,"Unsupported automatic depth draw");
    auto target=BuildDepthClearDraw(registers_,depthExtent_);target.state.vertexCount=packet[1];
    Require(memory.CanGpuRead(target.address,target.size) && memory.CanGpuWrite(target.address,target.size),"Depth target GPU permissions unavailable");
    const auto vsAddress=Address(registers_[0x2c48],registers_[0x2c49]&255)<<8;
    const auto psAddress=Address(registers_[0x2c08],registers_[0x2c09]&255)<<8;
    const auto vs=ReadShader(vsAddress,memory),ps=ReadShader(psAddress,memory);
    const auto shaders=CompileGraphicsShader({registers_,vs,ps});
    std::vector<std::vector<std::uint8_t>> inputs;std::uint64_t total=0;
    for(const auto& buffer:shaders.buffers) {
        Require(buffer.size<=128ull*1024*1024 && total<=128ull*1024*1024-buffer.size,"Graphics buffer budget exceeded");
        total+=buffer.size;
        if(!buffer.inlineBytes.empty())inputs.push_back(buffer.inlineBytes);
        else {
            inputs.emplace_back(buffer.size);
            Require(memory.ReadGpu(buffer.address,inputs.back()),"Graphics input GPU range unavailable");
        }
    }
    std::vector<std::uint8_t> original(target.size);
    Require(memory.ReadGpu(target.address,original),"Depth surface unavailable");
    auto linear=TransformSurface32(target.tileMode,target.pitch,target.allocationHeight,original,false);
    const auto drawn=RenderDepthClearDraw(shaders,target.state,inputs);
    // Preserve padding and every pixel outside the logical surface extent.
    for(unsigned y=0;y<target.state.height;++y)
        std::memcpy(linear.data()+std::size_t(y)*target.pitch*4,drawn.depth.data()+std::size_t(y)*target.state.width*4,target.state.width*4);
    const auto tiled=TransformSurface32(target.tileMode,target.pitch,target.allocationHeight,linear,true);
    Write(target.address,tiled,memory);
    ++metrics.depthDraws;metrics.visibleDraws+=drawn.occlusionVisible;
    metrics.surfaceTransfers+=target.tileMode==8 || target.tileMode==31?0:2;
}
void GuestGpu::DrawColor(std::span<const std::uint32_t> packet,GuestAddressSpace& memory) {
    Require(packet.size()==6 && packet[1] && packet[1]<=1048576 && packet[4]<=packet[1] &&
            packet[4] && !packet[5] && !registers_[0xa29f] && !registers_[0xa2a5],"Unsupported indexed color draw");
    auto target=BuildColorDraw(registers_);target.state.raster.vertexCount=packet[4];
    Require(memory.CanGpuRead(target.address,target.size) && memory.CanGpuWrite(target.address,target.size),"Color target GPU permissions unavailable");
    const auto vsAddress=Address(registers_[0x2c48],registers_[0x2c49]&255)<<8;
    const auto psAddress=Address(registers_[0x2c08],registers_[0x2c09]&255)<<8;
    const auto vs=ReadShader(vsAddress,memory),ps=ReadShader(psAddress,memory);
    const auto fetch=ResolveVertexFetch(registers_,vs,[&](auto address,auto bytes){return memory.ReadGpu(address,bytes);});
    const auto shaders=CompileGraphicsShader({fetch.registers,fetch.code,ps});
    std::vector<std::vector<std::uint8_t>> inputs,textures;std::uint64_t total=target.size;
    auto read=[&](std::uint64_t address,std::uint64_t size) {
        Require(size<=128ull*1024*1024 && total<=128ull*1024*1024-size,"Graphics resource budget exceeded");total+=size;
        std::vector<std::uint8_t> bytes(size);Require(memory.ReadGpu(address,bytes),"Graphics input GPU range unavailable");return bytes;
    };
    for(const auto& buffer:shaders.buffers)inputs.push_back(buffer.inlineBytes.empty()?read(buffer.address,buffer.size):buffer.inlineBytes);
    for(const auto& image:shaders.images)textures.push_back(read(image.address,image.size));
    auto indices=read(Address(packet[2],packet[3]),std::uint64_t(packet[4])*2);
    std::vector<std::uint8_t> original(target.size);
    Require(memory.ReadGpu(target.address,original),"Color surface unavailable");
    auto linear=TransformSurface32(target.tileMode,target.pitch,target.allocationHeight,original,false);
    const auto drawn=RenderColorDraw(shaders,target.state,inputs,textures,linear,indices);
    const auto tiled=TransformSurface32(target.tileMode,target.pitch,target.allocationHeight,drawn.color,true);
    Write(target.address,tiled,memory);++metrics.colorDraws;metrics.visibleDraws+=drawn.occlusionVisible;
    metrics.surfaceTransfers+=target.tileMode==8 || target.tileMode==31?0:2;
}
std::pair<std::string,std::string> GuestGpu::SubmitAndFlip(const Translation::CpuFrame& frame,GuestAddressSpace& memory,GuestEqueues& queues) {
    std::uint64_t currentWord=0;submissionComplete_=false;
    try {
        const auto count=std::uint32_t(frame.integer[7]);
        Require(count && count<=64,"Unsupported command buffer count");
        Require(frame.integer[6] && frame.integer[2] && memory.CanRead(frame.integer[6],count*8) && memory.CanRead(frame.integer[2],count*4),"Invalid submission arrays");
        Require(!frame.integer[1] && !frame.integer[8],"Constant command buffers require an adapter");
        std::vector<std::vector<std::uint32_t>> submissions;
        for(unsigned i=0;i<count;++i) {
            std::uint32_t bytes;
            Require(memory.Read(frame.integer[2]+i*4,{reinterpret_cast<std::uint8_t*>(&bytes),4}),"Invalid command buffer length pointer");
            Require(bytes && bytes<=0x3ffffc && !(bytes&3),"Invalid command buffer length");
            auto address=memory.Get(frame.integer[6]+i*8);submissions.emplace_back(bytes/4);
            Require(memory.ReadGpu(address,{reinterpret_cast<std::uint8_t*>(submissions.back().data()),bytes}),"Command buffer GPU memory unavailable");
        }
        flipReservation_={};
        if(!submissions.empty() && submissions.back().size()>=64) {
            auto start=submissions.back().end()-64;
            if(*start==0xc03e1000)std::copy_n(start,7,flipReservation_.begin());
        }
        ++metrics.submissions;
        for(const auto& words:submissions)for(std::size_t cursor=0;cursor<words.size();) {
            currentWord=cursor;const auto header=words[cursor];
            if((header>>30)==2){++cursor;++metrics.packets;continue;}
            Require((header>>30)==3,"Unsupported PM4 packet type");
            const auto length=((header>>16)&0x3fff)+2,opcode=(header>>8)&255;
            Require(length<=words.size()-cursor,"Truncated PM4 packet");
            const auto packet=std::span(words).subspan(cursor,length);
            Require(!(header&1),"PM4 predication requires an adapter");
            switch(opcode) {
                case 0x10:break; // Driver annotations and reserved NOP payload.
                case 0x28:Require(length==3,"Invalid context control");break;
                case 0x12:Require(length==2 && !packet[1],"Unsupported clear state");registers_=DefaultGpuRegisters();depthExtent_=0;break;
                case 0x69:case 0x76:case 0x68:case 0x79: {
                    Require(length>=3 && !(packet[1]&0xffff0000),"Unsupported PM4 register index");
                    const auto base=opcode==0x76?0x2c00u:opcode==0x69?0xa000u:opcode==0x68?0x2000u:0xc000u;
                    Require(packet[1]+length-2<=0x1000,"PM4 register range unavailable");
                    for(unsigned i=2;i<length;++i)registers_[base+packet[1]+i-2]=packet[i];
                    if(opcode==0x69 && packet[1]==0x10) {
                        depthExtent_=0;
                        if(length==10) {
                            Require(words.size()-cursor>=24 && words[cursor+22]==0xc0001000,"Missing depth extent hint");
                            depthExtent_=words[cursor+23];
                        }
                    }
                    break;
                }
                case 0x2f:Require(length==2,"Invalid instance count");registers_[0xc24d]=packet[1];break;
                case 0x2a:Require(length==2,"Invalid index type");registers_[0xa29f]=packet[1];break;
                case 0x58:
                    Require(length==7,"Invalid acquire memory packet");
                    std::atomic_thread_fence(std::memory_order_acq_rel);break;
                case 0x3c: {
                    Require(length==7 && packet[1]==0x13,"Unsupported GPU wait mode");
                    std::uint32_t value;Require(memory.ReadGpu(Address(packet[2],packet[3]),{reinterpret_cast<std::uint8_t*>(&value),4}),"GPU wait address unavailable");
                    if((value&packet[5])!=packet[4])return {"gpu-wait","GPU memory condition is not signaled at word "+std::to_string(cursor)};
                    break;
                }
                case 0x50:
                    // Common Liverpool treats this CP prefetch packet as a
                    // cache operation, not a write to guest address 0x3022c.
                    Require(length==7 && packet[1]==0x60000000 && packet[4]==0x3022c && !packet[5] &&
                        (packet[6]&~0x1fffffu)==0x28200000,"DMA transfer requires an adapter");
                    Require(memory.CanGpuRead(Address(packet[2],packet[3]),packet[6]&0x1fffff),"DMA prefetch source unavailable");break;
                case 0x15:Compute(packet,memory);break;
                case 0x48: {
                    Require(length==5 && (packet[3]>>29)==2 && (packet[1]&0x3f)==0x2f,"Unsupported end-of-shader event");
                    const auto data=packet[4];Write(Address(packet[2],packet[3]&65535),{reinterpret_cast<const std::uint8_t*>(&data),4},memory);break;
                }
                case 0x47: {
                    Require(length==6 && packet[1]==0x504 && !(packet[3]&~0xe300ffffu),"Unsupported end-of-pipe control");
                    const auto dataSelect=packet[3]>>29,interrupt=(packet[3]>>24)&3;
                    Require(dataSelect<=2 && interrupt<=2 && (interrupt!=1 || dataSelect==0),"Unsupported end-of-pipe signal");
                    const auto address=Address(packet[2],packet[3]&65535);
                    const auto data=Address(packet[4],packet[5]);const unsigned bytes=dataSelect==1?4:dataSelect==2?8:0;
                    Require(!bytes || !(address&(bytes-1)),"Unaligned end-of-pipe write");
                    // Every preceding GPU operation has already completed its
                    // fence/readback. Publish memory before notifying waiters.
                    if(bytes)Write(address,{reinterpret_cast<const std::uint8_t*>(&data),bytes},memory);
                    if(interrupt)queues.Trigger(64,-14,64);
                    ++metrics.endOfPipes;break;
                }
                case 0x2d:
                    if(registers_[0xa000]==1 && !registers_[0xa08e] && (registers_[0xa200]&2)) {
                        DrawDepth(packet,memory);break;
                    }
                    return {"gpu-unsupported-draw","Graphics pipeline required at word "+std::to_string(cursor)};
                case 0x27:DrawColor(packet,memory);break;
                default: {
                    std::ostringstream detail;detail<<"PM4 opcode 0x"<<std::hex<<opcode<<" at word "<<std::dec<<cursor;
                    return {"gpu-unsupported-packet",detail.str()};
                }
            }
            ++metrics.packets;cursor+=length;
        }
        submissionComplete_=true;
        return {"gpu-unimplemented-presentation","VideoOut presentation has not completed"};
    } catch(const std::exception& error) {
        return {"gpu-error",std::string(error.what())+" at word "+std::to_string(currentWord)};
    }
}
std::pair<std::string,std::string> GuestGpu::Present(const Translation::CpuFrame& frame,GuestAddressSpace& memory,
    GuestEqueues& queues,GuestVideoOut& video,const GuestFrameSink& sink) {
    if(!sink)return {"gpu-unimplemented-presentation","No frame output is connected"};
    try {
        Require(flipReservation_[0]==0xc03e1000 && (flipReservation_[1]==0x68750777 || flipReservation_[1]==0x68750778),"Unsupported flip reservation");
        Require(frame.integer[4]<=UINT64_MAX-32 && memory.CanRead(frame.integer[4]+8,24),"Invalid flip stack arguments");
        const auto index=std::uint32_t(memory.Get(frame.integer[4]+8));
        const auto mode=std::uint32_t(memory.Get(frame.integer[4]+16));const auto argument=memory.Get(frame.integer[4]+24);
        Require(mode<=2 && video.CanPresent(frame.integer[9],index),"Invalid VideoOut flip request");
        const auto target=*video.Buffer(index);const auto& a=target.attribute;
        Require((a.pixelFormat==0x80000000 || a.pixelFormat==0x80002200) && !a.option,"Unsupported display pixel format/options");
        const auto geometry=DisplaySurface(a.width,a.height,a.pitch,a.tilingMode==0);
        std::vector<std::uint8_t> original(geometry.size);Require(memory.ReadGpu(target.address,original),"VideoOut buffer GPU read failed");
        const auto linear=a.tilingMode==0?TransformSurface32(geometry.tileMode,geometry.pitch,geometry.height,original,false):std::move(original);
        GuestFrame image;image.width=a.width;image.height=a.height;image.rgba.resize(std::size_t(a.width)*a.height*4);
        for(unsigned y=0;y<a.height;++y) {
            auto* destination=image.rgba.data()+std::size_t(y)*a.width*4;
            std::memcpy(destination,linear.data()+std::size_t(y)*geometry.pitch*4,a.width*4);
            if(a.pixelFormat==0x80000000)for(unsigned x=0;x<a.width;++x)std::swap(destination[x*4],destination[x*4+2]);
        }
        const auto portLabel=GuestVideoOut::LabelAddress+index*8;
        Require(memory.CanGpuWrite(portLabel,8),"VideoOut ownership label unavailable");
        if(flipReservation_[1]==0x68750778) {
            const auto address=Address(flipReservation_[2],flipReservation_[3]);
            Require(!(address&3) && memory.CanGpuWrite(address,4),"Invalid flip completion label");
            const auto value=flipReservation_[4];Write(address,{reinterpret_cast<const std::uint8_t*>(&value),4},memory);
        }
        const std::uint64_t busy=1;Write(portLabel,{reinterpret_cast<const std::uint8_t*>(&busy),8},memory);
        Require(sink(image),"Frame output did not confirm presentation");
        video.Presented(index,argument,memory,queues);++metrics.framesOutput;
        if(a.tilingMode==0)++metrics.surfaceTransfers;
        if(captureDigests_) {
            auto hash=14695981039346656037ull;for(auto byte:image.rgba){hash^=byte;hash*=1099511628211ull;}metrics.frameDigest=hash;
        }
        return {"gpu-frame-output","Frame delivered to the connected output"};
    } catch(const std::exception& error) {return {"gpu-presentation-error",error.what()};}
}

}
