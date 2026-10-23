// SPDX-License-Identifier: MIT
// Experimental embedding bridge. Process-lifetime hook installation is serialized
// before any FEX context exists. iPhone code pages require debugger preparation.
#pragma once
#include <FEXCore/Utils/AllocatorHooks.h>
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>
#include <map>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
extern "C" void LSX4DebugPrepareCodePages(void*,size_t);
#endif
namespace AppleFexMemory {
struct Mapping { void* writable; size_t size; };
inline std::mutex mutex;
inline std::map<uintptr_t,Mapping> mappings;
inline Mapping* Find(uintptr_t address,size_t size,uintptr_t& base) {
 auto i=mappings.upper_bound(address);if(i==mappings.begin())return nullptr;--i;
 if(address-i->first>=i->second.size)return nullptr;
 if(size>i->second.size-(address-i->first)) {fprintf(stderr,"partial code alias range\n");abort();}
 base=i->first;return &i->second;
}
inline void* Map(void* hint,size_t size,int protection,int flags,int fd,off_t offset) {
 if(!(protection&PROT_EXEC))return ::mmap(hint,size,protection,flags,fd,offset);
 const size_t page=static_cast<size_t>(getpagesize());
 if(hint||fd!=-1||offset||flags!=(MAP_ANONYMOUS|MAP_PRIVATE)||size==0||size>SIZE_MAX-page){errno=EINVAL;return MAP_FAILED;}
 size=(size+page-1)&~(page-1);
 void* rx=::mmap(nullptr,size,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0);
 if(rx==MAP_FAILED)return MAP_FAILED;
 vm_address_t alias=0;vm_prot_t current=0,maximum=0;
 auto r=vm_remap(mach_task_self(),&alias,size,page-1,VM_FLAGS_ANYWHERE,mach_task_self(),reinterpret_cast<vm_address_t>(rx),FALSE,&current,&maximum,VM_INHERIT_NONE);
 if(r!=KERN_SUCCESS){::munmap(rx,size);errno=ENOMEM;return MAP_FAILED;}
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
 int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};struct kinfo_proc process{};size_t processSize=sizeof(process);
 bool prepared=sysctl(mib,4,&process,&processSize,nullptr,0)==0 && (process.kp_proc.p_flag&P_TRACED);
 constexpr size_t chunk=4*1024*1024;
 for(size_t at=0;prepared&&at<size;at+=chunk){const size_t span=std::min(chunk,size-at);auto* address=static_cast<char*>(rx)+at;LSX4DebugPrepareCodePages(address,span);prepared=memcmp(address,"LSX4JIT!",8)==0;}
 if(!prepared){vm_deallocate(mach_task_self(),alias,size);::munmap(rx,size);errno=EPERM;return MAP_FAILED;}
#endif
 if(::mprotect(reinterpret_cast<void*>(alias),size,PROT_READ|PROT_WRITE)) {vm_deallocate(mach_task_self(),alias,size);::munmap(rx,size);return MAP_FAILED;}
 std::lock_guard lock(mutex);mappings.emplace(reinterpret_cast<uintptr_t>(rx),Mapping{reinterpret_cast<void*>(alias),size});return rx;
}
inline int Unmap(void* address,size_t size) {
 std::lock_guard lock(mutex);const auto key=reinterpret_cast<uintptr_t>(address);auto i=mappings.find(key);
 if(i==mappings.end())return ::munmap(address,size);
 const size_t page=getpagesize();size=(size+page-1)&~(page-1);
 if(size!=i->second.size){errno=EINVAL;return -1;}
 if(vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(i->second.writable),size)!=KERN_SUCCESS)return -1;
 const int result=::munmap(address,size);if(!result)mappings.erase(i);return result;
}
inline void* Writable(void* address,size_t size) {
 if(!address)return nullptr;
 std::lock_guard lock(mutex);uintptr_t base=0;const auto key=reinterpret_cast<uintptr_t>(address);auto* m=Find(key,size,base);
 return m?static_cast<unsigned char*>(m->writable)+(key-base):address;
}
inline void Publish(void* address,size_t size) {
 std::lock_guard lock(mutex);uintptr_t base=0;const auto key=reinterpret_cast<uintptr_t>(address);auto* m=Find(key,size,base);
 std::atomic_thread_fence(std::memory_order_release);
 if(m)sys_dcache_flush(static_cast<unsigned char*>(m->writable)+(key-base),size);
 sys_icache_invalidate(address,size);
}
inline bool Protect(void* address,size_t size,FEXCore::Allocator::ProtectOptions options) {
 using P=FEXCore::Allocator::ProtectOptions;int protection=0;
 if((options&P::Read)==P::Read)protection|=PROT_READ;
 if((options&P::Write)==P::Write)protection|=PROT_WRITE;
 if((options&P::Exec)==P::Exec)protection|=PROT_EXEC;
 std::lock_guard lock(mutex);uintptr_t base=0;const auto key=reinterpret_cast<uintptr_t>(address);auto* m=Find(key,size,base);
 if(!m)return ::mprotect(address,size,protection)==0;
 // FEX may make its trailing guard inaccessible; do not create RWX mappings.
 if(protection!=PROT_NONE && protection!=(PROT_READ|PROT_EXEC)){errno=ENOTSUP;return false;}
 const int wp=protection?PROT_READ|PROT_WRITE:PROT_NONE;
 return ::mprotect(static_cast<unsigned char*>(m->writable)+(key-base),size,wp)==0 && ::mprotect(address,size,protection)==0;
}
inline void Install(){
 FEXCore::Allocator::mmap=Map;FEXCore::Allocator::munmap=Unmap;
 FEXCore::Allocator::writable_code=Writable;FEXCore::Allocator::publish_code=Publish;FEXCore::Allocator::protect_code=Protect;
}
}
