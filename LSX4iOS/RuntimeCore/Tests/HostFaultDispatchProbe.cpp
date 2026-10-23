// SPDX-License-Identifier: GPL-2.0-or-later
// A watched access must reach the in-process owner instead of becoming a
// failed Mach copy. This fixture owns its page and its temporary signal handlers.
#include "executor/dynamic_translation/process_memory.h"
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

static void* watchedPage{};
static std::size_t pageBytes{};
static volatile sig_atomic_t handled{};
static void HandlePageAccess(int, siginfo_t* info, void*) {
    const auto address=reinterpret_cast<std::uintptr_t>(info->si_addr);
    const auto first=reinterpret_cast<std::uintptr_t>(watchedPage);
    if(address<first || address-first>=pageBytes ||
       mprotect(watchedPage,pageBytes,PROT_READ|PROT_WRITE)!=0) _exit(97);
    handled=handled+1;
}
int main() {
    pageBytes=static_cast<std::size_t>(getpagesize());
    watchedPage=mmap(nullptr,pageBytes,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(watchedPage==MAP_FAILED)return 2;
    struct sigaction action{},oldBus{},oldSegv{};
    action.sa_sigaction=HandlePageAccess;action.sa_flags=SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGBUS,&action,&oldBus) || sigaction(SIGSEGV,&action,&oldSegv))return 3;
    if(mprotect(watchedPage,pageBytes,PROT_READ))return 4;
    const std::uint32_t expected=0x91ba247e;
    const auto address=reinterpret_cast<std::uintptr_t>(watchedPage);
    const bool copied=Lsx4::Translation::WriteProcessGuestMemoryWithFaultDispatch(address,&expected,sizeof(expected));
    const bool writePassed=copied && handled==1 && *static_cast<std::uint32_t*>(watchedPage)==expected;
    if(mprotect(watchedPage,pageBytes,PROT_NONE))return 5;
    std::uint32_t observed{};
    const bool checkedRead=Lsx4::Translation::ReadProcessGuestMemory(address,&observed,sizeof(observed));
    const bool checkedReadStayedNonFaulting=!checkedRead && handled==1;
    const bool read=Lsx4::Translation::ReadProcessGuestMemoryWithFaultDispatch(address,&observed,sizeof(observed));
    const bool passed=writePassed && checkedReadStayedNonFaulting && read && handled==2 && observed==expected;
    sigaction(SIGBUS,&oldBus,nullptr);sigaction(SIGSEGV,&oldSegv,nullptr);
    munmap(watchedPage,pageBytes);
    std::printf("apple_watched_access write=%d read=%d handled=%d checkedReadNonFaulting=%d passed=%d\n",copied,read,handled,checkedReadStayedNonFaulting,passed);
    return passed?0:1;
}
