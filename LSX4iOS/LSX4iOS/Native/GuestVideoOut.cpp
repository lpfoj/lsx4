// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestVideoOut.h"
namespace Lsx4::Apple {
std::uint32_t GuestVideoOut::RegisterBuffers(const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    constexpr std::uint32_t Invalid=0x80290001,Address=0x80290002;
    const auto handle=std::uint32_t(frame.integer[7]),start=std::uint32_t(frame.integer[6]),count=std::uint32_t(frame.integer[1]);
    const auto pointers=frame.integer[2],attributes=frame.integer[8];
    if(!pointers || !attributes)return Address;
    if(handle!=1 || !open_)return 0x8029000b;
    if(start>=slots_.size() || !count || count>slots_.size()-start)return Invalid;
    unsigned group=0;while(group<groups_.size() && groups_[group])++group;
    if(group==groups_.size())return 0x8029000f;
    for(unsigned i=0;i<count;++i)if(slots_[start+i].group>=0)return 0x80290010;
    Attribute value{};
    if(!memory.Read(attributes,{reinterpret_cast<std::uint8_t*>(&value),sizeof(value)}) ||
       !memory.CanRead(pointers,count*8))return Address;
    if(value.reserved0 || value.reserved1)return Invalid;
    if(value.aspectRatio)return 0x80290008;
    if(value.width>value.pitch || value.pitch>16384)return 0x80290004;
    if(!value.width || !value.height || value.width>16384 || value.height>16384)return 0x80290005;
    if(value.tilingMode>1)return 0x80290007;
    switch(value.pixelFormat) {
        case 0x80000000:case 0x80002200:case 0x88060000:case 0x88000000:case 0x88740000:case 0xc1060000:break;
        default:return 0x80290003;
    }
    const auto bytes=std::uint64_t(value.pitch)*value.height*(value.pixelFormat==0xc1060000?8:4);
    std::array<std::uint64_t,16> addresses{};
    for(unsigned i=0;i<count;++i) {
        addresses[i]=memory.Get(pointers+i*8);
        if(!addresses[i] || !memory.IsMapped(addresses[i],bytes))return 0x80290013;
    }
    if(!memory.CanWrite(LabelAddress+start*8,count*8))return Address;
    // Validate the entire group before changing a slot. Tile padding and GPU
    // view creation are the renderer's responsibility; this is registration.
    groups_[group]=value;
    for(unsigned i=0;i<count;++i) {slots_[start+i]={addresses[i],int(group)};memory.Put(LabelAddress+(start+i)*8,0);}
    return group;
}
std::uint32_t GuestVideoOut::UnregisterBuffers(std::uint64_t handle,std::uint64_t group) {
    if(std::uint32_t(handle)!=1 || !open_)return 0x8029000b;
    if(group>=groups_.size() || !groups_[group])return 0x80290001;
    for(auto& slot:slots_)if(slot.group==int(group))slot={};
    groups_[group].reset();return 0;
}
}
