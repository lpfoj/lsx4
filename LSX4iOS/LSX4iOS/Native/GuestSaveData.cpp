// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestSaveData.h"
#include "GuestUserService.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace Lsx4::Apple {
namespace {
constexpr std::uint32_t Parameter=0x809f0000,NotInitialized=0x809f0001,Busy=0x809f0003,
    NotMounted=0x809f0004,Exists=0x809f0007,NotFound=0x809f0008,Internal=0x809f000b,
    MountFull=0x809f000c,InvalidUser=0x809f0011;
struct Descriptor {int value{-1};~Descriptor(){if(value>=0)::close(value);}};
bool Component(std::string_view name) {
    if(name.empty() || name=="." || name=="..")return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){
        return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-' || c=='.';
    });
}
template<std::size_t N> bool ReadString(GuestAddressSpace& memory,std::uint64_t pointer,std::string& text) {
    std::array<std::uint8_t,N> bytes{};
    if(!pointer || !memory.Read(pointer,bytes))return false;
    auto end=std::find(bytes.begin(),bytes.end(),0);
    if(end==bytes.end())return false;
    text.assign(bytes.begin(),end);return true;
}
}
GuestSaveData::GuestSaveData(std::filesystem::path anchor,std::string title):anchor_(std::move(anchor)),title_(std::move(title)){}
GuestSaveData::~GuestSaveData()=default;
int GuestSaveData::OpenRoot(bool create) {
    if(anchor_.empty() || !Component(title_)){errno=EINVAL;return -1;}
    Descriptor current{::open(anchor_.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};
    if(current.value<0)return -1;
    for(const auto& name:{std::string("SaveData"),title_,std::to_string(GuestUserService::UserId)}) {
        if(create && ::mkdirat(current.value,name.c_str(),0700)!=0 && errno!=EEXIST)return -1;
        const int next=::openat(current.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(next<0)return -1;
        ::close(current.value);current.value=next;
    }
    const int result=current.value;current.value=-1;return result;
}
std::optional<std::uint32_t> GuestSaveData::Call(std::string_view identity,std::uint64_t a,
    std::uint64_t b,GuestAddressSpace& memory,GuestFilesystem& filesystem) {
    constexpr std::string_view suffix="#libSceSaveData#libSceSaveData";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    if(nid=="TywrFKCoLGY" || nid=="l1NmDeDpNGU" || nid=="ZkZhskCPXFw") {
        if(!Component(title_) || anchor_.empty())return Parameter;
        initialized_=true;return 0;
    }
    const bool mount=nid=="32HQAQdwM2o",mount2=nid=="0z45PIH+SNI",unmount=nid=="BMR4F-Uek3E",terminate=nid=="yKDy8S5yLA0";
    if(!mount && !mount2 && !unmount && !terminate)return std::nullopt;
    if(!initialized_)return NotInitialized;
    if(terminate) {
        for(const auto& slot:slots_)if(!slot.mount.empty())return Busy;
        initialized_=false;return 0;
    }
    if(unmount) {
        std::string point;if(!ReadString<16>(memory,a,point))return Parameter;
        for(auto& slot:slots_)if(!slot.mount.empty() && point=="/"+slot.mount) {
            if(!filesystem.UnmountDirectory(slot.mount))return Busy;
            slot={};return 0;
        }
        return NotMounted;
    }
    if(!a || !b || !memory.CanRead(a,mount?80:64) || !memory.CanWrite(b,64))return Parameter;
    if(std::uint32_t(memory.Get(a))!=GuestUserService::UserId)return InvalidUser;
    const auto directoryPointer=memory.Get(a+(mount?16:8));
    const auto blocks=memory.Get(a+(mount?32:16));
    const auto mode=std::uint32_t(memory.Get(a+(mount?40:24)));
    std::string directory;if(!ReadString<32>(memory,directoryPointer,directory) || !Component(directory))return Parameter;
    // COPY_ICON requires a separate icon service. It is not silently performed.
    if(mode&~0x2fu || (mode&3)==0 || (mode&3)==3 || ((mode&4)&&(mode&32)))return Parameter;
    const bool create=mode&4,createIfMissing=mode&32,readOnly=mode&1;
    if(readOnly && (create || createIfMissing))return Parameter;
    if((create || createIfMissing) && (blocks<96 || blocks>32768))return Parameter;
    Slot* selected=nullptr;unsigned index=0;
    for(unsigned i=0;i<slots_.size();++i) {
        if(!slots_[i].mount.empty() && slots_[i].directory==directory)return Busy;
        if(!selected && slots_[i].mount.empty()){selected=&slots_[i];index=i;}
    }
    if(!selected)return MountFull;
    Descriptor root{OpenRoot(create || createIfMissing)};
    if(root.value<0)return errno==ENOENT?NotFound:Internal;
    struct stat status{};const bool exists=::fstatat(root.value,directory.c_str(),&status,AT_SYMLINK_NOFOLLOW)==0;
    if(exists && !S_ISDIR(status.st_mode))return Parameter;
    if(exists && create)return Exists;
    if(!exists && !create && !createIfMissing)return NotFound;
    bool created=false;
    if(!exists) {
        if(::mkdirat(root.value,directory.c_str(),0700)!=0)return Internal;
        created=true;
    }
    Descriptor folder{::openat(root.value,directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};
    if(folder.value<0)return Internal;
    const auto name="savedata"+std::to_string(index);
    if(!filesystem.MountDirectory(name,folder.value,!readOnly))return Internal;
    const auto point="/"+name;
    std::array<std::uint8_t,64> result{};std::copy(point.begin(),point.end(),result.begin());
    result[28]=created && createIfMissing?1:0;
    if(!memory.Write(b,result)) {filesystem.UnmountDirectory(name);return Parameter;}
    *selected={directory,name};return 0;
}
}
