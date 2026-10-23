// Experimental full-runtime embedding; explicit opt-in and bounded allocation.
#include "Adapter/PreparedPool.h"
#include "host_signal_context.h"
#include "Integration/AppleFaultRouting.h"
extern "C" size_t LSX4FexPreparedPoolCapacityBytes() {
  return AppleFexPreparedPool::published_capacity.load(std::memory_order_acquire);
}
extern "C" size_t LSX4FexPreparedPoolConsumedBytes() {
  return AppleFexPreparedPool::published_consumed.load(std::memory_order_acquire);
}
extern "C" bool LSX4PrepareFexPool(size_t bytes) {
  if(!AppleFexPreparedPool::Prepare(bytes)) return false;
  AppleFexPreparedPool::Install(); return true;
}
extern "C" void executor_lsx4_apple_fex_normalize_signal(int* sig, siginfo_t* info, void* raw) {
  lsx4::fex2608::HostNormalizeMemoryFault(*sig, *info, static_cast<ucontext_t*>(raw));
}
extern "C" bool LSX4WaitFexDetached() {
 for(int i=0;i<3000;++i) {
  int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};struct kinfo_proc p{};size_t n=sizeof(p);
  if(sysctl(mib,4,&p,&n,nullptr,0)) return false;
  if(!(p.kp_proc.p_flag&P_TRACED)) return true;
  usleep(10000);
 }
 return false;
}

extern "C" bool executor_lsx4_apple_fex_precheck_memory_owner(int sig, const siginfo_t* info) {
  return Lsx4::AppleFex::PrecheckMemoryOwner(sig, info);
}

// Called only by a failed normal-context deferred landing or its unreachable
// resume fallback. Preserve the assembly's exact exit status.
extern "C" [[noreturn]] void lsx4_fex2608_report_deferred_exit(uint32_t code) {
  char text[] = "[APPLE_FEX_DEFERRED_EXIT] code=0x00000000\n";
  constexpr char digits[] = "0123456789abcdef";
  constexpr size_t first = sizeof("[APPLE_FEX_DEFERRED_EXIT] code=0x") - 1;
  for (unsigned i=0; i<8; ++i) text[first+i] = digits[(code >> ((7-i)*4)) & 15];
  (void)::write(STDERR_FILENO, text, sizeof(text)-1);
  ::_exit(static_cast<int>(code));
}
