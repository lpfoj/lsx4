// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include "GuestEqueue.h"
#include <optional>
#include <string_view>
namespace Lsx4::Apple {
// VideoOut control-plane state follows the common driver's single main port.
// Submission/presentation remain explicit unsupported boundaries until the GPU
// renderer is connected. Opening a port never advances a frame counter.
class GuestVideoOut {
public:
    struct Attribute {
        std::uint32_t pixelFormat,tilingMode,aspectRatio,width,height,pitch,option,reserved0;
        std::uint64_t reserved1;
    };
    static_assert(sizeof(Attribute)==40);
    struct DisplayBuffer {std::uint64_t address;Attribute attribute;};
    static constexpr std::uint64_t LabelAddress=0x530000000000;
    bool CanPresent(std::uint32_t handle,unsigned index) const {return handle==1 && open_ && Buffer(index).has_value();}
    void Presented(unsigned index,std::uint64_t argument,GuestAddressSpace& memory,GuestEqueues& queues) {
        if(!CanPresent(1,index))throw std::runtime_error("VideoOut port changed before presentation");
        if(previousBuffer_>=0 && previousBuffer_!=int(index))memory.Put(LabelAddress+unsigned(previousBuffer_)*8,0);
        previousBuffer_=index;flipArgument_=argument;++presentedFrames_;
        queues.Trigger(6,-13,6|(argument<<16));
    }
    std::optional<std::uint64_t> BufferLabel(std::uint32_t handle,unsigned requested) const {
        if(handle!=1 || !open_)return std::nullopt;
        if(requested<slots_.size() && slots_[requested].group>=0)return LabelAddress+requested*8;
        if(previousBuffer_>=0)return LabelAddress+unsigned(previousBuffer_)*8;
        // Before the first presentation, the common driver resolves an invalid
        // requested index to the first registered slot. No previous flip exists.
        for(unsigned i=0;i<slots_.size();++i)if(slots_[i].group>=0)return LabelAddress+i*8;
        return requested<slots_.size()?std::optional(LabelAddress+requested*8):std::nullopt;
    }
    std::optional<DisplayBuffer> Buffer(unsigned index) const {
        if(index>=slots_.size() || slots_[index].group<0)return std::nullopt;
        return DisplayBuffer{slots_[index].address,*groups_[slots_[index].group]};
    }
    std::optional<std::uint64_t> Call(std::string_view identity,const Translation::CpuFrame& frame,GuestAddressSpace& memory,GuestEqueues& queues) {
        constexpr std::uint32_t invalid=0x80290001,badAddress=0x80290002,badHandle=0x8029000b;
        constexpr std::string_view suffix="#libSceVideoOut#libSceVideoOut";
        if(!identity.ends_with(suffix))return std::nullopt;
        const auto nid=identity.substr(0,identity.size()-suffix.size());
        const auto a=frame.integer[7],b=frame.integer[6];
        if(nid=="w3BY+tAEiQY")return RegisterBuffers(frame,memory);
        if(nid=="N5KDtkIjjJ4")return UnregisterBuffers(a,b);
        if(nid=="HXzjK9yI30k" || nid=="Xru92wHJRmg" || nid=="-Ozn0F1AFRg" || nid=="oNOQn3knW6s") {
            if(std::uint32_t(b)!=1 || !open_)return badHandle;
            if(!queues.Exists(a))return 0x8029000c;
            const bool flip=nid=="HXzjK9yI30k" || nid=="-Ozn0F1AFRg";
            const auto eventId=std::uint64_t(flip?6:7);
            if(nid=="-Ozn0F1AFRg" || nid=="oNOQn3knW6s") {
                queues.Remove(a,eventId,-13);return 0;
            }
            return queues.Add(a,{eventId,-13,1,0,0,frame.integer[2]})==0?0:0x8029100c;
        }
        if(nid=="Up36PTk687E") {
            if(std::uint32_t(b)!=0 || std::uint32_t(frame.integer[2])!=0)return invalid;
            if(open_)return 0x80290009;
            if(memory.IsFree(LabelAddress,16384))memory.Map({LabelAddress,0x36,std::vector<std::uint8_t>(16384)});
            if(!memory.Fill(LabelAddress,0,128))return badAddress;
            open_=true;flipRate_=0;previousBuffer_=-1;presentedFrames_=0;return 1;
        }
        if(nid=="i6-sR91Wt-4") {
            if(!a || !memory.CanWrite(a,40))throw std::runtime_error("Invalid VideoOut attribute pointer");
            const auto width=std::uint32_t(frame.integer[8]),height=std::uint32_t(frame.integer[9]);
            auto pitch=std::uint32_t(memory.Get(frame.integer[4]+8));
            if(pitch<width || pitch>16384)pitch=width;
            std::array<std::uint8_t,40> value{};
            auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)value[at+i]=v>>(8*i);};
            put(0,b);put(4,frame.integer[2]);put(8,frame.integer[1]);put(12,width);put(16,height);put(20,pitch);
            if(!memory.Write(a,value))throw std::runtime_error("Invalid VideoOut attribute output");
            return frame.integer[0]; // void API
        }
        const bool known=nid=="uquVH4-Du78" || nid=="6kPnj51T62Y" || nid=="SbU3dwp80lQ" ||
            nid=="CBiu4mCE1DA" || nid=="zgXifHT9ErY" || nid=="OcQybQejHEY";
        if(!known)return std::nullopt;
        if(std::uint32_t(a)!=1 || !open_)return badHandle;
        if(nid=="uquVH4-Du78") {queues.RemoveFilter(-13);open_=false;flipRate_=0;groups_={};slots_={};return 0;}
        if(nid=="CBiu4mCE1DA") {
            if(b>2)return invalid;
            flipRate_=b;return 0;
        }
        if(nid=="zgXifHT9ErY")return 0; // No frame has been submitted.
        if(nid=="OcQybQejHEY") {
            if(!b || !memory.CanWrite(b,8))return badAddress;
            memory.Put(b,LabelAddress);return 16;
        }
        const bool resolution=nid=="6kPnj51T62Y";
        if(!resolution && presentedFrames_)throw std::runtime_error("VideoOut status after presentation requires a clock adapter");
        const unsigned size=resolution?48:64;
        if(!b || !memory.CanWrite(b,size))return badAddress;
        std::array<std::uint8_t,64> value{};
        auto put=[&](unsigned at,std::uint64_t v,unsigned bytes){for(unsigned i=0;i<bytes;++i)value[at+i]=v>>(8*i);};
        if(resolution) {
            put(0,1280,4);put(4,720,4);put(8,1280,4);put(12,720,4);
            put(16,3,8);put(24,0x42480000,4);
        }else {put(24,UINT64_MAX,8);put(56,UINT32_MAX,4);}
        return memory.Write(b,std::span(value).first(size))?0:badAddress;
    }
private:
    std::uint32_t RegisterBuffers(const Translation::CpuFrame& frame,GuestAddressSpace& memory);
    std::uint32_t UnregisterBuffers(std::uint64_t handle,std::uint64_t group);
    struct Slot {std::uint64_t address{};int group{-1};};
    std::array<Slot,16> slots_{};
    std::array<std::optional<Attribute>,4> groups_{};
    bool open_{};
    std::uint32_t flipRate_{};
    int previousBuffer_{-1};
    std::uint64_t presentedFrames_{},flipArgument_{};
};
}
