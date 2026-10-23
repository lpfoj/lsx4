// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestPad.h"
namespace Lsx4::Apple {
using Libraries::Pad::OrbisPadData;
static_assert(sizeof(OrbisPadData)==120 && offsetof(OrbisPadData,timestamp)==80);
static_assert(sizeof(Libraries::Pad::OrbisPadControllerInformation)==28);
GuestPadInput::GuestPadInput() {
    current_.leftStick=current_.rightStick={128,128};current_.orientation.w=1;
}
void GuestPadInput::Update(std::uint32_t buttons,std::array<std::uint8_t,6> axes,bool connected,std::uint64_t timestamp,std::optional<Libraries::Pad::OrbisPadTouch> touch) {
    std::scoped_lock lock(mutex_);
    if(connected && !current_.connected && current_.connectedCount<255)++current_.connectedCount;
    current_.connected=connected;
    current_.buttons=static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(connected?buttons&0x10fffe:0);
    current_.leftStick=connected?Libraries::Pad::OrbisPadAnalogStick{axes[0],axes[1]}:Libraries::Pad::OrbisPadAnalogStick{128,128};
    current_.rightStick=connected?Libraries::Pad::OrbisPadAnalogStick{axes[2],axes[3]}:Libraries::Pad::OrbisPadAnalogStick{128,128};
    current_.analogButtons={std::uint8_t(connected?axes[4]:0),std::uint8_t(connected?axes[5]:0),{}};
    current_.timestamp=std::max(timestamp,current_.timestamp);
    current_.touchData={};
    if(connected && touch) {
        current_.touchData.touchNum=1;current_.touchData.touch[0]=*touch;
        current_.touchData.touch[0].x=std::min<std::uint16_t>(touch->x,1919);
        current_.touchData.touch[0].y=std::min<std::uint16_t>(touch->y,949);
    }
    if(pending_.size()==64)pending_.pop_front();
    pending_.push_back(current_);
}
OrbisPadData GuestPadInput::Current() const {std::scoped_lock lock(mutex_);return current_;}
std::vector<OrbisPadData> GuestPadInput::Read(unsigned maximum) {
    std::scoped_lock lock(mutex_);std::vector<OrbisPadData> result;
    const auto count=std::min<std::size_t>(maximum,pending_.size());result.reserve(std::max<std::size_t>(count,1));
    for(std::size_t i=0;i<count;++i) {result.push_back(pending_.front());pending_.pop_front();}
    if(result.empty() && maximum)result.push_back(current_);
    return result;
}
GuestPadInput& PlatformPadInput(){static GuestPadInput input;return input;}
std::optional<std::uint64_t> GuestPad::Call(std::string_view identity,const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    constexpr std::string_view suffix="#libScePad#libScePad";
    constexpr std::uint32_t Invalid=0x80920001,BadHandle=0x80920003;
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    const auto a=frame.integer[7],b=frame.integer[6],c=frame.integer[2];
    if(nid=="hv1luiJrqQM") {initialized_=true;return 0;}
    if(nid=="xk0AcarP3V4" || nid=="u1GRHp+oWoY") {
        if(std::uint32_t(a)!=1000)return 0x80920008;
        if(std::uint32_t(b)!=0 && std::uint32_t(b)!=16)return 0x80920007;
        if(std::uint32_t(c)!=0)return 0x80920002;
        if(nid=="u1GRHp+oWoY")return opened_?1:0x80920008;
        initialized_=true;
        if(opened_)return 0x80920004;
        opened_=true;return 1;
    }
    const bool known=nid=="6ncge5+l5Qs" || nid=="gjP9-KQzoUk" || nid=="YndgXqQVV7c" ||
        nid=="q1cHNfGycLI" || nid=="RR4novUEENY" || nid=="DscD1i9HX1w";
    if(!known)return std::nullopt;
    if(!initialized_)return 0x80920005;
    if(std::uint32_t(a)!=1 || !opened_)return BadHandle;
    if(nid=="6ncge5+l5Qs") {opened_=false;return 0;}
    if(nid=="DscD1i9HX1w") {lightBar_={0,0,255};return 0;}
    if(nid=="RR4novUEENY") {
        std::array<std::uint8_t,4> color{};
        if(!b || !memory.Read(b,color))return Invalid;
        if(color[0]<13 && color[1]<13 && color[2]<13)return 0x80920006;
        lightBar_={color[0],color[1],color[2]};return 0;
    }
    if(nid=="gjP9-KQzoUk") {
        if(!b || !memory.CanWrite(b,28))return Invalid;
        const auto current=input_.Current();Libraries::Pad::OrbisPadControllerInformation info{};
        info.touchPadInfo={1,{1920,950}};info.stickInfo={1,1};
        info.connected=current.connected;info.connectedCount=current.connectedCount;
        info.deviceClass=Libraries::Pad::OrbisPadDeviceClass::Standard;
        memory.Write(b,{reinterpret_cast<const std::uint8_t*>(&info),sizeof(info)});return 0;
    }
    const bool state=nid=="YndgXqQVV7c";
    const auto count=state?1:std::uint32_t(c);
    if(count<1 || count>64)return Invalid;
    if(!b || !memory.CanWrite(b,count*sizeof(OrbisPadData)))return Invalid;
    const auto samples=state?std::vector<OrbisPadData>{input_.Current()}:input_.Read(count);
    if(!memory.Write(b,{reinterpret_cast<const std::uint8_t*>(samples.data()),samples.size()*sizeof(OrbisPadData)}))return Invalid;
    for(const auto& sample:samples) {
        ++samplesRead;const auto buttons=static_cast<std::uint32_t>(sample.buttons);buttonsReadMask|=buttons;
        if(buttons || sample.leftStick.x!=128 || sample.leftStick.y!=128 ||
           sample.rightStick.x!=128 || sample.rightStick.y!=128 || sample.analogButtons.l2 ||
           sample.analogButtons.r2 || sample.touchData.touchNum)++nonNeutralSamplesRead;
    }
    return state?0:samples.size();
}
}
