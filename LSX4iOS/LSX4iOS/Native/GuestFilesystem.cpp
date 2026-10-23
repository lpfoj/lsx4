// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestFilesystem.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Lsx4::Apple {
namespace {
// Darwin and Orbis share these BSD errno values. Keep an explicit translation
// so an unexpected host failure cannot silently acquire a different meaning.
std::uint64_t Error(int e,bool wide=false) {
    unsigned value;
    switch(e) {
        case EPERM:value=1;break;case ENOENT:value=2;break;case EINTR:value=4;break;
        case EIO:value=5;break;case EBADF:value=9;break;case ENOMEM:value=12;break;
        case EACCES:value=13;break;case EFAULT:value=14;break;case EEXIST:value=17;break;
        case ENOTDIR:value=20;break;case EISDIR:value=21;break;case EINVAL:value=22;break;
        case EMFILE:value=24;break;case EFBIG:value=27;break;case ENOSPC:value=28;break;
        case ESPIPE:value=29;break;case EROFS:value=30;break;case EAGAIN:value=35;break;
        case ELOOP:value=62;break;case ENAMETOOLONG:value=63;break;default:value=5;break;
    }
    const std::uint32_t code=0x80020000u+value;
    return wide ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code))) : code;
}
struct Descriptor {
    int value{-1};
    ~Descriptor(){if(value>=0)::close(value);}
};
std::array<std::uint8_t,120> Stat(const struct stat& host) {
    std::array<std::uint8_t,120> out{};
    auto put=[&](unsigned offset,std::uint64_t value,unsigned size){
        for(unsigned i=0;i<size;++i)out[offset+i]=value>>(8*i);
    };
    const bool directory=S_ISDIR(host.st_mode);
    // Match common HLE's virtual mode, size and block geometry. Host inode,
    // ownership and device IDs are intentionally not console identifiers.
    const auto size=directory?65536:host.st_size;
    put(8,(directory?0040000:0100000)|0777,2);
    put(24,host.st_atimespec.tv_sec,8);put(32,host.st_atimespec.tv_nsec,8);
    put(40,host.st_mtimespec.tv_sec,8);put(48,host.st_mtimespec.tv_nsec,8);
    put(56,host.st_ctimespec.tv_sec,8);put(64,host.st_ctimespec.tv_nsec,8);
    put(72,size,8);put(80,(size+511)/512,8);put(88,directory?65536:512,4);
    return out;
}
}
GuestFilesystem::GuestFilesystem(const std::filesystem::path& game) {
    if(game.empty())return;
    const int root=::open(game.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(root<0)throw std::runtime_error("Cannot mount game directory");
    mounts_.emplace("app0",Mount{root,false});
}
GuestFilesystem::~GuestFilesystem() {
    for(auto [guest,host]:descriptors_)::close(host);
    for(auto& [name,mount]:mounts_)if(mount.descriptor>=0)::close(mount.descriptor);
}
bool GuestFilesystem::MountDirectory(std::string name,int directory,bool writable) {
    if(name.empty() || name=="." || name==".." || name.find('/')!=std::string::npos || mounts_.contains(name))return false;
    struct stat status{};
    if(::fstat(directory,&status)!=0 || !S_ISDIR(status.st_mode))return false;
    const int descriptor=::fcntl(directory,F_DUPFD_CLOEXEC,0);
    if(descriptor<0)return false;
    mounts_.emplace(std::move(name),Mount{descriptor,writable});return true;
}
bool GuestFilesystem::UnmountDirectory(std::string_view name) {
    if(name=="app0")return false;
    auto mount=mounts_.find(std::string(name));if(mount==mounts_.end())return false;
    for(const auto& [fd,owner]:descriptorMounts_)if(owner==name)return false;
    // Data-file close/fsync owns data durability; this flushes directory entries.
    if(mount->second.writable && ::fsync(mount->second.descriptor)!=0)return false;
    ::close(mount->second.descriptor);mounts_.erase(mount);return true;
}
int GuestFilesystem::ReadPath(GuestAddressSpace& memory,std::uint64_t address,std::string& out) {
    for(unsigned i=0;i<=255;++i) {
        std::uint8_t c;
        if(address>UINT64_MAX-i || !memory.Read(address+i,{&c,1}))return EFAULT;
        if(!c)return out.empty()?ENOENT:0;
        out+=char(c);
    }
    return ENAMETOOLONG;
}
int GuestFilesystem::Open(const std::string& path,int flags,unsigned mode) {
    // Relative paths use /app0, matching the application's working directory.
    auto relative=std::filesystem::path(path);
    if(!relative.is_absolute())relative=std::filesystem::path("/app0")/relative;
    auto components=relative.relative_path();
    auto part=components.begin();
    if(part==components.end()){errno=ENOENT;return -1;}
    auto mount=mounts_.find(part->string());
    if(mount==mounts_.end()){errno=ENOENT;return -1;}
    if(!mount->second.writable && ((flags&O_ACCMODE)!=O_RDONLY || (flags&(O_CREAT|O_TRUNC|O_APPEND)))) {
        errno=EROFS;return -1;
    }
    if(mount->second.writable && (flags&O_ACCMODE)!=O_RDONLY)flags|=O_SYNC;
    Descriptor current{::dup(mount->second.descriptor)};
    if(current.value<0)return -1;
    std::vector<std::string> names;
    for(++part;part!=components.end();++part) {
        const auto name=part->string();
        if(name.empty() || name==".")continue;
        if(name==".."){errno=EACCES;return -1;}
        names.push_back(name);
    }
    for(std::size_t i=0;i+1<names.size();++i) {
        const int next=::openat(current.value,names[i].c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(next<0)return -1;
        ::close(current.value);current.value=next;
    }
    return ::openat(current.value,names.empty()?".":names.back().c_str(),flags|O_NOFOLLOW|O_CLOEXEC,mode&0777);
}
std::uint64_t GuestFilesystem::PosixWrite(std::uint64_t fd,std::uint64_t buffer,std::uint64_t size,
    std::uint64_t errnoAddress,GuestAddressSpace& memory) {
    if(!memory.CanWrite(errnoAddress,4))throw std::runtime_error("Guest errno storage is unavailable");
    const auto value=Call("4wSze92BhLI#libkernel#libkernel",fd,buffer,size,0,memory).value();
    if(static_cast<std::int64_t>(value)<0) {
        memory.Put32(errnoAddress,std::uint32_t(value)&0xffff);return UINT64_MAX;
    }
    return value;
}
std::optional<std::uint64_t> GuestFilesystem::Call(const std::string& identity,
    std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,GuestAddressSpace& memory) {
    constexpr std::string_view suffix="#libkernel#libkernel";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    if(nid=="1G3lF1Gg1k8") {
        std::string path;const auto error=ReadPath(memory,a,path);
        if(error)return Error(error);
        attemptedPaths.push_back(path);
        constexpr unsigned known=3|4|8|0x80|0x200|0x400|0x800|0x1000|0x10000|0x20000;
        const unsigned flags=b;
        if((flags&3)==3 || (flags&~known))return Error(EINVAL);
        int hostFlags=(flags&3)==0?O_RDONLY:(flags&3)==1?O_WRONLY:O_RDWR;
        if(flags&4)hostFlags|=O_NONBLOCK;
        if(flags&8)hostFlags|=O_APPEND;
        if(flags&(0x80|0x1000))hostFlags|=O_SYNC;
        if(flags&0x200)hostFlags|=O_CREAT;
        if(flags&0x400)hostFlags|=O_TRUNC;
        if(flags&0x800)hostFlags|=O_EXCL;
        if(flags&0x20000)hostFlags|=O_DIRECTORY;
        if(descriptors_.size()>=1024)return Error(EMFILE);
        const int fd=Open(path,hostFlags,c);
        if(fd<0)return Error(errno);
        // O_DIRECT is a cache policy, not a different byte stream. Darwin
        // F_NOCACHE is the equivalent available on both macOS and iOS.
        if(flags&0x10000)::fcntl(fd,F_NOCACHE,1);
        int guest=3;while(descriptors_.contains(guest))++guest;
        descriptors_.emplace(guest,fd);
        const auto relative=std::filesystem::path(path).relative_path();
        descriptorMounts_[guest]=std::filesystem::path(path).is_absolute()?relative.begin()->string():"app0";
        openedPaths.push_back(path);return guest;
    }
    if(nid=="eV9wAD2riIA") {
        if(!memory.CanWrite(b,120))return Error(EFAULT);
        std::string path;const auto error=ReadPath(memory,a,path);
        if(error)return Error(error);
        Descriptor fd{Open(path,O_RDONLY,0)};
        if(fd.value<0)return Error(errno);
        struct stat host{};if(::fstat(fd.value,&host))return Error(errno);
        return memory.Write(b,Stat(host))?0:Error(EFAULT);
    }
    const bool read=nid=="Cg4srZ6TKbU",pread=nid=="+r3rMFwItV4";
    const bool write=nid=="4wSze92BhLI",pwrite=nid=="nKWi-N2HBV4";
    const bool seek=nid=="oib76F-12fk",close=nid=="UK2Tl2DWUns";
    const bool stat=nid=="kBwCPsYX-m4",sync=nid=="fTx66l5iWIA";
    if(!read && !pread && !write && !pwrite && !seek && !close && !stat && !sync)return std::nullopt;
    const bool wide=read||pread||write||pwrite||seek;
    if(write && (static_cast<int>(a)==1 || static_cast<int>(a)==2)) {
        if(c>INT64_MAX)return Error(EINVAL,true);
        if(b>UINT64_MAX-c || !memory.CanRead(b,c))return Error(EFAULT,true);
        std::array<std::uint8_t,4096> bytes{};
        constexpr char hex[]="0123456789abcdef";
        for(std::uint64_t offset=0;offset<c;) {
            const auto count=std::min<std::uint64_t>(bytes.size(),c-offset);
            if(!memory.Read(b+offset,std::span(bytes).first(count)))return Error(EFAULT,true);
            for(auto byte:std::span(bytes).first(count)) {
                if((byte>=32 && byte<127) || byte=='\n' || byte=='\r' || byte=='\t')consoleOutput+=char(byte);
                else {consoleOutput+="\\x";consoleOutput+=hex[byte>>4];consoleOutput+=hex[byte&15];}
            }
            // A bounded console history consumes the stream without writing
            // guest text to host file descriptors or interpreting control bytes.
            if(consoleOutput.size()>65536)consoleOutput.erase(0,consoleOutput.size()-65536);
            offset+=count;
        }
        consoleBytes+=c;return c;
    }
    auto found=descriptors_.find(static_cast<int>(a));
    if(found==descriptors_.end())return Error(EBADF,wide);
    const int fd=found->second;
    if(close) {
        const int rc=::close(fd);const int error=errno;descriptors_.erase(found);
        descriptorMounts_.erase(static_cast<int>(a));
        return rc<0?Error(error):0;
    }
    if(sync)return ::fsync(fd)<0?Error(errno):0;
    if(seek) {
        if(c>2)return Error(EINVAL,true);
        const auto offset=::lseek(fd,static_cast<std::int64_t>(b),static_cast<int>(c));
        return offset<0?Error(errno,true):static_cast<std::uint64_t>(offset);
    }
    if(stat) {
        if(!memory.CanWrite(b,120))return Error(EFAULT);
        struct stat host{};if(::fstat(fd,&host))return Error(errno);
        return memory.Write(b,Stat(host))?0:Error(EFAULT);
    }
    if(c>INT64_MAX || ((pread||pwrite) && (d>INT64_MAX || c>INT64_MAX-d)))return Error(EINVAL,true);
    if(b>UINT64_MAX-c)return Error(EFAULT,true);
    if(read||pread) {if(!memory.CanWrite(b,c))return Error(EFAULT,true);}
    else if(!memory.CanRead(b,c))return Error(EFAULT,true);
    std::array<std::uint8_t,65536> buffer{};
    std::uint64_t total=0;
    while(total<c) {
        const auto size=std::min<std::uint64_t>(buffer.size(),c-total);
        auto bytes=std::span(buffer).first(size);
        if(write||pwrite)if(!memory.Read(b+total,bytes))return total?total:Error(EFAULT,true);
        const auto count=read?::read(fd,buffer.data(),size):pread?::pread(fd,buffer.data(),size,d+total):
                         write?::write(fd,buffer.data(),size): ::pwrite(fd,buffer.data(),size,d+total);
        if(count<0) {if(errno==EINTR)continue;return total?total:Error(errno,true);}
        if(!count)break;
        if(read||pread) {
            if(!memory.Write(b+total,bytes.first(count)))return total?total:Error(EFAULT,true);
            bytesRead+=count;
        } else bytesWritten+=count;
        total+=count;if(static_cast<std::uint64_t>(count)<size)break;
    }
    return total;
}
}
