// SPDX-License-Identifier: MIT

#include "host_signal_context.h"
#include "arm64_signal_context_records.h"
#include "deferred_signal_protocol.h"
#include "deferred_write_epoch.h"
#include "executable_registry.h"
#include "fex_context_bridge_api.h"
#include "guest_cpu_profile_ir.h"
#include "guest_store_trace_ir.h"
#include "hle_resume_lease.h"
#include "host_midr_quirk.h"
#include "jit_metadata_decoder.h"
#include "signal_atomic_classifier.h"
#include "signal_capture_gate.h"
#include "signal_reject_telemetry.h"
#include "proof_metrics_ir.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/ExactStoreWatch.h>
#include <FEXCore/Core/ExecutableCodeObserver.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/PrctlUtils.h>
#include <FEXCore/Utils/TypeDefines.h>

#include "Interface/Context/Context.h"
#include "Interface/Core/CPUBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#if !defined(__APPLE__)
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <sys/syscall.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#include <utility>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

extern "C" uint64_t lsx4_fex2608_read_fpcr();
extern "C" void lsx4_fex2608_write_fpcr(uint64_t value);

namespace {

constexpr const char *kInvalidBridgeRequest =
    "invalid FEX-2608 context bridge request";
constexpr const char *kContextCreationFailed =
    "FEX-2608 CreateNewContext returned null";
constexpr const char *kCoreInitializationFailed = "FEX-2608 InitCore failed";
constexpr const char *kSignalOwnerUnavailable =
    "staged FEX runtime: host-fault signal owner and async guest signals "
    "unavailable";
constexpr const char *kSignalOwnerActive =
    "full FEX runtime: LSX4 signal owner, guest delivery, and thread wake "
    "active";
constexpr const char *kSoftwareTsoName = "FEX atomic software TSO";
constexpr const char *kHardwareTsoName = "host hardware TSO";
constexpr const char *kRelaxedVectorTsoName =
    "FEX software TSO (VectorTSO relaxed A/B)";
constexpr const char *kRelaxedMemcpySetTsoName =
    "FEX software TSO (MemcpySetTSO relaxed A/B)";
constexpr const char *kRelaxedGameHubStableName =
    "FEX software TSO (VectorTSO+MemcpySetTSO relaxed SM8650 A/B)";
constexpr const char *kHardwareTsoEnableFailed =
    "hardware TSO was selected but PR_SET_MEM_MODEL_TSO failed";
constexpr const char *kHardwareTsoBindFailed =
    "hardware TSO bind failed; :game restart required";
constexpr uint64_t kGuestExitSyscall = UINT64_C(0x5ad00002);
constexpr int32_t kBridgeWakeSignal = SIGURG;
constexpr int32_t kOrbisSigBus = 10;
constexpr int32_t kOrbisSaSigInfo = 4;
constexpr int32_t kOrbisSaNoDefer = INT32_C(0x40000000);
constexpr uint32_t kOrbisSaResetHand = UINT32_C(0x80000000);
constexpr uint64_t kBaseRuntimeCommonFeatures =
    LSX4_FEX2608_RUNTIME_MAPPED_ENTRY | LSX4_FEX2608_RUNTIME_EXACT_HLE_THUNK |
    LSX4_FEX2608_RUNTIME_ACTIVE_CONTEXT | LSX4_FEX2608_RUNTIME_ACTIVE_TLS |
    LSX4_FEX2608_RUNTIME_GUEST_CALLBACK |
    LSX4_FEX2608_RUNTIME_EXECUTABLE_RANGES |
    LSX4_FEX2608_RUNTIME_SIGNAL_ACTION_STORAGE;
constexpr uint64_t kBaseRuntimeFeatures =
    kBaseRuntimeCommonFeatures | LSX4_FEX2608_RUNTIME_STRONG_MEMORY;
constexpr uint64_t kFullRuntimeFeatures =
    kBaseRuntimeFeatures | LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER |
    LSX4_FEX2608_RUNTIME_GUEST_SIGNAL_DELIVERY |
    LSX4_FEX2608_RUNTIME_GUEST_FUTEX_SCHEDULING;
constexpr uint64_t kRuntimeApiFeatures =
    kFullRuntimeFeatures | LSX4_FEX2608_RUNTIME_RELAXED_MEMORY_MODEL_AB;
constexpr uint32_t kSignalOwnerCreateFlags =
    LSX4_FEX2608_CREATE_STAGED_PRIVATE_HLT_EXIT |
    LSX4_FEX2608_CREATE_FULL_HOST_SIGNAL_OWNER;
constexpr uint32_t kMemoryModelAbCreateFlags =
    LSX4_FEX2608_CREATE_RELAX_VECTOR_TSO_AB |
    LSX4_FEX2608_CREATE_RELAX_MEMCPY_SET_TSO_AB;
constexpr uint32_t kCodegenAbCreateFlags =
    LSX4_FEX2608_CREATE_EXACT_MULTIBLOCK_AB |
    LSX4_FEX2608_CREATE_MAXINST_512_AB |
    LSX4_FEX2608_CREATE_MAXINST_256_AB |
    LSX4_FEX2608_CREATE_PERSISTENT_RIP_SEED_AB |
    LSX4_FEX2608_CREATE_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB |
    LSX4_FEX2608_CREATE_PRESERVE_TYPED_TIME_COMMON_FRAME_AB |
    LSX4_FEX2608_CREATE_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB;
constexpr uint64_t kRuntimeDiagnostics =
    LSX4_FEX2608_DIAGNOSTIC_HOST_FAULT_SIGNAL_OWNER_UNAVAILABLE |
    LSX4_FEX2608_DIAGNOSTIC_ASYNC_GUEST_SIGNAL_UNAVAILABLE;
constexpr uint32_t kAllContextFlags =
    LSX4_FEX_CONTEXT_GPR_VALID | LSX4_FEX_CONTEXT_XMM_VALID |
    LSX4_FEX_CONTEXT_FLAGS_VALID | LSX4_FEX_CONTEXT_TLS_VALID;
constexpr uint32_t kAllHleResultFlags = LSX4_FEX_HLE_RESULT_GUEST_EXIT;
constexpr size_t kThunkIdentitySize = sizeof(FEXCore::IR::SHA256Sum::data);
constexpr size_t kGuestCpuProfileIdCapacity = 128;
constexpr size_t kCallbackScratchSize = 256 * 1024;
constexpr size_t kDeferredSignalStackSize = 64 * 1024;
constexpr uint32_t kDeferredSignalResumeBrk =
    UINT32_C(0xd4200000) | (UINT32_C(0x4f45) << 5);
constexpr int32_t kOrbisMcHasSegments = 0x1;
constexpr int32_t kOrbisMcHasBases = 0x2;
/*
 * This is the modified Orbis inline-XSAVE ABI, not stock FreeBSD 10's
 * pointer-based mc_xfpustate ABI.  The exact PS4 frame producer in
 * GuestFramesManagement.cpp publishes sizeof(mcontext)==0x480 and flags 1|2;
 * its 832-byte mc_fpstate already contains legacy, XSAVE header, and YMMH.
 * Therefore bit 4 (_MC_HASFPXSTATE in stock FreeBSD) is unknown here.
 */
constexpr uint32_t kOrbisMcKnownFlags = kOrbisMcHasSegments | kOrbisMcHasBases;

FEXCore::IR::SHA256Sum DeriveTypedTimeThunkIdentity(
    const std::array<uint8_t, kThunkIdentitySize> &base) noexcept {
  constexpr std::array<std::pair<size_t, uint8_t>, 4> Delta{{
      {0, 0xa7}, {11, 0x5d}, {23, 0xc3}, {31, 0x6e}}};
  FEXCore::IR::SHA256Sum result{};
  std::memcpy(result.data, base.data(), base.size());
  for (const auto &[index, value] : Delta) {
    result.data[index] ^= value;
  }
  return result;
}

constexpr uint64_t kOrbisFpFormatNoDevice = UINT64_C(0x10000);
constexpr uint64_t kOrbisFpFormatXmm = UINT64_C(0x10002);
constexpr uint64_t kOrbisFpOwnedNone = UINT64_C(0x20000);
constexpr uint64_t kOrbisFpOwnedFpu = UINT64_C(0x20001);
constexpr uint64_t kOrbisFpOwnedPcb = UINT64_C(0x20002);
constexpr uint32_t kDefaultSupportedMxcsrMask = UINT32_C(0x0000ffc0);
constexpr uint64_t kOrbisSupportedXstate = UINT64_C(0x7);
constexpr uint64_t kOrbisRflagsUserChange = (UINT64_C(1) << 0) |  /* CF */
                                            (UINT64_C(1) << 2) |  /* PF */
                                            (UINT64_C(1) << 4) |  /* AF */
                                            (UINT64_C(1) << 6) |  /* ZF */
                                            (UINT64_C(1) << 7) |  /* SF */
                                            (UINT64_C(1) << 8) |  /* TF */
                                            (UINT64_C(1) << 10) | /* DF */
                                            (UINT64_C(1) << 11) | /* OF */
                                            (UINT64_C(1) << 14) | /* NT */
                                            (UINT64_C(1) << 16) | /* RF */
                                            (UINT64_C(1) << 18) | /* AC */
                                            (UINT64_C(1) << 21);  /* ID */
constexpr const char *kBusAdralnRejectLogPath =
    "/data/data/app.lsx4.android/files/fex-bus-adraln-reject.log";

constexpr bool IsRelaxedMemoryModelAb(uint32_t create_flags) {
  return (create_flags & kMemoryModelAbCreateFlags) != 0;
}

constexpr uint64_t RuntimeFeaturesForCreateFlags(uint32_t create_flags) {
  const uint64_t memory_model =
      IsRelaxedMemoryModelAb(create_flags)
          ? LSX4_FEX2608_RUNTIME_RELAXED_MEMORY_MODEL_AB
          : LSX4_FEX2608_RUNTIME_STRONG_MEMORY;
  const uint64_t owner_features =
      (create_flags & LSX4_FEX2608_CREATE_FULL_HOST_SIGNAL_OWNER) != 0
          ? LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER |
                LSX4_FEX2608_RUNTIME_GUEST_SIGNAL_DELIVERY |
                LSX4_FEX2608_RUNTIME_GUEST_FUTEX_SCHEDULING
          : 0;
  return kBaseRuntimeCommonFeatures | memory_model | owner_features;
}

const char *MemoryModelName(uint32_t strong_memory_mode,
                            uint32_t create_flags) {
  if ((create_flags & kMemoryModelAbCreateFlags) ==
      kMemoryModelAbCreateFlags) {
    return kRelaxedGameHubStableName;
  }
  if ((create_flags & LSX4_FEX2608_CREATE_RELAX_VECTOR_TSO_AB) != 0) {
    return kRelaxedVectorTsoName;
  }
  if ((create_flags & LSX4_FEX2608_CREATE_RELAX_MEMCPY_SET_TSO_AB) != 0) {
    return kRelaxedMemcpySetTsoName;
  }
  return strong_memory_mode == LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO
             ? kHardwareTsoName
             : kSoftwareTsoName;
}

/* FreeBSD/Orbis x86-64 signal frame ABI used by funnel-arm. */
struct OrbisMcontext {
  uint64_t mc_onstack;
  uint64_t mc_rdi;
  uint64_t mc_rsi;
  uint64_t mc_rdx;
  uint64_t mc_rcx;
  uint64_t mc_r8;
  uint64_t mc_r9;
  uint64_t mc_rax;
  uint64_t mc_rbx;
  uint64_t mc_rbp;
  uint64_t mc_r10;
  uint64_t mc_r11;
  uint64_t mc_r12;
  uint64_t mc_r13;
  uint64_t mc_r14;
  uint64_t mc_r15;
  int32_t mc_trapno;
  uint16_t mc_fs;
  uint16_t mc_gs;
  uint64_t mc_addr;
  int32_t mc_flags;
  uint16_t mc_es;
  uint16_t mc_ds;
  uint64_t mc_err;
  uint64_t mc_rip;
  uint64_t mc_cs;
  uint64_t mc_rflags;
  uint64_t mc_rsp;
  uint64_t mc_ss;
  uint64_t mc_len;
  uint64_t mc_fpformat;
  uint64_t mc_ownedfp;
  uint64_t mc_lbrfrom;
  uint64_t mc_lbrto;
  uint64_t mc_aux1;
  uint64_t mc_aux2;
  alignas(64) uint64_t mc_fpstate[104];
  uint64_t mc_fsbase;
  uint64_t mc_gsbase;
  uint64_t mc_spare[6];
};

struct OrbisFpSlot {
  uint8_t bytes[16];
};

struct OrbisXstateHeader {
  uint64_t xstate_bv;
  uint64_t xcomp_bv;
  uint8_t reserved[48];
};

struct alignas(64) OrbisInlineFpState {
  uint16_t fcw;
  uint16_t fsw;
  uint8_t ftw;
  uint8_t reserved0;
  uint16_t fop;
  uint64_t fip;
  uint64_t fdp;
  uint32_t mxcsr;
  uint32_t mxcsr_mask;
  OrbisFpSlot st[8];
  OrbisFpSlot xmm[16];
  uint8_t padding[96];
  OrbisXstateHeader xstate;
  OrbisFpSlot ymmh[16];
};

struct OrbisExStack {
  uint64_t ss_sp;
  uint64_t ss_size;
  int32_t ss_flags;
  int32_t align;
};

struct OrbisSigset {
  uint64_t bits[2];
};

union OrbisSigval {
  int32_t sival_int;
  uint64_t sival_ptr;
};

struct OrbisSiginfo {
  int32_t signo;
  int32_t error;
  int32_t code;
  int32_t pid;
  uint32_t uid;
  int32_t status;
  uint64_t address;
  OrbisSigval value;
  union {
    struct {
      int32_t trapno;
    } fault;
    struct {
      int32_t timer_id;
      int32_t overrun;
    } timer;
    struct {
      int32_t descriptor;
    } message_queue;
    struct {
      int64_t band;
    } poll;
    struct {
      int64_t spare1;
      int32_t spare2[7];
    } spare;
  } reason;
};

struct OrbisUcontext {
  OrbisSigset signal_mask;
  int32_t reserved0[12];
  OrbisMcontext machine;
  uint64_t link;
  OrbisExStack stack;
  int32_t flags;
  int32_t spare[4];
  int32_t reserved1[3];
};

static_assert(sizeof(OrbisMcontext) == 0x480);
static_assert(sizeof(OrbisSiginfo) == 0x50);
static_assert(sizeof(OrbisUcontext) == 0x500);
static_assert(alignof(OrbisMcontext) == 64);
static_assert(alignof(OrbisUcontext) == 64);
static_assert(offsetof(OrbisMcontext, mc_fpstate) == 0x100);
static_assert(sizeof(((OrbisMcontext *)nullptr)->mc_fpstate) == 0x340);
static_assert(offsetof(OrbisUcontext, machine) == 0x40);
static_assert((offsetof(OrbisUcontext, machine) +
               offsetof(OrbisMcontext, mc_fpstate)) == 0x140);
static_assert(((offsetof(OrbisUcontext, machine) +
                offsetof(OrbisMcontext, mc_fpstate)) &
               63) == 0);
static_assert(sizeof(OrbisFpSlot) == 16);
static_assert(sizeof(OrbisXstateHeader) == 64);
static_assert(sizeof(OrbisInlineFpState) == 832);
static_assert(alignof(OrbisInlineFpState) == 64);
static_assert(offsetof(OrbisInlineFpState, st) == 32);
static_assert(offsetof(OrbisInlineFpState, xmm) == 160);
static_assert(offsetof(OrbisInlineFpState, xstate) == 512);
static_assert(offsetof(OrbisInlineFpState, ymmh) == 576);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct Runtime;
struct ThreadRecord;

struct ExecutableRangeStore {
  uint64_t generation{};
  std::vector<Lsx4FexExecutableRangeV5> ranges;
};

constexpr uint32_t kPersistentRipSeedVersion = 1;
constexpr uint32_t kPersistentRipSeedCapacity = 65536;
constexpr uint32_t kPersistentRipSeedPrewarmLimit = 4096;
constexpr auto kPersistentRipSeedPrewarmBudget = std::chrono::milliseconds{200};

struct alignas(64) PersistentRipSeedHeader {
  std::array<char, 8> magic{};
  uint32_t version{};
  uint32_t header_size{};
  uint32_t entry_size{};
  uint32_t capacity{};
  uint64_t identity{};
  alignas(4) uint32_t cursor{};
  uint32_t reserved{};
};

struct alignas(8) PersistentRipSeedEntry {
  uint64_t rip{};
  uint32_t guest_length{};
  uint32_t sequence{};
  alignas(8) uint64_t checksum{};
};

static_assert(sizeof(PersistentRipSeedHeader) == 64);
static_assert(sizeof(PersistentRipSeedEntry) == 24);

uint64_t RipSeedHashBytes(uint64_t hash, const void *data, size_t size) {
  constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
  return hash;
}

uint64_t RipSeedHashString(uint64_t hash, const char *value) {
  return value != nullptr ? RipSeedHashBytes(hash, value, std::strlen(value))
                          : RipSeedHashBytes(hash, "<null>", 6);
}

uint64_t RipSeedChecksum(uint64_t identity, uint64_t rip,
                         uint32_t guest_length, uint32_t sequence) {
  uint64_t hash = RipSeedHashBytes(UINT64_C(1469598103934665603),
                                   &identity, sizeof(identity));
  hash = RipSeedHashBytes(hash, &rip, sizeof(rip));
  hash = RipSeedHashBytes(hash, &guest_length, sizeof(guest_length));
  hash = RipSeedHashBytes(hash, &sequence, sizeof(sequence));
  return hash != 0 ? hash : UINT64_C(1);
}

std::string SanitizeRipSeedNamespace(const char *value) {
  std::string sanitized;
  if (value == nullptr) {
    return sanitized;
  }
  sanitized.reserve(std::min<size_t>(std::strlen(value), 160));
  for (const unsigned char byte : std::string_view{value}) {
    if (sanitized.size() == 160) {
      break;
    }
    const bool safe = (byte >= 'a' && byte <= 'z') ||
                      (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '-' ||
                      byte == '_' || byte == '.';
    sanitized.push_back(safe ? static_cast<char>(byte) : '_');
  }
  return sanitized;
}

bool IsPersistentRipSeedRange(const ExecutableRangeStore *store,
                              uint64_t rip, uint32_t guest_length) {
  if (store == nullptr || store->ranges.empty() || guest_length == 0) {
    return false;
  }
  const auto upper = std::upper_bound(
      store->ranges.begin(), store->ranges.end(), rip,
      [](uint64_t address, const Lsx4FexExecutableRangeV5 &range) {
        return address < range.address;
      });
  if (upper == store->ranges.begin()) {
    return false;
  }
  const auto &range = *std::prev(upper);
  constexpr uint32_t kRequired =
      LSX4_FEX_RANGE_EXECUTABLE | LSX4_FEX_RANGE_PERSISTENT_MODULE;
  constexpr uint32_t kForbidden =
      LSX4_FEX_RANGE_WRITABLE | LSX4_FEX_RANGE_SELF_MODIFYING |
      LSX4_FEX_RANGE_HLE_TRAMPOLINE | LSX4_FEX_RANGE_HLE_FRAME_POOL;
  if ((range.range_flags & kRequired) != kRequired ||
      (range.range_flags & kForbidden) != 0 || rip < range.address ||
      range.size == 0 || rip - range.address >= range.size) {
    return false;
  }
  return guest_length <= range.size - (rip - range.address);
}

class PersistentRipSeedProfile {
public:
  PersistentRipSeedProfile() = default;
  PersistentRipSeedProfile(const PersistentRipSeedProfile &) = delete;
  PersistentRipSeedProfile &operator=(const PersistentRipSeedProfile &) = delete;

  ~PersistentRipSeedProfile() {
    if (mapping_ != MAP_FAILED) {
      (void)::munmap(mapping_, mapping_size_);
    }
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  bool Initialize(const char *runtime_root, const char *cache_namespace,
                  const char *host_build_id, const char *guest_profile_id,
                  const uint8_t *thunk_identity, size_t thunk_identity_size,
                  const ExecutableRangeStore *ranges, const bool rebuild) {
    if (runtime_root == nullptr || runtime_root[0] == '\0' ||
        cache_namespace == nullptr || cache_namespace[0] == '\0' ||
        host_build_id == nullptr || host_build_id[0] == '\0' ||
        ranges == nullptr) {
      return false;
    }

    constexpr uint64_t kFnvOffset = UINT64_C(1469598103934665603);
    identity_ = RipSeedHashString(kFnvOffset, cache_namespace);
    identity_ = RipSeedHashString(identity_, host_build_id);
    identity_ = RipSeedHashString(identity_, guest_profile_id);
    identity_ = RipSeedHashBytes(identity_, thunk_identity, thunk_identity_size);
    for (const auto &range : ranges->ranges) {
      if ((range.range_flags & LSX4_FEX_RANGE_PERSISTENT_MODULE) == 0) {
        continue;
      }
      identity_ = RipSeedHashBytes(identity_, &range.address,
                                   sizeof(range.address));
      identity_ = RipSeedHashBytes(identity_, &range.size, sizeof(range.size));
      identity_ = RipSeedHashBytes(identity_, &range.range_flags,
                                   sizeof(range.range_flags));
    }

    const std::string safe_namespace =
        SanitizeRipSeedNamespace(cache_namespace);
    if (safe_namespace.empty()) {
      return false;
    }
    directory_ = std::string{runtime_root} + "/fex-rip-seeds";
    if (::mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST) {
      return false;
    }
    struct stat directory_stat {};
    if (::stat(directory_.c_str(), &directory_stat) != 0 ||
        !S_ISDIR(directory_stat.st_mode) || directory_stat.st_uid != ::getuid()) {
      return false;
    }
    if (rebuild) {
      const std::string prefix = safe_namespace + "-mid";
      DIR *directory = ::opendir(directory_.c_str());
      if (directory == nullptr) {
        return false;
      }
      const int directory_fd = ::dirfd(directory);
      bool removed = true;
      while (const dirent *entry = ::readdir(directory)) {
        const std::string_view name{entry->d_name};
        constexpr std::string_view suffix{".ripseed-v1"};
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
          continue;
        }
        if (::unlinkat(directory_fd, entry->d_name, 0) != 0 &&
            errno != ENOENT) {
          removed = false;
          break;
        }
      }
      (void)::closedir(directory);
      if (!removed) {
        return false;
      }
    }
    char identity_suffix[32]{};
    const int identity_written =
        std::snprintf(identity_suffix, sizeof(identity_suffix), "-mid%016" PRIx64,
                      identity_);
    if (identity_written <= 0 ||
        static_cast<size_t>(identity_written) >= sizeof(identity_suffix)) {
      return false;
    }
    path_ = directory_ + "/" + safe_namespace + identity_suffix +
            ".ripseed-v1";
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                 0600);
    if (fd_ < 0) {
      return false;
    }
    struct stat file_stat {};
    if (::fstat(fd_, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
        file_stat.st_uid != ::getuid()) {
      return false;
    }
    (void)::fchmod(fd_, 0600);
    mapping_size_ = sizeof(PersistentRipSeedHeader) +
                    sizeof(PersistentRipSeedEntry) *
                        kPersistentRipSeedCapacity;
    if (file_stat.st_size != static_cast<off_t>(mapping_size_) &&
        ::ftruncate(fd_, static_cast<off_t>(mapping_size_)) != 0) {
      return false;
    }
    mapping_ = ::mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) {
      return false;
    }
    header_ = static_cast<PersistentRipSeedHeader *>(mapping_);
    entries_ = reinterpret_cast<PersistentRipSeedEntry *>(header_ + 1);
    const std::array<char, 8> magic{'L', 'S', 'X', '4', 'R', 'I', 'P', 'S'};
    const bool valid = header_->magic == magic &&
                       header_->version == kPersistentRipSeedVersion &&
                       header_->header_size == sizeof(*header_) &&
                       header_->entry_size == sizeof(*entries_) &&
                       header_->capacity == kPersistentRipSeedCapacity &&
                       header_->identity == identity_;
    if (!valid) {
      std::memset(mapping_, 0, mapping_size_);
      header_->magic = magic;
      header_->version = kPersistentRipSeedVersion;
      header_->header_size = sizeof(*header_);
      header_->entry_size = sizeof(*entries_);
      header_->capacity = kPersistentRipSeedCapacity;
      header_->identity = identity_;
      __atomic_store_n(&header_->cursor, UINT32_C(0), __ATOMIC_RELEASE);
      (void)::msync(header_, sizeof(*header_), MS_SYNC);
    }
    return true;
  }

  bool ready() const { return header_ != nullptr && entries_ != nullptr; }
  const std::string &path() const { return path_; }

  void Record(const ExecutableRangeStore *ranges, uint64_t rip,
              uint64_t guest_length) noexcept {
    if (!ready() || guest_length == 0 || guest_length > UINT32_MAX ||
        !IsPersistentRipSeedRange(ranges, rip,
                                  static_cast<uint32_t>(guest_length))) {
      return;
    }
    const uint32_t sequence =
        __atomic_fetch_add(&header_->cursor, UINT32_C(1), __ATOMIC_RELAXED) + 1;
    auto &entry = entries_[(sequence - 1) % kPersistentRipSeedCapacity];
    __atomic_store_n(&entry.checksum, UINT64_C(0), __ATOMIC_RELAXED);
    entry.rip = rip;
    entry.guest_length = static_cast<uint32_t>(guest_length);
    entry.sequence = sequence;
    __atomic_store_n(&entry.checksum,
                     RipSeedChecksum(identity_, rip, entry.guest_length,
                                     sequence),
                     __ATOMIC_RELEASE);
  }

  std::vector<uint64_t> SnapshotRecent(const ExecutableRangeStore *ranges,
                                       uint32_t limit) const {
    std::vector<uint64_t> result;
    if (!ready() || ranges == nullptr || limit == 0) {
      return result;
    }
    const uint32_t cursor =
        __atomic_load_n(&header_->cursor, __ATOMIC_ACQUIRE);
    const uint32_t available =
        std::min(cursor, kPersistentRipSeedCapacity);
    result.reserve(std::min(limit, available));
    std::unordered_set<uint64_t> seen;
    seen.reserve(std::min(limit, available));
    for (uint32_t offset = 0;
         offset < available && result.size() < limit; ++offset) {
      const uint32_t sequence = cursor - offset;
      const auto &entry =
          entries_[(sequence - 1) % kPersistentRipSeedCapacity];
      const uint64_t checksum_before =
          __atomic_load_n(&entry.checksum, __ATOMIC_ACQUIRE);
      const uint64_t rip = entry.rip;
      const uint32_t guest_length = entry.guest_length;
      const uint32_t stored_sequence = entry.sequence;
      const uint64_t checksum_after =
          __atomic_load_n(&entry.checksum, __ATOMIC_ACQUIRE);
      if (checksum_before == 0 || checksum_before != checksum_after ||
          stored_sequence != sequence ||
          checksum_before != RipSeedChecksum(identity_, rip, guest_length,
                                             stored_sequence) ||
          !IsPersistentRipSeedRange(ranges, rip, guest_length) ||
          !seen.insert(rip).second) {
        continue;
      }
      result.push_back(rip);
    }
    return result;
  }

private:
  int fd_{-1};
  void *mapping_{MAP_FAILED};
  size_t mapping_size_{};
  uint64_t identity_{};
  PersistentRipSeedHeader *header_{};
  PersistentRipSeedEntry *entries_{};
  std::string directory_;
  std::string path_;
};

struct ThreadBinding {
  Runtime *runtime{};
  ThreadRecord *record{};
  uint32_t thunk_depth{};
};

thread_local ThreadBinding g_thread_binding{};

constexpr size_t kSignalBindingSlotCount = 1024;
static_assert((kSignalBindingSlotCount & (kSignalBindingSlotCount - 1)) == 0);

/*
 * Signal handlers must never observe a slot assembled from two different
 * bindings.  Each publication therefore owns a fresh immutable node.  Nodes
 * are deliberately process-resident: a reader interrupted between its two
 * slot loads can safely inspect the old node, and pointer identity cannot
 * suffer ABA because published addresses are never reused.
 */
struct SignalBindingNode {
  uint64_t tid{};
  Runtime *runtime{};
  ThreadRecord *record{};
  uint64_t generation{};
};

struct SignalBindingSlot {
  std::atomic<const SignalBindingNode *> binding{};
};

static_assert(std::atomic<const SignalBindingNode *>::is_always_lock_free);
std::array<SignalBindingSlot, kSignalBindingSlotCount> g_signal_bindings{};
std::atomic<uint64_t> g_next_signal_binding_generation{1};

std::optional<uint64_t> AllocateSignalBindingGeneration() {
  uint64_t generation =
      g_next_signal_binding_generation.load(std::memory_order_relaxed);
  while (generation != 0 && generation != UINT64_MAX) {
    if (g_next_signal_binding_generation.compare_exchange_weak(
            generation, generation + 1, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return generation;
    }
  }
  return std::nullopt;
}

std::mutex g_process_init_mutex;
bool g_process_initialized{};
uint32_t g_process_page_size{};
fextl::vector<FEXCore::Allocator::MemoryRegion> g_process_allocator_regions;

#if 0
// Superseded by the shared, host-tested ExecutableRegistry implementation
// below. Kept temporarily in this integration worktree for line-by-line
// comparison until the caller migration is complete.
constexpr size_t kExecutableRegistrySlotCount = 256;
constexpr size_t kMaximumExecutableRangeSize = size_t {128} * 1024 * 1024;
constexpr size_t kFexCodeGranule = 4096;
constexpr size_t kExecutableBitmapWords =
    kMaximumExecutableRangeSize / kFexCodeGranule / 64;
constexpr size_t kMaximumPublishAlignmentGap = 15;
constexpr size_t kExecutableBlockSlotCount = 1048576;
constexpr size_t kExecutableBlockBucketCount = 262144;
// Buckets are the low eighteen bits of the 4-KiB page-number hash, not unique
// physical pages. Unrelated pages can collide. Keep traversal signal-bounded
// and fail the whole publication atomically when the conservative chain
// capacity would be exceeded; never publish a partially discoverable block.
constexpr size_t kMaximumExecutableBlockBucketDepth = 128;

enum class ExecutableSlotState : uint32_t {
  Empty,
  Live,
  Retiring,
  Quarantined,
};

struct ExecutableRegistrySlot {
  std::atomic<uint32_t> state{};
  FEXCore::ExecutableCode::RangeKind kind{};
  uint32_t reserved{};
  uint64_t generation{};
  uintptr_t base{};
  size_t size{};
  size_t host_page_size{};
  std::atomic<uintptr_t> shared_published_end{};
  std::array<std::atomic<uint64_t>, kExecutableBitmapWords> published_pages{};
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);

std::array<ExecutableRegistrySlot, kExecutableRegistrySlotCount>
    g_executable_registry{};
std::mutex g_executable_registry_writer_mutex;
std::atomic<uint64_t> g_executable_registry_readers{};
std::atomic<bool> g_executable_registry_compacting{};
std::atomic<uint64_t> g_executable_registry_generation{1};
// Sticky poison is reserved for an impossible internal invariant or a failed
// lifetime/retirement protocol. Rejected admission (bad caller input, OOM,
// capacity, overlap, or a full hash chain) is local and must leave both the
// existing view and future valid publications usable.
std::atomic<bool> g_executable_registry_poisoned{};

void PoisonExecutableRegistry() noexcept {
  g_executable_registry_poisoned.store(true, std::memory_order_release);
}

struct ExecutableBlockSlot {
  uintptr_t begin{};
  uintptr_t end{};
  uintptr_t page_address{};
  uint64_t mapping_generation{};
  uint32_t next{};
  uint32_t mapping_slot{};
};

std::array<ExecutableBlockSlot, kExecutableBlockSlotCount>
    g_executable_blocks{};
std::array<std::atomic<uint32_t>, kExecutableBlockBucketCount>
    g_executable_block_buckets{};
std::atomic<uint64_t> g_executable_block_count{};

size_t ExecutableBlockBucket(uintptr_t address) noexcept {
  uint64_t page = address / kFexCodeGranule;
  page ^= page >> 33;
  page *= UINT64_C(0xff51afd7ed558ccd);
  page ^= page >> 33;
  return static_cast<size_t>(page) & (kExecutableBlockBucketCount - 1);
}

bool CheckedRange(uintptr_t base, size_t size, uintptr_t* end) noexcept {
  if (size == 0 || size > UINTPTR_MAX - base) {
    return false;
  }
  *end = base + size;
  return true;
}

ExecutableRegistrySlot* FindExecutableSlotLocked(
    FEXCore::ExecutableCode::RangeKind kind, const void* base,
    size_t size) noexcept {
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  for (auto& slot : g_executable_registry) {
    if (slot.state.load(std::memory_order_relaxed) ==
            static_cast<uint32_t>(ExecutableSlotState::Live) &&
        slot.kind == kind && slot.base == address && slot.size == size) {
      return &slot;
    }
  }
  return nullptr;
}

bool ReserveExecutableRange(void*, FEXCore::ExecutableCode::RangeKind kind,
                            const void* base, size_t size,
                            size_t host_page_size) noexcept {
  uintptr_t end{};
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  if (g_executable_registry_poisoned.load(std::memory_order_acquire) ||
      (kind != FEXCore::ExecutableCode::RangeKind::SharedJIT &&
       kind != FEXCore::ExecutableCode::RangeKind::PersistentCache) ||
      !CheckedRange(address, size, &end) ||
      size > kMaximumExecutableRangeSize || host_page_size < kFexCodeGranule ||
      (host_page_size & (host_page_size - 1)) != 0 ||
      address % host_page_size != 0 || size % host_page_size != 0) {
    return false;
  }

  std::lock_guard lock{g_executable_registry_writer_mutex};
  for (const auto& slot : g_executable_registry) {
    const auto state = static_cast<ExecutableSlotState>(
        slot.state.load(std::memory_order_relaxed));
    if (state == ExecutableSlotState::Empty) {
      continue;
    }
    uintptr_t slot_end{};
    if (!CheckedRange(slot.base, slot.size, &slot_end) ||
        (address < slot_end && slot.base < end)) {
      // A corrupt live slot is an internal invariant violation. A normal
      // overlap is merely a rejected reservation.
      if (!CheckedRange(slot.base, slot.size, &slot_end)) {
        PoisonExecutableRegistry();
      }
      return false;
    }
  }

  for (auto& slot : g_executable_registry) {
    if (slot.state.load(std::memory_order_relaxed) !=
        static_cast<uint32_t>(ExecutableSlotState::Empty)) {
      continue;
    }
    const uint64_t generation =
        g_executable_registry_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0 || generation == UINT64_MAX) {
      PoisonExecutableRegistry();
      return false;
    }
    slot.kind = kind;
    slot.generation = generation;
    slot.base = address;
    slot.size = size;
    slot.host_page_size = host_page_size;
    slot.shared_published_end.store(address, std::memory_order_relaxed);
    for (auto& word : slot.published_pages) {
      word.store(0, std::memory_order_relaxed);
    }
    slot.state.store(static_cast<uint32_t>(ExecutableSlotState::Live),
                     std::memory_order_release);
    return true;
  }

  // 256 simultaneous mappings is the explicit process budget. Exhaustion is
  // a controlled activation/allocation failure, never an untracked mapping.
  return false;
}

bool PublishExecutableRange(void*, FEXCore::ExecutableCode::RangeKind kind,
                            const void* base, size_t size) noexcept {
  uintptr_t end{};
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  if (g_executable_registry_poisoned.load(std::memory_order_acquire) ||
      !CheckedRange(address, size, &end)) {
    return false;
  }
  std::lock_guard lock{g_executable_registry_writer_mutex};
  for (auto& slot : g_executable_registry) {
    if (slot.state.load(std::memory_order_relaxed) !=
            static_cast<uint32_t>(ExecutableSlotState::Live) ||
        slot.kind != kind || address < slot.base || end > slot.base + slot.size) {
      continue;
    }
    if (kind == FEXCore::ExecutableCode::RangeKind::SharedJIT) {
      const uintptr_t published =
          slot.shared_published_end.load(std::memory_order_relaxed);
      if (address < published || address - published > kMaximumPublishAlignmentGap) {
        break;
      }
      slot.shared_published_end.store(end, std::memory_order_release);
      return true;
    }
    if ((address - slot.base) % kFexCodeGranule != 0 ||
        size % kFexCodeGranule != 0) {
      break;
    }
    const size_t first = (address - slot.base) / kFexCodeGranule;
    const size_t last = (end - slot.base) / kFexCodeGranule;
    for (size_t page = first; page < last; ++page) {
      slot.published_pages[page / 64].fetch_or(
          UINT64_C(1) << (page % 64), std::memory_order_release);
    }
    return true;
  }
  return false;
}

bool PublishExecutableBlock(void*, FEXCore::ExecutableCode::RangeKind kind,
                            const void* block_begin, size_t block_size) noexcept {
  uintptr_t block_end{};
  const uintptr_t address = reinterpret_cast<uintptr_t>(block_begin);
  if (g_executable_registry_poisoned.load(std::memory_order_acquire) ||
      (address & 3) != 0 || !CheckedRange(address, block_size, &block_end)) {
    return false;
  }

  // Serialize the complete immutable-node append with retirement. A retire
  // cannot publish Retiring until every bucket reference for this generation
  // is release-visible.
  std::lock_guard registry_lock{g_executable_registry_writer_mutex};
  uint64_t mapping_generation{};
  uint32_t mapping_slot{};
  for (uint32_t slot_index = 0;
       slot_index < g_executable_registry.size(); ++slot_index) {
    const auto& mapping = g_executable_registry[slot_index];
      if (mapping.state.load(std::memory_order_relaxed) !=
              static_cast<uint32_t>(ExecutableSlotState::Live) ||
          mapping.kind != kind || address < mapping.base ||
          block_end > mapping.base + mapping.size) {
        continue;
      }
      bool published{};
      if (mapping.kind == FEXCore::ExecutableCode::RangeKind::SharedJIT) {
        published = block_end <= mapping.shared_published_end.load(
                                     std::memory_order_acquire);
      } else {
        published = true;
        const size_t first = (address - mapping.base) / kFexCodeGranule;
        const size_t last = (block_end - 1 - mapping.base) / kFexCodeGranule;
        for (size_t page = first; page <= last; ++page) {
          if ((mapping.published_pages[page / 64].load(
                   std::memory_order_acquire) &
               (UINT64_C(1) << (page % 64))) == 0) {
            published = false;
            break;
          }
        }
      }
      if (!published) {
        break;
      }
    mapping_generation = mapping.generation;
    mapping_slot = slot_index;
    break;
  }
  if (mapping_generation == 0) {
    return false;
  }

  const size_t first_page = address / kFexCodeGranule;
  const size_t last_page = (block_end - 1) / kFexCodeGranule;
  const uint64_t reference_count = last_page - first_page + 1;
  // Persistent cache entrypoints can repeat the same compiled multiblock.
  // Deduplicate the exact extent before consuming immutable sidecar capacity.
  if (kind == FEXCore::ExecutableCode::RangeKind::PersistentCache) {
    uint32_t reference =
        g_executable_block_buckets[ExecutableBlockBucket(address)].load(
            std::memory_order_acquire);
    size_t traversed{};
    while (reference != 0 &&
           traversed++ < kMaximumExecutableBlockBucketDepth) {
      const size_t index = reference - 1;
      if (index >= kExecutableBlockSlotCount) {
        PoisonExecutableRegistry();
        return false;
      }
      const auto& block = g_executable_blocks[index];
      if (block.begin == address && block.end == block_end &&
          block.mapping_generation == mapping_generation) {
        return true;
      }
      reference = block.next;
    }
    if (reference != 0) {
      return false;
    }
  }

  const uint64_t first_index =
      g_executable_block_count.load(std::memory_order_relaxed);
  if (first_index > kExecutableBlockSlotCount ||
      reference_count > kExecutableBlockSlotCount - first_index) {
    if (first_index > kExecutableBlockSlotCount) {
      PoisonExecutableRegistry();
    }
    return false;
  }

  // Preflight every affected hash chain before reserving slots or publishing
  // even one bucket head. PublishBlock is serialized by the registry writer
  // mutex, so a successful preflight makes the append transaction infallible;
  // an overfull/corrupt bucket leaves the previous registry wholly intact.
  constexpr size_t kMaximumBlockPages = 4096;
  struct StagedBucketDepth {
    uint32_t bucket{};
    uint32_t additions{};
  };
  std::array<StagedBucketDepth, kMaximumBlockPages> staged_buckets{};
  if (reference_count > kMaximumBlockPages) {
    return false;
  }
  size_t staged_bucket_count{};
  for (uint64_t offset = 0; offset < reference_count; ++offset) {
    const uintptr_t page_address =
        (first_page + offset) * kFexCodeGranule;
    const size_t bucket_index = ExecutableBlockBucket(page_address);
    size_t staged_index{};
    for (; staged_index < staged_bucket_count; ++staged_index) {
      if (staged_buckets[staged_index].bucket == bucket_index) {
        break;
      }
    }
    if (staged_index == staged_bucket_count) {
      staged_buckets[staged_index].bucket = bucket_index;
      ++staged_bucket_count;
    }
    ++staged_buckets[staged_index].additions;
  }
  for (size_t staged_index = 0; staged_index < staged_bucket_count;
       ++staged_index) {
    const auto staged = staged_buckets[staged_index];
    uint32_t cursor = g_executable_block_buckets[staged.bucket].load(
        std::memory_order_acquire);
    size_t depth{};
    while (cursor != 0 && depth < kMaximumExecutableBlockBucketDepth) {
      const size_t cursor_index = cursor - 1;
      if (cursor_index >= first_index ||
          cursor_index >= kExecutableBlockSlotCount) {
        PoisonExecutableRegistry();
        return false;
      }
      cursor = g_executable_blocks[cursor_index].next;
      ++depth;
    }
    if (cursor != 0 ||
        staged.additions > kMaximumExecutableBlockBucketDepth - depth) {
      return false;
    }
  }

  // Initialize every immutable node before any release-published bucket can
  // expose it to a signal reader.
  for (uint64_t offset = 0; offset < reference_count; ++offset) {
    const uint64_t index = first_index + offset;
    auto& block = g_executable_blocks[index];
    block.begin = address;
    block.end = block_end;
    block.page_address = (first_page + offset) * kFexCodeGranule;
    block.mapping_generation = mapping_generation;
    block.mapping_slot = mapping_slot;
    block.next = 0;
  }
  for (uint64_t offset = 0; offset < reference_count; ++offset) {
    const uint64_t index = first_index + offset;
    auto& block = g_executable_blocks[index];
    auto& bucket = g_executable_block_buckets[
        ExecutableBlockBucket((first_page + offset) * kFexCodeGranule)];
    uint32_t head = bucket.load(std::memory_order_acquire);
    block.next = head;
    bucket.store(static_cast<uint32_t>(index + 1),
                 std::memory_order_release);
  }
  g_executable_block_count.store(first_index + reference_count,
                                 std::memory_order_release);
  return true;
}

bool PublishExecutableBlocks(
    void*, FEXCore::ExecutableCode::RangeKind kind,
    const FEXCore::ExecutableCode::BlockRange* blocks,
    size_t count) noexcept {
  constexpr size_t kMaximumBlockPages = 4096;
  struct StagedBlock {
    uintptr_t begin{};
    uintptr_t end{};
    uint64_t mapping_generation{};
    uint32_t mapping_slot{};
    uint32_t reserved{};
  };

  if (count == 0) {
    return true;
  }
  if (g_executable_registry_poisoned.load(std::memory_order_acquire) ||
      blocks == nullptr || count > kExecutableBlockSlotCount ||
      count > SIZE_MAX / sizeof(StagedBlock)) {
    return false;
  }
  std::unique_ptr<StagedBlock[]> staged{
      new (std::nothrow) StagedBlock[count]};
  std::unique_ptr<uint32_t[]> bucket_additions{
      new (std::nothrow) uint32_t[kExecutableBlockBucketCount]{}};
  if (!staged || !bucket_additions) {
    return false;
  }

  std::lock_guard registry_lock{g_executable_registry_writer_mutex};
  const uint64_t first_index =
      g_executable_block_count.load(std::memory_order_relaxed);
  if (first_index > kExecutableBlockSlotCount) {
    PoisonExecutableRegistry();
    return false;
  }

  // Resolve every extent to a live, fully published mapping before reserving
  // one immutable node. No stage below this point mutates the registry until
  // all capacity and bounded-chain checks have also passed.
  for (size_t block_index = 0; block_index < count; ++block_index) {
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(blocks[block_index].Begin);
    uintptr_t end{};
    if ((begin & 3) != 0 || (blocks[block_index].Size & 3) != 0 ||
        !CheckedRange(begin, blocks[block_index].Size, &end)) {
      return false;
    }

    bool found{};
    for (uint32_t slot_index = 0;
         slot_index < g_executable_registry.size(); ++slot_index) {
      const auto& mapping = g_executable_registry[slot_index];
      if (mapping.state.load(std::memory_order_relaxed) !=
              static_cast<uint32_t>(ExecutableSlotState::Live) ||
          mapping.kind != kind || begin < mapping.base ||
          end > mapping.base + mapping.size) {
        continue;
      }
      bool published = kind ==
                           FEXCore::ExecutableCode::RangeKind::SharedJIT
                           ? end <= mapping.shared_published_end.load(
                                        std::memory_order_acquire)
                           : true;
      if (kind == FEXCore::ExecutableCode::RangeKind::PersistentCache) {
        const size_t first =
            (begin - mapping.base) / kFexCodeGranule;
        const size_t last =
            (end - 1 - mapping.base) / kFexCodeGranule;
        for (size_t page = first; page <= last; ++page) {
          if ((mapping.published_pages[page / 64].load(
                   std::memory_order_acquire) &
               (UINT64_C(1) << (page % 64))) == 0) {
            published = false;
            break;
          }
        }
      }
      if (!published) {
        break;
      }
      staged[block_index] = StagedBlock{
          begin, end, mapping.generation, slot_index, 0};
      found = true;
      break;
    }
    if (!found) {
      return false;
    }
  }

  std::sort(staged.get(), staged.get() + count,
            [](const StagedBlock& lhs, const StagedBlock& rhs) {
              if (lhs.mapping_generation != rhs.mapping_generation) {
                return lhs.mapping_generation < rhs.mapping_generation;
              }
              if (lhs.begin != rhs.begin) {
                return lhs.begin < rhs.begin;
              }
              return lhs.end < rhs.end;
            });

  // Compact duplicate entrypoints and extents already registered for this
  // mapping generation. Existing chains are bounded before accepting a match.
  size_t unique_count{};
  uint64_t total_references{};
  for (size_t block_index = 0; block_index < count; ++block_index) {
    const auto candidate = staged[block_index];
    if (unique_count != 0) {
      const auto& previous = staged[unique_count - 1];
      if (previous.mapping_generation == candidate.mapping_generation &&
          previous.begin == candidate.begin && previous.end == candidate.end) {
        continue;
      }
    }

    bool already_published{};
    uint32_t cursor =
        g_executable_block_buckets[ExecutableBlockBucket(candidate.begin)]
            .load(std::memory_order_acquire);
    size_t depth{};
    while (cursor != 0 && depth++ < kMaximumExecutableBlockBucketDepth) {
      const size_t cursor_index = cursor - 1;
      if (cursor_index >= first_index ||
          cursor_index >= kExecutableBlockSlotCount) {
        PoisonExecutableRegistry();
        return false;
      }
      const auto& existing = g_executable_blocks[cursor_index];
      if (existing.mapping_generation == candidate.mapping_generation &&
          existing.begin == candidate.begin &&
          existing.end == candidate.end) {
        already_published = true;
        break;
      }
      cursor = existing.next;
    }
    if (!already_published && cursor != 0 &&
        depth >= kMaximumExecutableBlockBucketDepth) {
      return false;
    }
    if (already_published) {
      continue;
    }

    const size_t first_page = candidate.begin / kFexCodeGranule;
    const size_t last_page = (candidate.end - 1) / kFexCodeGranule;
    const uint64_t references = last_page - first_page + 1;
    if (references > kMaximumBlockPages ||
        references > kExecutableBlockSlotCount - first_index -
                         total_references) {
      return false;
    }
    for (uint64_t offset = 0; offset < references; ++offset) {
      const size_t bucket = ExecutableBlockBucket(
          (first_page + offset) * kFexCodeGranule);
      if (bucket_additions[bucket] == UINT32_MAX) {
        PoisonExecutableRegistry();
        return false;
      }
      ++bucket_additions[bucket];
    }
    total_references += references;
    staged[unique_count++] = candidate;
  }

  for (size_t bucket_index = 0;
       bucket_index < kExecutableBlockBucketCount; ++bucket_index) {
    if (bucket_additions[bucket_index] == 0) {
      continue;
    }
    uint32_t cursor =
        g_executable_block_buckets[bucket_index].load(
            std::memory_order_acquire);
    size_t depth{};
    while (cursor != 0 && depth < kMaximumExecutableBlockBucketDepth) {
      const size_t cursor_index = cursor - 1;
      if (cursor_index >= first_index ||
          cursor_index >= kExecutableBlockSlotCount) {
        PoisonExecutableRegistry();
        return false;
      }
      cursor = g_executable_blocks[cursor_index].next;
      ++depth;
    }
    if (cursor != 0 ||
        bucket_additions[bucket_index] >
            kMaximumExecutableBlockBucketDepth - depth) {
      return false;
    }
  }

  // From here onward no operation can fail. Initialize every immutable node,
  // then release-publish bucket heads, and publish the final count last. The
  // caller must not expose LookupCache mappings until this transaction returns.
  uint64_t next_index = first_index;
  for (size_t block_index = 0; block_index < unique_count; ++block_index) {
    const auto& candidate = staged[block_index];
    const size_t first_page = candidate.begin / kFexCodeGranule;
    const size_t last_page = (candidate.end - 1) / kFexCodeGranule;
    for (size_t page = first_page; page <= last_page; ++page) {
      auto& block = g_executable_blocks[next_index++];
      block.begin = candidate.begin;
      block.end = candidate.end;
      block.page_address = page * kFexCodeGranule;
      block.mapping_generation = candidate.mapping_generation;
      block.mapping_slot = candidate.mapping_slot;
      block.next = 0;
    }
  }

  next_index = first_index;
  for (size_t block_index = 0; block_index < unique_count; ++block_index) {
    const auto& candidate = staged[block_index];
    const size_t first_page = candidate.begin / kFexCodeGranule;
    const size_t last_page = (candidate.end - 1) / kFexCodeGranule;
    for (size_t page = first_page; page <= last_page; ++page) {
      auto& block = g_executable_blocks[next_index];
      auto& bucket =
          g_executable_block_buckets[ExecutableBlockBucket(
              page * kFexCodeGranule)];
      block.next = bucket.load(std::memory_order_acquire);
      bucket.store(static_cast<uint32_t>(next_index + 1),
                   std::memory_order_release);
      ++next_index;
    }
  }
  g_executable_block_count.store(first_index + total_references,
                                 std::memory_order_release);
  return true;
}

bool BeginRetireExecutableRange(void*,
                                FEXCore::ExecutableCode::RangeKind kind,
                                const void* base, size_t size) noexcept {
  ExecutableRegistrySlot* target{};
  std::unique_lock lock{g_executable_registry_writer_mutex};
  target = FindExecutableSlotLocked(kind, base, size);
  if (target == nullptr) {
    return false;
  }
  target->state.store(static_cast<uint32_t>(ExecutableSlotState::Retiring),
                      std::memory_order_seq_cst);
  g_executable_registry_compacting.store(true, std::memory_order_seq_cst);

  constexpr size_t kRetireYieldBudget = 4096;
  for (size_t attempt = 0; attempt < kRetireYieldBudget; ++attempt) {
    if (g_executable_registry_readers.load(std::memory_order_seq_cst) == 0) {
      for (auto& bucket : g_executable_block_buckets) {
        bucket.store(0, std::memory_order_relaxed);
      }
      const uint64_t old_count = std::min<uint64_t>(
          g_executable_block_count.load(std::memory_order_relaxed),
          kExecutableBlockSlotCount);
      uint64_t new_count{};
      for (uint64_t old_index = 0; old_index < old_count; ++old_index) {
        const auto old = g_executable_blocks[old_index];
        if (old.mapping_generation == target->generation) {
          continue;
        }
        auto& replacement = g_executable_blocks[new_count];
        replacement = old;
        auto& bucket = g_executable_block_buckets[
            ExecutableBlockBucket(replacement.page_address)];
        replacement.next = bucket.load(std::memory_order_relaxed);
        bucket.store(static_cast<uint32_t>(new_count + 1),
                     std::memory_order_relaxed);
        ++new_count;
      }
      g_executable_block_count.store(new_count, std::memory_order_release);
      g_executable_registry_compacting.store(false,
                                             std::memory_order_seq_cst);
      return true;
    }
    if (attempt >= 64) {
      ::sched_yield();
    }
  }

  // A registry lease protects only metadata readers. A last shared_ptr owns
  // the executable lifetime; if readers nevertheless fail to quiesce, never
  // unmap/reuse this VA and fail all future reservations closed.
  target->state.store(static_cast<uint32_t>(ExecutableSlotState::Quarantined),
                      std::memory_order_release);
  g_executable_registry_compacting.store(false, std::memory_order_seq_cst);
  PoisonExecutableRegistry();
  return false;
}

void FinishRetireExecutableRange(void*,
                                 FEXCore::ExecutableCode::RangeKind kind,
                                 const void* base, size_t size,
                                 bool unmapped) noexcept {
  std::lock_guard lock{g_executable_registry_writer_mutex};
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  for (auto& slot : g_executable_registry) {
    if (slot.kind != kind || slot.base != address || slot.size != size ||
        slot.state.load(std::memory_order_relaxed) !=
            static_cast<uint32_t>(ExecutableSlotState::Retiring)) {
      continue;
    }
    if (unmapped) {
      slot.state.store(static_cast<uint32_t>(ExecutableSlotState::Empty),
                       std::memory_order_release);
    } else {
      slot.state.store(static_cast<uint32_t>(ExecutableSlotState::Quarantined),
                       std::memory_order_release);
      PoisonExecutableRegistry();
    }
    return;
  }
  PoisonExecutableRegistry();
}
#endif

using ProductionExecutableRegistry =
    lsx4::fex2608::ExecutableRegistry<256, size_t{128} * 1024 * 1024, 4096, 15,
                                      1048576, 262144, 128, 4096, 4096>;
ProductionExecutableRegistry g_executable_registry_store{};

std::optional<lsx4::fex2608::ExecutableRangeKind>
ToRegistryKind(FEXCore::ExecutableCode::RangeKind kind) noexcept {
  switch (kind) {
  case FEXCore::ExecutableCode::RangeKind::SharedJIT:
    return lsx4::fex2608::ExecutableRangeKind::SharedJIT;
  case FEXCore::ExecutableCode::RangeKind::PersistentCache:
    return lsx4::fex2608::ExecutableRangeKind::PersistentCache;
  }
  return std::nullopt;
}

bool ReserveExecutableRange(void *, FEXCore::ExecutableCode::RangeKind kind,
                            const void *base, size_t size,
                            size_t host_page_size) noexcept {
  const auto translated = ToRegistryKind(kind);
  return translated.has_value() && g_executable_registry_store.Reserve(
                                       *translated, base, size, host_page_size);
}

bool PublishExecutableRange(void *, FEXCore::ExecutableCode::RangeKind kind,
                            const void *base, size_t size) noexcept {
  const auto translated = ToRegistryKind(kind);
  return translated.has_value() &&
         g_executable_registry_store.PublishRange(*translated, base, size);
}

bool PublishExecutableBlock(void *, FEXCore::ExecutableCode::RangeKind kind,
                            const void *block_begin,
                            size_t block_size) noexcept {
  const auto translated = ToRegistryKind(kind);
  const bool published =
      translated.has_value() && g_executable_registry_store.PublishBlock(
                                    *translated, block_begin, block_size);
  if (published) {
    lsx4::fex2608::RecordProofPublishedNativeBlock(block_size);
  }
  return published;
}

bool PublishExecutableBlocks(void *, FEXCore::ExecutableCode::RangeKind kind,
                             const FEXCore::ExecutableCode::BlockRange *blocks,
                             size_t count) noexcept {
  const auto translated = ToRegistryKind(kind);
  if (!translated.has_value() || (count != 0 && blocks == nullptr) ||
      count > SIZE_MAX / sizeof(lsx4::fex2608::ExecutableBlockRange)) {
    return false;
  }
  std::unique_ptr<lsx4::fex2608::ExecutableBlockRange[]> translated_blocks{
      count == 0 ? nullptr
                 : new (std::nothrow)
                       lsx4::fex2608::ExecutableBlockRange[count]};
  if (count != 0 && !translated_blocks) {
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    translated_blocks[index] = {blocks[index].Begin, blocks[index].Size};
  }
  const bool published = g_executable_registry_store.PublishBlocks(
      *translated, translated_blocks.get(), count);
  if (published) {
    for (size_t index = 0; index < count; ++index) {
      lsx4::fex2608::RecordProofPublishedNativeBlock(blocks[index].Size);
    }
  }
  return published;
}

bool BeginRetireExecutableRange(void *, FEXCore::ExecutableCode::RangeKind kind,
                                const void *base, size_t size) noexcept {
  const auto translated = ToRegistryKind(kind);
  return translated.has_value() &&
         g_executable_registry_store.BeginRetire(*translated, base, size);
}

void FinishRetireExecutableRange(void *,
                                 FEXCore::ExecutableCode::RangeKind kind,
                                 const void *base, size_t size,
                                 bool unmapped) noexcept {
  const auto translated = ToRegistryKind(kind);
  if (!translated.has_value()) {
    return;
  }
  (void)g_executable_registry_store.FinishRetire(*translated, base, size,
                                                 unmapped);
}

const FEXCore::ExecutableCode::ObserverV3 g_executable_observer{
    .StructSize = sizeof(FEXCore::ExecutableCode::ObserverV3),
    .Version = FEXCore::ExecutableCode::OBSERVER_ABI_V3,
    .Opaque = nullptr,
    .Reserve = ReserveExecutableRange,
    .Publish = PublishExecutableRange,
    .PublishBlock = PublishExecutableBlock,
    .BeginRetire = BeginRetireExecutableRange,
    .FinishRetire = FinishRetireExecutableRange,
    .PublishBlocks = PublishExecutableBlocks,
};

uint64_t CurrentTid() { return lsx4::fex2608::HostCurrentTid(); }

struct HardwareTsoAttempt {
  bool enabled{};
  long get_before{-1};
  int get_before_errno{};
  long set_result{-1};
  int set_errno{};
  long get_after{-1};
  int get_after_errno{};
};

HardwareTsoAttempt TryEnableHardwareTsoForCurrentThread() noexcept {
  const int saved_errno = errno;
  HardwareTsoAttempt attempt{};
#if defined(__APPLE__)
  attempt.get_before_errno = ENOTSUP;
  return attempt;
#else
  errno = 0;
  attempt.get_before = ::prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
  attempt.get_before_errno = attempt.get_before == -1 ? errno : 0;
  if (attempt.get_before == PR_SET_MEM_MODEL_TSO) {
    attempt.enabled = true;
    attempt.get_after = attempt.get_before;
    errno = saved_errno;
    return attempt;
  }
  if (attempt.get_before != PR_SET_MEM_MODEL_DEFAULT) {
    errno = saved_errno;
    return attempt;
  }

  errno = 0;
  attempt.set_result = ::prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0);
  attempt.set_errno = attempt.set_result == -1 ? errno : 0;
  // SET success is authoritative.  On an always-TSO implementation the
  // diagnostic GET may legitimately continue to report DEFAULT.
  attempt.enabled = attempt.set_result == 0;
  if (attempt.enabled) {
    errno = 0;
    attempt.get_after = ::prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
    attempt.get_after_errno = attempt.get_after == -1 ? errno : 0;
  }
  errno = saved_errno;
  return attempt;
#endif
}

void LogHardwareTsoAttempt(const Lsx4FexHostCallbacksV3 *callbacks,
                           const char *phase, const HardwareTsoAttempt &attempt,
                           uint64_t sequence = 0) {
  if (callbacks == nullptr || callbacks->log == nullptr) {
    return;
  }
  char message[320]{};
  std::snprintf(
      message, sizeof(message),
      "[FEX_HARDWARE_TSO_THREAD] phase=%s sequence=%llu tid=%llu getBefore=%ld "
      "getBeforeErrno=%d set=%ld setErrno=%d getAfter=%ld "
      "getAfterErrno=%d enabled=%d",
      phase != nullptr ? phase : "-", static_cast<unsigned long long>(sequence),
      static_cast<unsigned long long>(CurrentTid()), attempt.get_before,
      attempt.get_before_errno, attempt.set_result, attempt.set_errno,
      attempt.get_after, attempt.get_after_errno, attempt.enabled ? 1 : 0);
  callbacks->log(callbacks->callback_context, attempt.enabled ? 4 : 6,
                 "strong-memory", message);
}

void RestoreHardwareTsoAfterProbe(const Lsx4FexHostCallbacksV3 *callbacks,
                                  const char *phase,
                                  const HardwareTsoAttempt &attempt) noexcept {
  #if !defined(__APPLE__)
  if (!attempt.enabled || attempt.get_before != PR_SET_MEM_MODEL_DEFAULT ||
      attempt.set_result != 0) {
    return;
  }
  const int saved_errno = errno;
  errno = 0;
  const long restore_result =
      ::prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_DEFAULT, 0, 0, 0);
  const int restore_errno = restore_result == -1 ? errno : 0;
  if (callbacks != nullptr && callbacks->log != nullptr) {
    char message[224]{};
    std::snprintf(message, sizeof(message),
                  "[FEX_HARDWARE_TSO_PROBE_RESTORE] phase=%s tid=%llu "
                  "result=%ld errno=%d",
                  phase != nullptr ? phase : "-",
                  static_cast<unsigned long long>(CurrentTid()), restore_result,
                  restore_errno);
    callbacks->log(callbacks->callback_context, restore_result == 0 ? 4 : 6,
                   "strong-memory", message);
  }
  errno = saved_errno;
#else
  (void)callbacks; (void)phase; (void)attempt;
#endif
}

size_t SignalBindingStart(uint64_t tid) {
  tid ^= tid >> 33;
  tid *= UINT64_C(0xff51afd7ed558ccd);
  tid ^= tid >> 33;
  return static_cast<size_t>(tid) & (kSignalBindingSlotCount - 1);
}

bool PublishSignalBinding(uint64_t tid, Runtime *runtime,
                          ThreadRecord *record) {
  const auto generation = AllocateSignalBindingGeneration();
  if (!generation.has_value()) {
    return false;
  }
  auto *published =
      new (std::nothrow) SignalBindingNode{tid, runtime, record, *generation};
  if (published == nullptr) {
    return false;
  }
  const size_t start = SignalBindingStart(tid);
  for (size_t probe = 0; probe < kSignalBindingSlotCount; ++probe) {
    auto &slot =
        g_signal_bindings[(start + probe) & (kSignalBindingSlotCount - 1)];
    const SignalBindingNode *empty = nullptr;
    if (slot.binding.compare_exchange_strong(empty, published,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
      return true;
    }
  }
  delete published;
  return false;
}

ThreadBinding FindSignalBinding(uint64_t tid) {
  const size_t start = SignalBindingStart(tid);
  for (size_t probe = 0; probe < kSignalBindingSlotCount; ++probe) {
    auto &slot =
        g_signal_bindings[(start + probe) & (kSignalBindingSlotCount - 1)];
    for (unsigned retry = 0; retry != 2; ++retry) {
      const SignalBindingNode *binding =
          slot.binding.load(std::memory_order_acquire);
      if (binding == nullptr || binding->tid != tid) {
        break;
      }
      ThreadBinding result{binding->runtime, binding->record};
      if (slot.binding.load(std::memory_order_acquire) == binding) {
        return result;
      }
      /* Pointer identity cannot ABA; retry once for a same-TID rebind. */
    }
  }
  return {};
}

void RemoveSignalBinding(uint64_t tid, Runtime *runtime, ThreadRecord *record) {
  const size_t start = SignalBindingStart(tid);
  for (size_t probe = 0; probe < kSignalBindingSlotCount; ++probe) {
    auto &slot =
        g_signal_bindings[(start + probe) & (kSignalBindingSlotCount - 1)];
    const SignalBindingNode *binding =
        slot.binding.load(std::memory_order_acquire);
    if (binding == nullptr || binding->tid != tid ||
        binding->runtime != runtime || binding->record != record) {
      continue;
    }
    if (slot.binding.compare_exchange_strong(binding, nullptr,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      return;
    }
  }
}

uint64_t CurrentPthreadHandle() {
  const pthread_t self = pthread_self();
  uint64_t value{};
  static_assert(sizeof(self) <= sizeof(value));
  std::memcpy(&value, &self, sizeof(self));
  return value;
}

bool AddOverflows(uint64_t base, uint64_t size) {
  return size > std::numeric_limits<uint64_t>::max() - base;
}

uint64_t HashCpuTuning(const Lsx4FexCpuTuningV1 &source) {
  Lsx4FexCpuTuningV1 copy = source;
  copy.reserved0 = 0;
  copy.identity = 0;
  const auto *bytes = reinterpret_cast<const uint8_t *>(&copy);
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t index = 0; index < sizeof(copy); ++index) {
    hash ^= bytes[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

Lsx4FexCpuTuningV1 BalancedCpuTuning() {
  Lsx4FexCpuTuningV1 profile{};
  profile.struct_size = sizeof(profile);
  profile.abi_version = LSX4_FEX_CPU_TUNING_ABI_V1;
  profile.preset = 1;
  profile.execution_mode = 0;
  profile.fast_memory = 1;
  profile.memory_manager = 1;
  profile.block_linking = 1;
  profile.persistent_cache = 1;
  profile.function_replacements = 1;
  profile.guest_clock_percent = 100;
  profile.cycle_skip = 0;
  profile.idle_loop_skip = 1;
  profile.wait_loop_detection = 1;
  profile.spin_loop_detection = 1;
  profile.max_guest_workers = 6;
  profile.fast_event_timing = 1;
  profile.jit_block_size = 1;
  profile.fp_accuracy = 0;
  profile.atomics = 1;
  profile.memory_exceptions = 1;
  profile.icache_accuracy = 1;
  profile.jit_feature_mask = 0;
  profile.jit_disable_mask = 0;
  profile.cpu_gpu_dual_thread = 1;
  profile.scheduler = 3;
  profile.preferred_heavy_threads = 4;
  profile.subsystem_multithreading = 1;
  profile.separate_io_thread = 1;
  profile.separate_audio_hle_thread = 1;
  profile.guest_thread_delay_mode = 0;
  profile.sync_on_idle_skip = 1;
  profile.cpu_gpu_sync = 1;
  profile.hle_mode = 1;
  profile.hle_fast_timers = 1;
  profile.per_game_profile = 2;
  profile.auto_game_db = 1;
  profile.identity = HashCpuTuning(profile);
  return profile;
}

bool IsCpuTuningValid(const Lsx4FexCpuTuningV1 *profile) {
  if (profile == nullptr || profile->struct_size != sizeof(*profile) ||
      profile->abi_version != LSX4_FEX_CPU_TUNING_ABI_V1 ||
      profile->identity != HashCpuTuning(*profile) || profile->preset > 4 ||
      profile->execution_mode != 0 || profile->fast_memory == 0 ||
      profile->fast_memory > 2 ||
      profile->memory_manager > 3 || profile->persistent_cache > 2 ||
      profile->function_replacements > 2 ||
      profile->guest_clock_percent < 25 || profile->guest_clock_percent > 400 ||
      profile->cycle_skip > 3 || profile->idle_loop_skip > 2 ||
      profile->wait_loop_detection > 2 || profile->spin_loop_detection > 2 ||
      profile->max_guest_workers < 1 || profile->max_guest_workers > 32 ||
      profile->jit_block_size > 2 || profile->fp_accuracy > 2 ||
      profile->atomics > 2 || profile->jit_feature_mask > 2 ||
      profile->jit_disable_mask >= (UINT32_C(1) << 18) ||
      profile->cpu_gpu_dual_thread > 2 ||
      profile->scheduler > 3 || profile->preferred_heavy_threads < 1 ||
      profile->preferred_heavy_threads > 8 ||
      profile->subsystem_multithreading > 2 ||
      profile->separate_io_thread > 2 ||
      profile->separate_audio_hle_thread > 2 ||
      profile->guest_thread_delay_mode > 1 ||
      profile->guest_thread_delay_us > 10000 || profile->cpu_gpu_sync > 2 ||
      profile->hle_mode > 2 || profile->fast_event_timing > 2 ||
      profile->hle_fast_timers > 2 || profile->per_game_profile > 2 ||
      profile->reserved0 != 0 ||
      profile->memory_manager == 0 || profile->memory_manager == 2 ||
      profile->ignore_bad_accesses != 0) {
    return false;
  }
  const uint32_t bits[] = {
      profile->block_linking,
      profile->accurate_cache_line_stores,
      profile->accurate_dma_timing,
      profile->memory_exceptions,
      profile->icache_accuracy,
      profile->ignore_bad_accesses,
      profile->sync_on_idle_skip,
      profile->force_real_clock_sync,
      profile->auto_game_db,
  };
  for (const uint32_t bit : bits) {
    if (bit > 1) {
      return false;
    }
  }
  if ((profile->jit_feature_mask != 2 && profile->jit_disable_mask != 0) ||
      (profile->force_real_clock_sync != 0 &&
       (profile->guest_clock_percent != 100 || profile->cycle_skip != 0))) {
    return false;
  }
  return true;
}

bool IsHostFeatureRequestValid(const Lsx4Fex2608BridgeHostFeaturesV1 *request) {
  return request != nullptr &&
         request->struct_size >= sizeof(Lsx4Fex2608BridgeHostFeaturesV1) &&
         request->abi_version == LSX4_FEX2608_CONTEXT_BRIDGE_ABI &&
         request->page_size != 0 &&
         (request->page_size & (request->page_size - 1)) == 0;
}

bool HostHasCortexX4();

bool EnsureProcessInitialized(uint32_t page_size) {
  std::lock_guard lock{g_process_init_mutex};
  if (g_process_initialized) {
    return page_size == g_process_page_size;
  }
  if (!FEXCore::ExecutableCode::InstallObserverV3(&g_executable_observer)) {
    return false;
  }
  // PS4 mapped entries are 64-bit guests. Match FEXInterpreter's x86-64
  // allocator contract: keep host allocations in the otherwise unavailable
  // upper canonical half when the host exposes it, and never steal the lower
  // guest VA ranges from 4 GiB upward (the SetupHooks path is for 32-bit
  // guests). These regions intentionally live for the process-resident bridge.
#if !defined(__APPLE__)
  g_process_allocator_regions =
      FEXCore::Allocator::Setup48BitAllocatorIfExists(page_size);
#endif
  FEXCore::Config::Initialize();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_VECTORTSOENABLED, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MEMCPYSETTSOENABLED, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_HALFBARRIERTSOENABLED, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, "5000");
  const bool has_cortex_x4 = HostHasCortexX4();
  const char *strict_split_locks =
      std::getenv("LSX4_FEX_STRICT_SPLIT_LOCKS");
  const bool strict_split_locks_requested =
      strict_split_locks != nullptr &&
      std::strcmp(strict_split_locks, "1") == 0;
  const bool strict_split_locks_effective =
      Lsx4::Fex2608::SelectStrictSplitLockPolicy(
          strict_split_locks_requested, has_cortex_x4);
  FEXCore::Config::Set(FEXCore::Config::CONFIG_STRICTINPROCESSSPLITLOCKS,
                       strict_split_locks_effective ? "1" : "0");
  const char *low_memory_cache = std::getenv("LSX4_FEX_LOW_MEMORY_CACHE");
  const bool prefer_low_memory =
      low_memory_cache != nullptr && std::strcmp(low_memory_cache, "1") == 0;
  const Lsx4::Fex2608::LookupCachePolicy lookup_cache_policy =
      Lsx4::Fex2608::SelectLookupCachePolicy(prefer_low_memory, has_cortex_x4);
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLEL2CACHE,
                       lookup_cache_policy.disable_l2 ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DYNAMICL1CACHE,
                       lookup_cache_policy.dynamic_l1 ? "1" : "0");
  std::fprintf(stderr,
               "FEX2608_LOOKUP_CACHE low_memory=%d cortex_x4=%d "
               "disable_l2=%d dynamic_l1=%d\n",
               prefer_low_memory ? 1 : 0, has_cortex_x4 ? 1 : 0,
               lookup_cache_policy.disable_l2 ? 1 : 0,
               lookup_cache_policy.dynamic_l1 ? 1 : 0);
#ifdef __ANDROID__
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_LOOKUP_CACHE_EFFECTIVE] lowMemory=%d cortexX4=%d "
      "disableL2=%d dynamicL1=%d",
      prefer_low_memory ? 1 : 0, has_cortex_x4 ? 1 : 0,
      lookup_cache_policy.disable_l2 ? 1 : 0,
      lookup_cache_policy.dynamic_l1 ? 1 : 0);
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_STRICT_SPLIT_LOCKS_EFFECTIVE] requested=%d cortexX4=%d "
      "effective=%d",
      strict_split_locks_requested ? 1 : 0, has_cortex_x4 ? 1 : 0,
      strict_split_locks_effective ? 1 : 0);
#endif
  g_process_page_size = page_size;
  g_process_initialized = true;
  return true;
}

struct HostCpuMidrDiscovery {
  std::vector<uint32_t> midrs;
  size_t readable_count{};
  bool disable_tso_imm9{};
  bool has_cortex_x4{};
};

bool ReadHostCpuMidr(size_t cpu_index, uint32_t *midr) {
  char path[96]{};
  const int path_size = std::snprintf(
      path, sizeof(path),
      "/sys/devices/system/cpu/cpu%zu/regs/identification/midr_el1", cpu_index);
  if (path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(path)) {
    return false;
  }

  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    return false;
  }

  std::array<char, 32> data{};
  ssize_t size{};
  do {
    size = ::read(fd, data.data(), data.size() - 1);
  } while (size == -1 && errno == EINTR);
  ::close(fd);
  if (size <= 0) {
    return false;
  }

  return Lsx4::Fex2608::ParseLinuxMidr(data.data(), static_cast<size_t>(size),
                                       midr);
}

const HostCpuMidrDiscovery &GetHostCpuMidrs() {
  // C++ static initialization is process-wide and thread-safe.
  // BuildHostFeatures may run for multiple contexts, but sysfs is read only for
  // the first one.
  static const HostCpuMidrDiscovery discovery = [] {
    HostCpuMidrDiscovery result{};
    const long configured_cpus = ::sysconf(_SC_NPROCESSORS_CONF);
    if (configured_cpus > 0) {
      result.midrs.resize(static_cast<size_t>(configured_cpus));
      for (size_t cpu = 0; cpu < result.midrs.size(); ++cpu) {
        uint32_t midr{};
        if (!ReadHostCpuMidr(cpu, &midr)) {
          continue;
        }
        result.midrs[cpu] = midr;
        ++result.readable_count;
        result.disable_tso_imm9 |=
            Lsx4::Fex2608::IsErratum3877900Affected(midr);
        result.has_cortex_x4 |= Lsx4::Fex2608::IsCortexX4(midr);
      }
    }

    // Retain the old bridge behavior if Android does not expose any MIDR.
    if (result.readable_count == 0) {
      result.midrs.clear();
      result.midrs.push_back(0);
    }
    std::fprintf(stderr,
                 "FEX2608_HOST_MIDR readable=%zu slots=%zu tso_imm9_quirk=%d "
                 "cortex_x4=%d\n",
                 result.readable_count, result.midrs.size(),
                 result.disable_tso_imm9 ? 1 : 0, result.has_cortex_x4 ? 1 : 0);
    return result;
  }();
  return discovery;
}

bool HostHasCortexX4() { return GetHostCpuMidrs().has_cortex_x4; }

FEXCore::HostFeatures
BuildHostFeatures(const Lsx4Fex2608BridgeHostFeaturesV1 &request) {
  FEXCore::HostFeatures features{};
  features.DCacheLineSize = request.dcache_line_size;
  features.ICacheLineSize = request.icache_line_size;
  features.SupportsCacheMaintenanceOps = false;
  features.SupportsAtomics =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_LSE) != 0;
  features.SupportsFlagM =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_FLAGM) != 0;
  features.SupportsFlagM2 =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_FLAGM2) != 0;
  features.SupportsRCPC =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_LRCPC) != 0;
  features.SupportsTSOImm9 =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_ILRCPC) != 0;
  features.SupportsSVE128 =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_SVE128) != 0;
  features.SupportsSVE256 =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_SVE256) != 0;
  features.SupportsAES = (request.feature_mask & LSX4_FEX2608_BRIDGE_AES) != 0;
  features.SupportsAES256 = features.SupportsAES;
  features.SupportsPMULL_128Bit =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_PMULL) != 0;
  features.SupportsSHA = (request.feature_mask & LSX4_FEX2608_BRIDGE_SHA) != 0;
  features.SupportsCRC =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_CRC32) != 0;
  features.SupportsAFP = (request.feature_mask & LSX4_FEX2608_BRIDGE_AFP) != 0;
  features.SupportsFCMA =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_FCMA) != 0;
  features.SupportsFRINTTS =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_FRINTTS) != 0;
  features.SupportsECV =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_ECV) != 0;
  features.SupportsRPRES =
      (request.feature_mask & LSX4_FEX2608_BRIDGE_RPRES) != 0;
  features.SupportsAVX = true;
  features.SupportsPreserveAllABI = true;
  features.SupportsFloatExceptions = false;
  features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
  // Preserve the bridge's established virtual CPU topology on every host.
  // Real MIDRs are consulted only for the Cortex-X4 LDAPUR erratum below;
  // exposing the physical vector here would also change guest CPUID core count
  // and could regress unaffected Oryon/8 Gen 5 devices.
  features.CPUMIDRs.push_back(0);
  const HostCpuMidrDiscovery &midr_discovery = GetHostCpuMidrs();
  if (features.SupportsTSOImm9 && midr_discovery.disable_tso_imm9) {
    features.SupportsTSOImm9 = false;
  }
#ifdef __ANDROID__
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_CODEGEN_FEATURES] fcma=%d frintts=%d ecv=%d afp=%d "
      "sve128=%d sve256=%d rpres=%d",
      features.SupportsFCMA ? 1 : 0, features.SupportsFRINTTS ? 1 : 0,
      features.SupportsECV ? 1 : 0, features.SupportsAFP ? 1 : 0,
      features.SupportsSVE128 ? 1 : 0, features.SupportsSVE256 ? 1 : 0,
      features.SupportsRPRES ? 1 : 0);
#endif
  return features;
}

bool IsSnapshotHeaderValid(const Lsx4FexExecutableRangeSnapshotV5 *snapshot) {
  return snapshot != nullptr &&
         snapshot->struct_size >= sizeof(Lsx4FexExecutableRangeSnapshotV5) &&
         snapshot->abi_version == LSX4_FEX_EXECUTABLE_RANGE_ABI_V5 &&
         snapshot->range_count <= snapshot->capacity &&
         (snapshot->range_count == 0 || snapshot->ranges != nullptr);
}

bool IsThreadIdentityHeaderValid(const Lsx4FexThreadIdentityV3 *thread) {
  return thread != nullptr &&
         thread->struct_size >= sizeof(Lsx4FexThreadIdentityV3) &&
         thread->abi_version == LSX4_FEX_BACKEND_ABI_V3;
}

struct ThreadRecord {
  struct LeafHleDispositionCacheEntry {
    uint64_t native_function{};
    bool rejected{};
  };

  std::mutex mutex;
  const Lsx4FexHostCallbacksV3 *host_callbacks{};
  Lsx4FexThreadIdentityV3 identity{};
  Lsx4FexGuestTlsStateV3 tls{};
  bool tls_configured{};
  std::atomic<bool> active{};
  std::atomic<bool> finalized{};
  std::atomic<uint64_t> bound_tid{};
  bool wake_signal_was_blocked{};
  std::atomic<uint64_t> context_epoch{};
  uint64_t callret_reset_sp{};
  bool nonlocal_pending{};
  FEXCore::Core::InternalThreadState *thread{};
  FEXCore::Core::CPUState::gdt_segment gdt[32]{};
  void *callret_allocation{};
  size_t callret_allocation_size{};
  void *scratch_allocation{};
  size_t scratch_allocation_size{};
  uint64_t scratch_base{};
  uint64_t scratch_size{};
  void *deferred_stack_allocation{};
  size_t deferred_stack_allocation_size{};
  uint64_t deferred_stack_base{};
  uint64_t deferred_stack_size{};
  uint64_t stack_base{};
  uint64_t stack_size{};
  std::array<LeafHleDispositionCacheEntry, 32> leaf_hle_dispositions{};
  std::array<LeafHleDispositionCacheEntry, 32> scalar_fp_hle_dispositions{};
  lsx4::fex2608::ResumeHleFramePools hle_frame_pools{};
  bool guest_exit_requested{};
  uint64_t guest_exit_code{};
  bool controlled_failure{};
  int32_t failure_status{LSX4_FEX_STATUS_OK};
  struct QueuedSignal {
    Lsx4FexGuestSignalDeliveryV3 delivery{};
    Lsx4FexGuestContextV3 explicit_context{};
    bool has_explicit_context{};
    int32_t signal_code{};
    uint8_t trap_number{};
    uint8_t reserved0{};
    uint16_t error_code{};
    uint64_t fault_address{};
  };
  std::mutex signal_mutex;
  std::array<QueuedSignal, 16> signal_queue{};
  uint32_t signal_queue_read{};
  uint32_t signal_queue_write{};
  bool signal_drain_active{};
  std::atomic<bool> interrupt_page_armed{};
  std::atomic<bool> normal_interrupt_pending{};
  std::atomic<bool> deferred_pin_release_interrupt{};
  uint64_t signal_mask_low{};
  uint64_t signal_mask_high{};
  struct HostSignalCapture {
    lsx4::fex2608::SignalCaptureGate gate{};
    int32_t signum{};
    int32_t signal_code{};
    uint32_t was_in_jit{};
    uint32_t generated_guest_fault{};
    uint64_t fault_address{};
    uint64_t host_pc{};
    uint64_t host_sp{};
    uint64_t guest_rip{};
    lsx4::fex2608::HostSignalMask saved_signal_mask{};
    uint64_t synchronous_fault_data{};
  } host_signal;
  struct DeferredSignalState {
    std::atomic<uint32_t> phase{};
    lsx4::fex2608::HostMachineContext interrupted_context{};
    sigset_t interrupted_mask{};
    std::array<uint64_t, 31> working_gprs{};
    uint64_t working_pstate{};
    FEXCore::ArchHelpers::Arm64::ExactJITCodeRange exact_code_range{};
    lsx4::fex2608::SignalAtomicSite atomic_site{};
    ProductionExecutableRegistry::PinnedBlock pinned_block{};
    uint64_t guest_rip{};
    uint64_t original_host_pc{};
    uint64_t thread_stop_handler_address_spill_sra{};
  } deferred_signal;
  lsx4::fex2608::DeferredWriteEpochQueue deferred_write_epochs{};
};

using lsx4::fex2608::CanSelfLeaseCallbackScratch;
using lsx4::fex2608::DeferredLandingClaim;
using lsx4::fex2608::DeferredPhase;
using lsx4::fex2608::DeferredSignalPhase;

void ReleaseDeferredPin(ThreadRecord *record) noexcept {
  if (record == nullptr) {
    return;
  }
  g_executable_registry_store.ReleasePin(&record->deferred_signal.pinned_block);
}

bool DeferredSignalStateIsQuiescent(const ThreadRecord *record) noexcept {
  return record != nullptr &&
         record->deferred_signal.phase.load(std::memory_order_acquire) ==
             DeferredPhase(DeferredSignalPhase::Idle) &&
         !record->deferred_pin_release_interrupt.load(
             std::memory_order_acquire) &&
         !record->normal_interrupt_pending.load(std::memory_order_acquire) &&
         record->deferred_signal.pinned_block.lease.slot == nullptr;
}

bool ArmDeferredPinReleaseInterrupt(ThreadRecord *record);

enum class DeferredLandingResult : uint32_t {
  Success = 1,
  InvalidThread = 211,
  InvalidPhase = 212,
  MissingMemoryLeaseCallbacks = 213,
  MemoryLeaseRejected = 214,
  AtomicEmulationRejected = 215,
  PinReleaseInterruptRejected = 216,
  GuestFaultCaptureRejected = 217,
};

uint32_t
AbortDeferredSignalLanding(ThreadRecord *record,
                           DeferredLandingResult result =
                               static_cast<DeferredLandingResult>(0)) noexcept {
#if defined(__APPLE__)
  if (static_cast<uint32_t>(result) != 0) {
    // Error path only. Fixed stack buffer and raw write remain safe even if a
    // nested host signal causes the surrounding fail-closed cleanup.
    char buffer[768]; char* out = buffer;
    const auto text = [&](const char* value) { while (*value) *out++ = *value++; };
    const auto hex = [&](uint64_t value) {
      constexpr char digits[] = "0123456789abcdef";
      text("0x");
      for (int shift = 60; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 15];
    };
    text("[APPLE_FEX_DEFERRED_ABORT] result="); hex(static_cast<uint32_t>(result));
    if (record != nullptr) {
      const auto& state = record->deferred_signal;
      text(" phase="); hex(state.phase.load(std::memory_order_acquire));
      text(" guestRip="); hex(state.guest_rip);
      text(" hostPc="); hex(state.original_host_pc);
      text(" operand="); hex(state.atomic_site.address);
      text(" operandSize="); hex(state.atomic_site.operand_size);
      text(" helper="); hex(state.atomic_site.helper_begin);
      text(" helperSize="); hex(state.atomic_site.helper_size);
      text(" kind="); hex(static_cast<unsigned>(state.atomic_site.kind));
      text(" writes="); hex(state.atomic_site.writes);
      text(" boundTid="); hex(record->bound_tid.load(std::memory_order_acquire));
      text(" active="); hex(record->active.load(std::memory_order_acquire));
    }
    text("\n");
    (void)::write(STDERR_FILENO, buffer, static_cast<size_t>(out - buffer));
  }
#endif
  if (record != nullptr) {
    auto &deferred = record->deferred_signal;
    deferred.phase.store(DeferredPhase(DeferredSignalPhase::ResumeAbort),
                         std::memory_order_release);
    ReleaseDeferredPin(record);
    record->deferred_pin_release_interrupt.store(false,
                                                 std::memory_order_release);
  }
  return static_cast<uint32_t>(result);
}

#if defined(__aarch64__)
extern "C" void lsx4_fex2608_deferred_signal_landing();
extern "C" char lsx4_fex2608_deferred_signal_resume_trap;
#endif

constexpr uint32_t DeferredAtomicLeaseAccessFlags(const bool writes) noexcept {
  return LSX4_FEX_GUEST_MEMORY_ACCESS_READ |
         (writes ? LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE : 0) |
         LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY;
}

static_assert(DeferredAtomicLeaseAccessFlags(false) ==
              (LSX4_FEX_GUEST_MEMORY_ACCESS_READ |
               LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY));
static_assert(DeferredAtomicLeaseAccessFlags(true) ==
              (LSX4_FEX_GUEST_MEMORY_ACCESS_READ |
               LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE |
               LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY));

extern "C" uint32_t
lsx4_fex2608_process_deferred_signal(ThreadRecord *record) noexcept {
#if defined(__aarch64__)
  if (record == nullptr || record->thread == nullptr) {
    return AbortDeferredSignalLanding(record,
                                      DeferredLandingResult::InvalidThread);
  }
  auto &deferred = record->deferred_signal;
  uint32_t landing = DeferredPhase(DeferredSignalPhase::Landing);
  if (!deferred.phase.compare_exchange_strong(
          landing, DeferredPhase(DeferredSignalPhase::HelperArmed),
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    return AbortDeferredSignalLanding(record,
                                      DeferredLandingResult::InvalidPhase);
  }

  const bool record_active = record->active.load(std::memory_order_acquire);
  const uint64_t bound_tid = record->bound_tid.load(std::memory_order_acquire);
  const uint64_t current_tid = CurrentTid();
  bool self_leased_scratch = false;
  if (record_active && bound_tid == current_tid) {
    self_leased_scratch =
        record->scratch_allocation != nullptr &&
        record->scratch_allocation_size >= record->scratch_size &&
        CanSelfLeaseCallbackScratch(deferred.atomic_site.helper_begin,
                                    deferred.atomic_site.helper_size,
                                    record->scratch_base, record->scratch_size,
                                    record_active, bound_tid, current_tid);
  }

  uint64_t memory_lease{};
  const uint32_t access_flags =
      DeferredAtomicLeaseAccessFlags(deferred.atomic_site.writes);
  const uint32_t permission_flags =
      access_flags &
      (LSX4_FEX_GUEST_MEMORY_ACCESS_READ | LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE);
  if (!self_leased_scratch) {
    if (record->host_callbacks == nullptr ||
        record->host_callbacks->acquire_guest_memory_lease == nullptr ||
        record->host_callbacks->release_guest_memory_lease == nullptr) {
      return AbortDeferredSignalLanding(
          record, DeferredLandingResult::MissingMemoryLeaseCallbacks);
    }
    int32_t lease_status = record->host_callbacks->acquire_guest_memory_lease(
        record->host_callbacks->callback_context,
        deferred.atomic_site.helper_begin, deferred.atomic_site.helper_size,
        access_flags, &memory_lease);
    if (lease_status == LSX4_FEX_STATUS_INVALID_ARGUMENT && memory_lease == 0) {
      lease_status = record->host_callbacks->acquire_guest_memory_lease(
          record->host_callbacks->callback_context,
          deferred.atomic_site.helper_begin, deferred.atomic_site.helper_size,
          permission_flags, &memory_lease);
    }
    if (lease_status != LSX4_FEX_STATUS_OK || memory_lease == 0) {
      uint64_t operand_lease{};
      int32_t operand_lease_status =
          record->host_callbacks->acquire_guest_memory_lease(
              record->host_callbacks->callback_context,
              deferred.atomic_site.address, deferred.atomic_site.operand_size,
              access_flags, &operand_lease);
      if (operand_lease_status == LSX4_FEX_STATUS_INVALID_ARGUMENT &&
          operand_lease == 0) {
        operand_lease_status =
            record->host_callbacks->acquire_guest_memory_lease(
                record->host_callbacks->callback_context,
                deferred.atomic_site.address,
                deferred.atomic_site.operand_size, permission_flags,
                &operand_lease);
      }
      const bool operand_is_mapped =
          operand_lease_status == LSX4_FEX_STATUS_OK && operand_lease != 0;
      if (operand_is_mapped) {
        record->host_callbacks->release_guest_memory_lease(
            record->host_callbacks->callback_context, operand_lease);
      }
      if (record->host_callbacks->log != nullptr) {
        const uint32_t host_instruction =
            lsx4::fex2608::LoadPublishedInstruction(
                reinterpret_cast<const uint32_t *>(
                    deferred.original_host_pc));
        const uint32_t address_register = (host_instruction >> 5U) & 31U;
        const uint64_t address_register_value =
            address_register < deferred.working_gprs.size()
                ? deferred.working_gprs[address_register]
                : 0;
        char message[512]{};
        std::snprintf(
            message, sizeof(message),
            "deferred atomic lease rejected: status=%d operand=0x%" PRIxPTR
            " operandSize=%zu helper=0x%" PRIxPTR " helperSize=%zu kind=%u"
            " writes=%u flags=0x%x operandLeaseStatus=%d operandMapped=%u"
            " guestRip=0x%" PRIx64
            " hostPc=0x%" PRIx64 " instruction=0x%08" PRIx32
            " rn=%u rnValue=0x%" PRIx64,
            lease_status, deferred.atomic_site.address,
            deferred.atomic_site.operand_size,
            deferred.atomic_site.helper_begin, deferred.atomic_site.helper_size,
            static_cast<unsigned>(deferred.atomic_site.kind),
            deferred.atomic_site.writes ? 1U : 0U, access_flags,
            operand_lease_status, operand_is_mapped ? 1U : 0U,
            deferred.guest_rip, deferred.original_host_pc,
            host_instruction, address_register, address_register_value);
        record->host_callbacks->log(record->host_callbacks->callback_context, 6,
                                    "deferred-atomic-lease", message);
      }
      if (!operand_is_mapped) {
        auto *frame = record->thread->CurrentFrame;
        auto &capture = record->host_signal;
        if (frame == nullptr || frame->ReturningStackLocation == 0 ||
            deferred.thread_stop_handler_address_spill_sra == 0 ||
            lsx4::fex2608::EnterSignalHandler(&capture.gate) !=
                lsx4::fex2608::SignalHandlerEntry::Primary) {
          return AbortDeferredSignalLanding(
              record, DeferredLandingResult::GuestFaultCaptureRejected);
        }

        capture.signum = SIGSEGV;
        capture.signal_code = SEGV_MAPERR;
        capture.was_in_jit = 1;
        capture.generated_guest_fault = 0;
        capture.fault_address = deferred.atomic_site.address;
        capture.host_pc = deferred.original_host_pc;
        capture.host_sp = lsx4::fex2608::HostSp(deferred.interrupted_context);
        capture.guest_rip = deferred.guest_rip;
        capture.synchronous_fault_data = 0;
        std::memcpy(&capture.saved_signal_mask, &deferred.interrupted_mask,
                    sizeof(capture.saved_signal_mask));

        lsx4::fex2608::HostSignalMask redirected_mask = capture.saved_signal_mask;
        for (const int owned_signal :
             {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, kBridgeWakeSignal}) {
          redirected_mask |= UINT64_C(1) << (owned_signal - 1);
        }
        std::memcpy(&deferred.interrupted_mask, &redirected_mask,
                    sizeof(redirected_mask));
        lsx4::fex2608::HostSp(deferred.interrupted_context) = frame->ReturningStackLocation;
        lsx4::fex2608::HostGpr(deferred.interrupted_context, 28) =
            reinterpret_cast<uint64_t>(frame);
        lsx4::fex2608::HostPc(deferred.interrupted_context) =
            deferred.thread_stop_handler_address_spill_sra;

        lsx4::fex2608::PublishSignalCapture(&capture.gate);
        lsx4::fex2608::LeaveSignalHandler(&capture.gate);
        ReleaseDeferredPin(record);
        record->deferred_pin_release_interrupt.store(
            false, std::memory_order_release);
        deferred.phase.store(
            DeferredPhase(DeferredSignalPhase::ResumeSignalCapture),
            std::memory_order_release);
        return static_cast<uint32_t>(DeferredLandingResult::Success);
      }
      return AbortDeferredSignalLanding(
          record, DeferredLandingResult::MemoryLeaseRejected);
    }
  }

  const auto pinned_kind =
      g_executable_registry_store.PinnedKind(deferred.pinned_block);
  const auto backpatch_policy =
      pinned_kind == lsx4::fex2608::ExecutableRangeKind::SharedJIT
          ? FEXCore::ArchHelpers::Arm64::JITBackpatchPolicy::Allow
          : FEXCore::ArchHelpers::Arm64::JITBackpatchPolicy::Disallow;
  const auto result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      record->thread,
      FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier,
      deferred.original_host_pc, deferred.working_gprs.data(), true,
      &deferred.exact_code_range, backpatch_policy);
  if (!self_leased_scratch) {
    record->host_callbacks->release_guest_memory_lease(
        record->host_callbacks->callback_context, memory_lease);
  }
  if (!result.has_value()) {
    return AbortDeferredSignalLanding(
        record, DeferredLandingResult::AtomicEmulationRejected);
  }
  // HandleUnalignedAccess models NZCV updates in the supplied host GPR image
  // when it recognizes a multi-instruction CAS sequence. The retained MSR in
  // the original instruction stream publishes that value after resumption.
  for (size_t i = 0; i < 31; ++i) lsx4::fex2608::HostGpr(deferred.interrupted_context, i) = deferred.working_gprs[i];
  lsx4::fex2608::HostPstate(deferred.interrupted_context) = deferred.working_pstate;
  lsx4::fex2608::HostPc(deferred.interrupted_context) = deferred.original_host_pc + *result;
  deferred.phase.store(DeferredPhase(DeferredSignalPhase::ResumeOriginal),
                       std::memory_order_release);
  const bool fex_owns_interrupted_pc =
      record->thread->CTX != nullptr &&
      record->thread->CTX->IsAddressInCodeBuffer(
          record->thread, deferred.original_host_pc);
  if (lsx4::fex2608::CanReleaseSharedJitPinBeforeResume(
          pinned_kind == lsx4::fex2608::ExecutableRangeKind::SharedJIT,
          fex_owns_interrupted_pc)) {
    // The helper has finished every read and possible backpatch of this block.
    // This guest thread cannot replace its FEX CodeBuffer owner between here
    // and sigreturn, and any later replacement occurs only after leaving this
    // generated block. Avoid a second synthetic interrupt, which would
    // otherwise restart guest execution at a metadata approximation.
    ReleaseDeferredPin(record);
    record->deferred_pin_release_interrupt.store(false,
                                                 std::memory_order_release);
    return static_cast<uint32_t>(DeferredLandingResult::Success);
  }
  if (!ArmDeferredPinReleaseInterrupt(record)) {
    return AbortDeferredSignalLanding(
        record, DeferredLandingResult::PinReleaseInterruptRejected);
  }
  return static_cast<uint32_t>(DeferredLandingResult::Success);
#else
  (void)record;
  return 0;
#endif
}

class ScopedPrimarySignalHandler final {
public:
  explicit ScopedPrimarySignalHandler(
      lsx4::fex2608::SignalCaptureGate *gate) noexcept
      : gate_{gate} {}

  ~ScopedPrimarySignalHandler() {
    if (!published_) {
      lsx4::fex2608::CancelSignalCapture(gate_);
    }
    lsx4::fex2608::LeaveSignalHandler(gate_);
  }

  void Publish() noexcept {
    lsx4::fex2608::PublishSignalCapture(gate_);
    published_ = true;
  }

  ScopedPrimarySignalHandler(const ScopedPrimarySignalHandler &) = delete;
  ScopedPrimarySignalHandler &
  operator=(const ScopedPrimarySignalHandler &) = delete;

private:
  lsx4::fex2608::SignalCaptureGate *gate_{};
  bool published_{};
};

enum class FailClosedExit : int32_t {
  NonQuiescentReturn = 201,
  FaultInsideDeferredHelper = 202,
  InvalidDeferredResumeTrap = 203,
  SignalCaptureReentry = 204,
  ConcurrentBusLanding = 205,
};

[[noreturn]] void TerminateOnNestedHostFault(int32_t signum,
                                             FailClosedExit reason) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  const char *marker{};
  size_t marker_size{};
  switch (reason) {
  case FailClosedExit::NonQuiescentReturn: {
    static constexpr char text[] =
        "LSX4_FEX_FAIL_CLOSED reason=non_quiescent_return\n";
    marker = text;
    marker_size = sizeof(text) - 1;
    break;
  }
  case FailClosedExit::FaultInsideDeferredHelper: {
    static constexpr char text[] =
        "LSX4_FEX_FAIL_CLOSED reason=fault_inside_deferred_helper\n";
    marker = text;
    marker_size = sizeof(text) - 1;
    break;
  }
  case FailClosedExit::InvalidDeferredResumeTrap: {
    static constexpr char text[] =
        "LSX4_FEX_FAIL_CLOSED reason=invalid_deferred_resume_trap\n";
    marker = text;
    marker_size = sizeof(text) - 1;
    break;
  }
  case FailClosedExit::SignalCaptureReentry: {
    static constexpr char text[] =
        "LSX4_FEX_FAIL_CLOSED reason=signal_capture_reentry\n";
    marker = text;
    marker_size = sizeof(text) - 1;
    break;
  }
  case FailClosedExit::ConcurrentBusLanding: {
    static constexpr char text[] =
        "LSX4_FEX_FAIL_CLOSED reason=concurrent_bus_landing\n";
    marker = text;
    marker_size = sizeof(text) - 1;
    break;
  }
  }
  if (marker != nullptr) {
    (void)::write(STDERR_FILENO, marker, marker_size);
  }
  (void)signum;
  ::_exit(static_cast<int32_t>(reason));
#endif
  __builtin_trap();
}

#if 0
class ExecutableRegistryReader {
 public:
  ExecutableRegistryReader() noexcept {
    // SC handshake with BeginRetire: in the single SC order, either this
    // increment precedes Live->Retiring (the retirer must wait for us), or the
    // subsequent SC state read observes Retiring. An acquire-only state read
    // would not establish that two-case proof.
    g_executable_registry_readers.fetch_add(1, std::memory_order_seq_cst);
    if (g_executable_registry_compacting.load(std::memory_order_seq_cst)) {
      g_executable_registry_readers.fetch_sub(1,
                                              std::memory_order_seq_cst);
      active_ = false;
    }
  }
  ~ExecutableRegistryReader() {
    if (active_) {
      g_executable_registry_readers.fetch_sub(1, std::memory_order_seq_cst);
    }
  }
  bool active() const noexcept { return active_; }
  ExecutableRegistryReader(const ExecutableRegistryReader&) = delete;
  ExecutableRegistryReader& operator=(const ExecutableRegistryReader&) = delete;

 private:
  bool active_{true};
};

struct ExecutableRangeLease {
  const ExecutableRegistrySlot* slot{};
  uint64_t generation{};
};

bool PublishedInterval(const ExecutableRangeLease& lease, uintptr_t address,
                       size_t size) noexcept {
  uintptr_t end{};
  const auto* slot = lease.slot;
  if (slot == nullptr || !CheckedRange(address, size, &end) ||
      slot->state.load(std::memory_order_seq_cst) !=
          static_cast<uint32_t>(ExecutableSlotState::Live) ||
      slot->generation != lease.generation || address < slot->base ||
      end > slot->base + slot->size) {
    return false;
  }
  if (slot->kind == FEXCore::ExecutableCode::RangeKind::SharedJIT) {
    return end <=
           slot->shared_published_end.load(std::memory_order_acquire);
  }
  const size_t first = (address - slot->base) / kFexCodeGranule;
  const size_t last = (end - 1 - slot->base) / kFexCodeGranule;
  for (size_t page = first; page <= last; ++page) {
    if ((slot->published_pages[page / 64].load(std::memory_order_acquire) &
         (UINT64_C(1) << (page % 64))) == 0) {
      return false;
    }
  }
  return true;
}

struct PublishedExecutableBlock {
  uintptr_t begin{};
  uintptr_t end{};
  ExecutableRangeLease lease{};
};

std::optional<PublishedExecutableBlock>
FindPublishedExecutableBlock(uintptr_t host_pc) noexcept {
  uint32_t reference =
      g_executable_block_buckets[ExecutableBlockBucket(host_pc)].load(
          std::memory_order_acquire);
  size_t traversed{};
  while (reference != 0 &&
         traversed++ < kMaximumExecutableBlockBucketDepth) {
    const size_t index = reference - 1;
    if (index >= kExecutableBlockSlotCount) {
      return std::nullopt;
    }
    const auto& block = g_executable_blocks[index];
    if (host_pc >= block.begin && host_pc < block.end) {
      if (block.mapping_slot >= g_executable_registry.size()) {
        return std::nullopt;
      }
      const auto& mapping = g_executable_registry[block.mapping_slot];
      if (mapping.state.load(std::memory_order_seq_cst) ==
              static_cast<uint32_t>(ExecutableSlotState::Live) &&
          mapping.generation == block.mapping_generation) {
        ExecutableRangeLease lease{&mapping, mapping.generation};
        if (PublishedInterval(lease, block.begin, block.end - block.begin)) {
          return PublishedExecutableBlock{block.begin, block.end, lease};
        }
      }
    }
    reference = block.next;
  }
  return std::nullopt;
}
#endif

class ExecutableRegistryReader {
public:
  ExecutableRegistryReader() noexcept : reader_{g_executable_registry_store} {}
  bool active() const noexcept { return reader_.active(); }
  const ProductionExecutableRegistry::Reader &native() const noexcept {
    return reader_;
  }
  ExecutableRegistryReader(const ExecutableRegistryReader &) = delete;
  ExecutableRegistryReader &
  operator=(const ExecutableRegistryReader &) = delete;

private:
  ProductionExecutableRegistry::Reader reader_;
};

using PublishedExecutableBlock = ProductionExecutableRegistry::PublishedBlock;

std::optional<PublishedExecutableBlock>
FindPublishedExecutableBlock(const ExecutableRegistryReader &reader,
                             uintptr_t host_pc) noexcept {
  return g_executable_registry_store.Find(reader.native(), host_pc);
}

std::optional<uint64_t> DecodeCurrentJitGuestRip(
    const ThreadRecord *record, const ucontext_t *context, uint64_t host_pc,
    const PublishedExecutableBlock &block,
    FEXCore::ArchHelpers::Arm64::ExactJITCodeRange *exact_code_range,
    bool permit_busy_spin_lock = false) noexcept {
#if defined(__aarch64__)
  if (record == nullptr || record->thread == nullptr || context == nullptr) {
    return std::nullopt;
  }
  const auto *frame = record->thread->CurrentFrame;
  if (frame == nullptr ||
      lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 28) != reinterpret_cast<uint64_t>(frame)) {
    return std::nullopt;
  }

  if (exact_code_range == nullptr || host_pc < block.begin ||
      host_pc >= block.end) {
    return std::nullopt;
  }
  using Backend = FEXCore::CPU::CPUBackend;
  static_assert(sizeof(Backend::JITCodeHeader) ==
                sizeof(lsx4::fex2608::JitCodeHeaderV1));
  static_assert(sizeof(Backend::JITCodeTail) ==
                sizeof(lsx4::fex2608::JitCodeTailV1));
  static_assert(alignof(Backend::JITCodeTail) ==
                alignof(lsx4::fex2608::JitCodeTailV1));
  static_assert(Backend::JITCodeTail::METADATA_VERSION == 1);
  const auto decoded = lsx4::fex2608::DecodeJitMetadataV1(
      block.begin, block.end, host_pc, permit_busy_spin_lock);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  *exact_code_range = {
      .BlockBegin = decoded->block_begin,
      .BlockEnd = decoded->block_end,
      .InlineTail = decoded->inline_tail,
  };
  return decoded->guest_rip;
#else
  (void)record;
  (void)context;
  (void)host_pc;
  (void)block;
  (void)exact_code_range;
  (void)permit_busy_spin_lock;
  return std::nullopt;
#endif
}

struct RetireVsBusTestState {
  void *base{};
  size_t size{};
  std::atomic<bool> entered{};
  std::atomic<bool> completed{};
  bool retired{};
};

void *RetireVsBusTestThread(void *opaque) {
  auto *state = static_cast<RetireVsBusTestState *>(opaque);
  state->entered.store(true, std::memory_order_release);
  state->retired = BeginRetireExecutableRange(
      nullptr, FEXCore::ExecutableCode::RangeKind::SharedJIT, state->base,
      state->size);
  state->completed.store(true, std::memory_order_release);
  return nullptr;
}

bool RunNoBackpatchDecoderAdversarialTests(ThreadRecord *record) {
#if defined(__aarch64__)
  using Backend = FEXCore::CPU::CPUBackend;
  auto fail = [](const char *stage, size_t index = 0) {
    std::fprintf(stderr, "FEX2608_DECODER FAIL %s index=%zu\n", stage, index);
    return false;
  };
  if (record == nullptr || record->thread == nullptr) {
    return fail("thread");
  }
  const long queried_page_size = ::sysconf(_SC_PAGESIZE);
  if (queried_page_size <= 0) {
    return fail("page-size");
  }
  const size_t page_size = static_cast<size_t>(queried_page_size);
  void *allocation = FEXCore::Allocator::VirtualAlloc(page_size, true);
  if (allocation == nullptr || allocation == MAP_FAILED) {
    return fail("allocate");
  }
  const uintptr_t block_begin = reinterpret_cast<uintptr_t>(allocation);
  const uintptr_t write_begin = reinterpret_cast<uintptr_t>(lsx4::fex2608::HostWritableCode(allocation, page_size));
  constexpr size_t instruction_offset = 2 * sizeof(uint32_t);
  constexpr uint32_t kMRSNZCVX0 = UINT32_C(0xd53b4200);
  constexpr uint32_t kNOP = UINT32_C(0xd503201f);
  constexpr uint32_t kLDARX0X1 = UINT32_C(0xc8dffc20);
  constexpr std::array<uint32_t, 13> kCASPair32{
      UINT32_C(0x887f8921), UINT32_C(0x6b07003f), UINT32_C(0x7a480040),
      UINT32_C(0x540000c1), UINT32_C(0x8821ad2a), UINT32_C(0x35ffff61),
      UINT32_C(0x2a0703ee), UINT32_C(0x2a0803ef), UINT32_C(0x14000004),
      UINT32_C(0x2a0103ee), UINT32_C(0x2a0203ef), UINT32_C(0xd5033f5f),
      UINT32_C(0xd51b4200)};
  constexpr std::array<uint32_t, 13> kCASPair64{
      UINT32_C(0xc87f8921), UINT32_C(0xeb07003f), UINT32_C(0xfa480040),
      UINT32_C(0x540000c1), UINT32_C(0xc821ad2a), UINT32_C(0xb5ffff61),
      UINT32_C(0xaa0703ee), UINT32_C(0xaa0803ef), UINT32_C(0x14000004),
      UINT32_C(0xaa0103ee), UINT32_C(0xaa0203ef), UINT32_C(0xd5033f5f),
      UINT32_C(0xd51b4200)};

  auto cleanup = [&] {
    FEXCore::Allocator::VirtualFree(allocation, page_size);
  };
  auto prepare = [&](const uint32_t *sequence, size_t instruction_count) {
    std::memset(reinterpret_cast<void*>(write_begin), 0, page_size);
    auto *instructions =
        reinterpret_cast<uint32_t *>(write_begin + instruction_offset);
    instructions[-1] = kMRSNZCVX0;
    if (instruction_count != 0) {
      std::memcpy(instructions, sequence, instruction_count * sizeof(uint32_t));
    }
    const size_t tail_offset =
        (instruction_offset + instruction_count * sizeof(uint32_t) +
         alignof(Backend::JITCodeTail) - 1) &
        ~(alignof(Backend::JITCodeTail) - 1);
    auto *header = reinterpret_cast<Backend::JITCodeHeader *>(write_begin);
    header->OffsetToBlockTail = static_cast<uint32_t>(tail_offset);
    auto *tail =
        reinterpret_cast<Backend::JITCodeTail *>(write_begin + tail_offset);
    *tail = {};
    const size_t block_size =
        (tail_offset + sizeof(Backend::JITCodeTail) + 3) & ~size_t{3};
    tail->Size = block_size;
    tail->MetadataVersion = Backend::JITCodeTail::METADATA_VERSION;
    return FEXCore::ArchHelpers::Arm64::ExactJITCodeRange{
        block_begin, block_begin + block_size, block_begin + tail_offset};
  };
  auto invoke = [&](const FEXCore::ArchHelpers::Arm64::ExactJITCodeRange *exact,
                    std::array<uint64_t, 31> *gprs,
                    size_t program_counter_offset = instruction_offset) {
    return FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
        record->thread,
        FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier,
        block_begin + program_counter_offset, gprs->data(), true, exact,
        FEXCore::ArchHelpers::Arm64::JITBackpatchPolicy::Disallow);
  };

  // Disallow is never permitted to turn a missing exact lease/range into an
  // unbounded signal-time scan.
  {
    const std::array<uint32_t, 2> code{kLDARX0X1, kNOP};
    const auto exact = prepare(code.data(), code.size());
    std::array<uint64_t, 31> gprs{};
    std::array<uint8_t, 16> source{};
    gprs[1] = reinterpret_cast<uint64_t>(source.data());
    const auto before = code;
    if (invoke(nullptr, &gprs).has_value() ||
        std::memcmp(reinterpret_cast<void *>(block_begin + instruction_offset),
                    before.data(), sizeof(before)) != 0) {
      cleanup();
      return fail("missing-exact");
    }
    (void)exact;
  }

  // Place the faulting instruction immediately before the metadata tail. Its
  // +4 landing is not executable code and must fail before touching memory or
  // code.
  {
    const std::array<uint32_t, 2> code{kNOP, kLDARX0X1};
    const auto exact = prepare(code.data(), code.size());
    std::array<uint64_t, 31> gprs{};
    std::array<uint8_t, 16> source{};
    gprs[1] = reinterpret_cast<uint64_t>(source.data());
    if (invoke(&exact, &gprs, instruction_offset + sizeof(uint32_t))
            .has_value() ||
        *reinterpret_cast<uint32_t *>(block_begin + instruction_offset +
                                      sizeof(uint32_t)) != kLDARX0X1) {
      cleanup();
      return fail("landing-tail");
    }
  }

  auto test_pair = [&](const std::array<uint32_t, 13> &code, bool is_64_bit) {
    const auto exact = prepare(code.data(), code.size());
    std::array<uint64_t, 31> gprs{};
    alignas(16) std::array<uint8_t, 48> source{};
    const uintptr_t address = reinterpret_cast<uintptr_t>(source.data() + 1);
    gprs[9] = address;
    if (is_64_bit) {
      gprs[7] = UINT64_C(0x1122334455667788);
      gprs[8] = UINT64_C(0x99aabbccddeeff00);
      gprs[10] = UINT64_C(0xfedcba9876543210);
      gprs[11] = UINT64_C(0x0123456789abcdef);
      std::memcpy(reinterpret_cast<void *>(address), &gprs[7],
                  sizeof(uint64_t));
      std::memcpy(reinterpret_cast<void *>(address + sizeof(uint64_t)),
                  &gprs[8], sizeof(uint64_t));
    } else {
      const uint32_t expected_low = UINT32_C(0x11223344);
      const uint32_t expected_high = UINT32_C(0x99aabbcc);
      gprs[7] = expected_low;
      gprs[8] = expected_high;
      gprs[10] = UINT32_C(0xfedcba98);
      gprs[11] = UINT32_C(0x01234567);
      std::memcpy(reinterpret_cast<void *>(address), &expected_low,
                  sizeof(expected_low));
      std::memcpy(reinterpret_cast<void *>(address + sizeof(expected_low)),
                  &expected_high, sizeof(expected_high));
    }
    const auto before = code;
    const auto handled = invoke(&exact, &gprs);
    if (!handled.has_value() || *handled != 9 * sizeof(uint32_t) ||
        std::memcmp(reinterpret_cast<void *>(block_begin + instruction_offset),
                    before.data(), sizeof(before)) != 0) {
      return false;
    }
    if (is_64_bit) {
      uint64_t actual_low{};
      uint64_t actual_high{};
      std::memcpy(&actual_low, reinterpret_cast<void *>(address),
                  sizeof(actual_low));
      std::memcpy(&actual_high,
                  reinterpret_cast<void *>(address + sizeof(actual_low)),
                  sizeof(actual_high));
      return actual_low == gprs[10] && actual_high == gprs[11];
    }
    uint32_t actual_low{};
    uint32_t actual_high{};
    std::memcpy(&actual_low, reinterpret_cast<void *>(address),
                sizeof(actual_low));
    std::memcpy(&actual_high,
                reinterpret_cast<void *>(address + sizeof(actual_low)),
                sizeof(actual_high));
    return actual_low == static_cast<uint32_t>(gprs[10]) &&
           actual_high == static_cast<uint32_t>(gprs[11]);
  };
  if (!test_pair(kCASPair32, false) || !test_pair(kCASPair64, true)) {
    cleanup();
    return fail("valid-pair");
  }

  // Every opcode is part of the contract. Replacing any one with NOP, or
  // truncating before the final NZCV restore, must reject without code writes.
  for (size_t malformed = 0; malformed < kCASPair32.size(); ++malformed) {
    auto code = kCASPair32;
    code[malformed] = kNOP;
    const auto exact = prepare(code.data(), code.size());
    std::array<uint64_t, 31> gprs{};
    alignas(16) std::array<uint8_t, 32> source{};
    gprs[9] = reinterpret_cast<uint64_t>(source.data() + 1);
    const auto before = code;
    if (invoke(&exact, &gprs).has_value() ||
        std::memcmp(reinterpret_cast<void *>(block_begin + instruction_offset),
                    before.data(), sizeof(before)) != 0) {
      cleanup();
      return fail("malformed", malformed);
    }
  }
  for (size_t truncated = 1; truncated < kCASPair32.size(); ++truncated) {
    const auto exact = prepare(kCASPair32.data(), truncated);
    std::array<uint64_t, 31> gprs{};
    alignas(16) std::array<uint8_t, 32> source{};
    gprs[9] = reinterpret_cast<uint64_t>(source.data() + 1);
    std::array<uint32_t, 13> before{};
    std::memcpy(before.data(),
                reinterpret_cast<void *>(block_begin + instruction_offset),
                truncated * sizeof(uint32_t));
    if (invoke(&exact, &gprs).has_value() ||
        std::memcmp(reinterpret_cast<void *>(block_begin + instruction_offset),
                    before.data(), truncated * sizeof(uint32_t)) != 0) {
      cleanup();
      return fail("truncated", truncated);
    }
  }

  cleanup();
  return true;
#else
  (void)record;
  return true;
#endif
}

// Enabled only by the Android smoke harness. This exercises the actual
// registry, decoder and FEX non-mutating alignment helper while retirement
// concurrently changes Live->Retiring. A successful test proves the reader
// lease remains
// held across all exact-block instruction reads and atomic emulation; the code
// bytes must remain unchanged because concurrent multi-instruction SMC is not
// an Arm-safe production contract.
bool RunRetireVsBusAdversarialTest(ThreadRecord *record) {
#if defined(__aarch64__)
  auto FailTest = [](const char *stage) {
    std::fprintf(stderr, "FEX2608_RETIRE_BUS FAIL %s errno=%d\n", stage, errno);
    return false;
  };
  if (record == nullptr || record->thread == nullptr ||
      record->thread->CurrentFrame == nullptr) {
    return FailTest("thread");
  }
  if (!RunNoBackpatchDecoderAdversarialTests(record)) {
    return FailTest("decoder");
  }
  const long queried_page_size = ::sysconf(_SC_PAGESIZE);
  if (queried_page_size <= 0 ||
      (queried_page_size & (queried_page_size - 1)) != 0) {
    return FailTest("page-size");
  }
  const size_t page_size = static_cast<size_t>(queried_page_size);
  // The registry contract intentionally caps a range below one code-buffer
  // guard page; make the synthetic reservation two host pages so its usable
  // portion can contain this exact block just like production SharedJIT.
  const size_t allocation_size = page_size * 2;
  void *allocation = FEXCore::Allocator::VirtualAlloc(allocation_size, true);
  if (allocation == nullptr || allocation == MAP_FAILED) {
    return FailTest("allocate");
  }
  bool reserved{};
  bool retired{};
  auto cleanup = [&] {
    if (!reserved) {
      FEXCore::Allocator::VirtualFree(allocation, allocation_size);
      return;
    }
    if (!retired) {
      retired = BeginRetireExecutableRange(
          nullptr, FEXCore::ExecutableCode::RangeKind::SharedJIT, allocation,
          allocation_size);
    }
    if (retired) {
      const bool unmapped =
          FEXCore::Allocator::munmap(allocation, allocation_size) == 0;
      FinishRetireExecutableRange(nullptr,
                                  FEXCore::ExecutableCode::RangeKind::SharedJIT,
                                  allocation, allocation_size, unmapped);
    }
  };

  if (!ReserveExecutableRange(nullptr,
                              FEXCore::ExecutableCode::RangeKind::SharedJIT,
                              allocation, allocation_size, page_size)) {
    cleanup();
    return FailTest("reserve");
  }
  reserved = true;

  using Backend = FEXCore::CPU::CPUBackend;
  const uintptr_t block_begin = reinterpret_cast<uintptr_t>(allocation);
  const uintptr_t write_begin = reinterpret_cast<uintptr_t>(lsx4::fex2608::HostWritableCode(allocation, page_size));
  constexpr size_t instruction_offset = sizeof(Backend::JITCodeHeader);
  constexpr size_t instruction_bytes = 2 * sizeof(uint32_t);
  const size_t tail_offset = (instruction_offset + instruction_bytes +
                              alignof(Backend::JITCodeTail) - 1) &
                             ~(alignof(Backend::JITCodeTail) - 1);
  const size_t table_offset = tail_offset + sizeof(Backend::JITCodeTail);
  const size_t block_size = (table_offset + 1 + 3) & ~size_t{3};
  if (block_size > page_size || instruction_offset > 64 ||
      (instruction_offset & 3) != 0) {
    cleanup();
    return FailTest("layout");
  }

  auto *header = reinterpret_cast<Backend::JITCodeHeader *>(write_begin);
  header->OffsetToBlockTail = static_cast<uint32_t>(tail_offset);
  auto *instructions =
      reinterpret_cast<uint32_t *>(write_begin + instruction_offset);
  constexpr uint32_t kTestLDAR =
      UINT32_C(0x08dffc00) | (UINT32_C(3) << 30) | (UINT32_C(1) << 5);
  constexpr uint32_t kNop = UINT32_C(0xd503201f);
  instructions[0] = kTestLDAR;
  instructions[1] = kNop;
  auto *tail =
      reinterpret_cast<Backend::JITCodeTail *>(write_begin + tail_offset);
  *tail = {};
  tail->Size = block_size;
  tail->RIP = UINT64_C(0x100000);
  tail->GuestSize = 1;
  tail->NumberOfRIPEntries = 1;
  tail->OffsetToRIPEntries = sizeof(Backend::JITCodeTail);
  tail->RIPTableSize = 1;
  tail->MetadataVersion = Backend::JITCodeTail::METADATA_VERSION;
  // One-byte vl64 pair: host += 4, guest += 1.
  *reinterpret_cast<uint8_t *>(write_begin + table_offset) = 0;

  if (!PublishExecutableRange(nullptr,
                              FEXCore::ExecutableCode::RangeKind::SharedJIT,
                              allocation, block_size) ||
      !PublishExecutableBlock(nullptr,
                              FEXCore::ExecutableCode::RangeKind::SharedJIT,
                              allocation, block_size)) {
    cleanup();
    return FailTest("publish");
  }

  RetireVsBusTestState retire_state{allocation, allocation_size};
  pthread_t retire_thread{};
  bool thread_started{};
  bool test_passed{};
  bool retire_waiting_observed{};
  std::optional<ProductionExecutableRegistry::PinnedBlock> deferred_pin;
  {
    ExecutableRegistryReader reader;
    const uintptr_t host_pc = block_begin + instruction_offset;
    const auto block = reader.active()
                           ? FindPublishedExecutableBlock(reader, host_pc)
                           : std::nullopt;
    ucontext_t context{};
    lsx4::fex2608::HostMachineContext context_storage{};
    lsx4::fex2608::InitializeHostContext(context, context_storage);
    lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(&context), 28) =
        reinterpret_cast<uint64_t>(record->thread->CurrentFrame);
    lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(&context)) = host_pc;
    FEXCore::ArchHelpers::Arm64::ExactJITCodeRange exact{};
    const auto guest_rip =
        block.has_value() ? DecodeCurrentJitGuestRip(record, &context, host_pc,
                                                     *block, &exact)
                          : std::nullopt;
    if (!guest_rip.has_value() || *guest_rip != tail->RIP + 1 ||
        !(deferred_pin =
              g_executable_registry_store.TryPin(reader.native(), *block))
             .has_value() ||
        pthread_create(&retire_thread, nullptr, RetireVsBusTestThread,
                       &retire_state) != 0) {
      if (deferred_pin.has_value()) {
        g_executable_registry_store.ReleasePin(&*deferred_pin);
      }
      cleanup();
      return FailTest("decode-or-create");
    }
    thread_started = true;

    for (size_t spin = 0;
         spin < 100000 && !retire_state.entered.load(std::memory_order_acquire);
         ++spin) {
      ::sched_yield();
    }
    for (size_t spin = 0; spin < 256; ++spin) {
      ::sched_yield();
    }
    retire_waiting_observed =
        retire_state.entered.load(std::memory_order_acquire) &&
        !retire_state.completed.load(std::memory_order_acquire);
    test_passed = retire_waiting_observed && instructions[0] == kTestLDAR &&
                  instructions[1] == kNop;
  }

  if (deferred_pin.has_value()) {
    g_executable_registry_store.ReleasePin(&*deferred_pin);
  }

  if (thread_started) {
    if (pthread_join(retire_thread, nullptr) != 0) {
      cleanup();
      return FailTest("join");
    }
    retired = retire_state.retired;
  }
  cleanup();
  if (!test_passed || !retired) {
    std::fprintf(stderr,
                 "FEX2608_RETIRE_BUS detail passed=%d retired=%d ins0=%08x "
                 "ins1=%08x waiting=%d\n",
                 test_passed, retired, instructions[0], instructions[1],
                 retire_waiting_observed);
    return FailTest(test_passed ? "retire" : "lease");
  }
  return true;
#else
  (void)record;
  return true;
#endif
}

class BridgeSignalDelegator final : public FEXCore::SignalDelegator {
public:
  explicit BridgeSignalDelegator(uintptr_t callback_return)
      : callback_return_{callback_return} {}

  uintptr_t GetThunkCallbackRET() const override { return callback_return_; }

private:
  uintptr_t callback_return_{};
};

class BridgeSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  explicit BridgeSyscallHandler(Runtime *runtime) : runtime_{runtime} {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame *frame,
                         FEXCore::HLE::SyscallArguments *) override;
  FEXCore::HLE::ExecutableRangeInfo
  QueryGuestExecutableRange(FEXCore::Core::InternalThreadState *,
                            uint64_t address) override;
  std::optional<FEXCore::ExecutableFileSectionInfo>
  LookupExecutableFileSection(FEXCore::Core::InternalThreadState *,
                              uint64_t) override {
    return std::nullopt;
  }

private:
  Runtime *runtime_{};
};

class BridgeThunkHandler final : public FEXCore::ThunkHandler {
public:
  explicit BridgeThunkHandler(Runtime *runtime) : runtime_{runtime} {}
  FEXCore::ThunkedFunction *
  LookupThunk(const FEXCore::IR::SHA256Sum &sha256) override;
  FEXCore::ContextThunkedFunction *
  LookupContextThunk(const FEXCore::IR::SHA256Sum &sha256) override;
  bool IsTypedTimeThunkIdentity(
      const FEXCore::IR::SHA256Sum &sha256) const;

private:
  Runtime *runtime_{};
};

struct Runtime {
  FEXCore::Context::Context *context{};
  BridgeSignalDelegator *signal_delegator{};
  BridgeSyscallHandler *syscall_handler{};
  BridgeThunkHandler *thunk_handler{};
  Lsx4FexHostCallbacksV3 callbacks{};
  std::array<uint8_t, kThunkIdentitySize> thunk_identity{};
  Lsx4FexGuestCpuProfileV6 guest_cpu_profile{};
  Lsx4FexCpuTuningV1 cpu_tuning{};
  std::array<char, kGuestCpuProfileIdCapacity> guest_cpu_profile_id{};
  bool exact_guest_cpu_profile{};
  bool exact_store_watch{};
  bool guest_store_trace{};
  bool guest_write_op_coverage{};
  uint32_t proof_metrics_mode{};
  uint64_t proof_metrics_epoch{};
  uint32_t page_size{};
  uint32_t strong_memory_mode{};
  uint32_t create_flags{};
  std::string runtime_root;
  std::string cache_namespace;
  std::string host_build_id;
  PersistentRipSeedProfile persistent_rip_seed;
  std::atomic<bool> persistent_rip_seed_prewarming{};
  std::atomic<bool> persistent_rip_seed_prewarm_done{};
  uint64_t runtime_features{kBaseRuntimeFeatures};
  uint64_t diagnostics{kRuntimeDiagnostics};
  void *sentinel_allocation{};
  uint64_t exit_sentinel{};
  uint64_t callback_return_sentinel{};
  uint64_t abort_stack_pointer{};
  std::mutex range_update_mutex;
  std::atomic<const ExecutableRangeStore *> executable_ranges{};
  void *range_context{};
  Lsx4Fex2608SnapshotExecutableRangesFnV2 snapshot_ranges{};
  std::mutex records_mutex;
  std::vector<ThreadRecord *> records;
  std::atomic<uint64_t> active_threads{};
  std::atomic<uint64_t> next_call_id{1};
  std::atomic<uint64_t> hardware_tso_bind_count{};
  std::atomic<bool> hardware_tso_bind_failed{};
  std::atomic<const lsx4::fex2608::ResumeHleFramePools *> hle_frame_pools{};
  std::atomic<uint32_t> callback_diagnostic_sequence{};
  std::atomic<uint64_t> signal_capture_count{};
  std::atomic<uint64_t> signal_interrupt_capture_count{};
  std::atomic<uint64_t> signal_state_rip_mismatch_count{};
  lsx4::fex2608::BusAdralnRejectTelemetry bus_adraln_rejects{};
  std::atomic<uint64_t> bus_adraln_busy_metadata_recoveries{};
  int bus_adraln_reject_fd{-1};
  Lsx4Fex2608DeferredWriteEpochCallbacksV1 deferred_write_epoch_callbacks{};
  std::atomic<uint64_t> deferred_write_epoch_armed{};
  std::atomic<uint64_t> deferred_write_epoch_promoted{};
  std::atomic<uint64_t> deferred_write_epoch_overflow_fallbacks{};
  std::atomic<uint64_t> deferred_write_epoch_unavailable_fallbacks{};
  std::atomic<uint64_t> deferred_write_epoch_interrupt_arm_failures{};
  std::mutex signal_actions_mutex;
  std::array<Lsx4FexGuestSignalActionV3, 130> signal_actions{};
  std::array<bool, 130> signal_action_valid{};
};

void ObserveCompiledBlockForRipSeed(void *opaque, uint64_t guest_rip,
                                    uint64_t guest_length) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr ||
      runtime->persistent_rip_seed_prewarming.load(std::memory_order_relaxed)) {
    return;
  }
  const auto *ranges =
      runtime->executable_ranges.load(std::memory_order_acquire);
  runtime->persistent_rip_seed.Record(ranges, guest_rip, guest_length);
}

void WritePersistentRipSeedProof(Runtime *runtime, size_t loaded,
                                 size_t attempted, uint64_t compiled,
                                 uint64_t elapsed_us, bool budget_hit) {
  if (runtime == nullptr || runtime->runtime_root.empty()) {
    return;
  }
  char proof[512]{};
  const int written = std::snprintf(
      proof, sizeof(proof),
      "schema=lsx4.fex-persistent-rip-seed.v1\n"
      "nativeCodePersisted=0\ncurrentGuestBytesRecompiled=1\n"
      "loaded=%zu\nattempted=%zu\ncompiled=%" PRIu64
      "\nelapsedUs=%" PRIu64 "\nbudgetHit=%d\nprofile=%s\n",
      loaded, attempted, compiled, elapsed_us, budget_hit ? 1 : 0,
      runtime->persistent_rip_seed.path().c_str());
  if (written <= 0) {
    return;
  }
  const std::string proof_path =
      runtime->runtime_root + "/fex-rip-seed-proof.txt";
  const int fd = ::open(proof_path.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                        0600);
  if (fd >= 0) {
    const size_t size =
        std::min(static_cast<size_t>(written), sizeof(proof) - 1);
    (void)::write(fd, proof, size);
    (void)::close(fd);
  }
}

void PrewarmPersistentRipSeeds(Runtime *runtime, ThreadRecord *record) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      !runtime->persistent_rip_seed.ready() ||
      runtime->persistent_rip_seed_prewarm_done.exchange(
          true, std::memory_order_acq_rel)) {
    return;
  }
  const auto *ranges =
      runtime->executable_ranges.load(std::memory_order_acquire);
  const auto seeds = runtime->persistent_rip_seed.SnapshotRecent(
      ranges, kPersistentRipSeedPrewarmLimit);
  auto *context_impl =
      static_cast<FEXCore::Context::ContextImpl *>(runtime->context);
  const uint64_t compilations_before =
      context_impl->SnapshotJITTelemetry().Compilations;
  const auto begin = std::chrono::steady_clock::now();
  size_t attempted{};
  bool budget_hit{};
  runtime->persistent_rip_seed_prewarming.store(true,
                                                 std::memory_order_release);
  for (const uint64_t rip : seeds) {
    runtime->context->CompileRIP(record->thread, rip);
    ++attempted;
    if (std::chrono::steady_clock::now() - begin >=
        kPersistentRipSeedPrewarmBudget) {
      budget_hit = attempted < seeds.size();
      break;
    }
  }
  runtime->persistent_rip_seed_prewarming.store(false,
                                                 std::memory_order_release);
  const uint64_t compilations_after =
      context_impl->SnapshotJITTelemetry().Compilations;
  const uint64_t elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin)
          .count();
  const uint64_t compiled = compilations_after >= compilations_before
                                ? compilations_after - compilations_before
                                : 0;
#ifdef __ANDROID__
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_PERSISTENT_RIP_SEED_PROOF] loaded=%zu attempted=%zu "
      "compiled=%" PRIu64 " elapsedUs=%" PRIu64
      " budgetHit=%d nativeCodePersisted=0 currentGuestBytesRecompiled=1",
      seeds.size(), attempted, compiled, elapsed_us, budget_hit ? 1 : 0);
#endif
  WritePersistentRipSeedProof(runtime, seeds.size(), attempted, compiled,
                              elapsed_us, budget_hit);
}

void RecordBusAdralnReject(
    Runtime *runtime, lsx4::fex2608::BusAdralnRejectReason reason) noexcept {
  if (runtime == nullptr || !runtime->bus_adraln_rejects.Record(reason)) {
    return;
  }
  const auto literal = lsx4::fex2608::BusAdralnRejectLiteral(reason);
  if (literal.data != nullptr && literal.size != 0) {
    const int output_fd = runtime->bus_adraln_reject_fd >= 0
                              ? runtime->bus_adraln_reject_fd
                              : STDERR_FILENO;
    (void)::write(output_fd, literal.data, literal.size);
  }
}

void RecordBusAdralnBusyMetadataRecovery(Runtime *runtime) noexcept {
  if (runtime == nullptr ||
      runtime->bus_adraln_busy_metadata_recoveries.fetch_add(
          1, std::memory_order_relaxed) != 0) {
    return;
  }
  constexpr char literal[] =
      "FEX2608_BUS_ADRALN_RECOVER reason=metadata_spin_lock_busy\n";
  const int output_fd = runtime->bus_adraln_reject_fd >= 0
                            ? runtime->bus_adraln_reject_fd
                            : STDERR_FILENO;
  (void)::write(output_fd, literal, sizeof(literal) - 1);
}

uint32_t PublishDeferredWriteEpochs(Runtime *runtime,
                                    ThreadRecord *record) noexcept {
  if (runtime == nullptr || record == nullptr ||
      runtime->deferred_write_epoch_callbacks.publish_ready == nullptr) {
    return 0;
  }
  const auto callbacks = runtime->deferred_write_epoch_callbacks;
  const uint32_t promoted = lsx4::fex2608::DrainDeferredWriteEpochSnapshot(
      &record->deferred_write_epochs, [&](const uint64_t token) noexcept {
        callbacks.publish_ready(callbacks.callback_context, token);
      });
  // The counter value is unchanged by fetch_add(0), but the shared RMW still
  // claims the cache line on every empty HLE safe point. Preserve the legacy
  // operation only for same-binary A/B; every real promotion remains counted.
  if (promoted != 0 ||
      (runtime->create_flags &
       LSX4_FEX2608_CREATE_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB) != 0) {
    runtime->deferred_write_epoch_promoted.fetch_add(
        promoted, std::memory_order_relaxed);
  }
  return promoted;
}

constexpr uint64_t GuestFpcrFromMxcsr(uint64_t fpcr, uint32_t mxcsr) noexcept {
  constexpr uint64_t kRoundingMask = UINT64_C(3) << 22;
  constexpr uint64_t kFlushToZeroMask = UINT64_C(1) << 24;
  const uint64_t x86_rounding = (mxcsr >> 13) & UINT32_C(3);
  const uint64_t arm_rounding =
      ((x86_rounding & UINT64_C(1)) << 1) | ((x86_rounding & UINT64_C(2)) >> 1);
  return (fpcr & ~(kRoundingMask | kFlushToZeroMask)) | (arm_rounding << 22) |
         (static_cast<uint64_t>((mxcsr >> 15) & UINT32_C(1)) << 24);
}

static_assert(GuestFpcrFromMxcsr(0, UINT32_C(0x00001f80)) == 0);
static_assert(GuestFpcrFromMxcsr(0, UINT32_C(0x00003f80)) ==
              (UINT64_C(2) << 22));
static_assert(GuestFpcrFromMxcsr(0, UINT32_C(0x00005f80)) ==
              (UINT64_C(1) << 22));
static_assert(GuestFpcrFromMxcsr(0, UINT32_C(0x00007f80)) ==
              (UINT64_C(3) << 22));
static_assert(GuestFpcrFromMxcsr(0, UINT32_C(0x00009f80)) ==
              (UINT64_C(1) << 24));

void ApplyGuestFpcr(const ThreadRecord *record) noexcept {
  if (record == nullptr || record->thread == nullptr ||
      record->thread->CurrentFrame == nullptr) {
    return;
  }
  const uint64_t current = lsx4_fex2608_read_fpcr();
  const uint32_t mxcsr = record->thread->CurrentFrame->State.mxcsr;
  const uint64_t desired = GuestFpcrFromMxcsr(current, mxcsr);
  if (desired != current) {
    lsx4_fex2608_write_fpcr(desired);
  }
}

class ScopedGuestFpcr final {
public:
  explicit ScopedGuestFpcr(const ThreadRecord *record) noexcept
      : host_fpcr_{lsx4_fex2608_read_fpcr()} {
    if (record == nullptr || record->thread == nullptr ||
        record->thread->CurrentFrame == nullptr) {
      return;
    }
    const uint32_t mxcsr = record->thread->CurrentFrame->State.mxcsr;
    const uint64_t desired = GuestFpcrFromMxcsr(host_fpcr_, mxcsr);
    if (desired != host_fpcr_) {
      lsx4_fex2608_write_fpcr(desired);
    }
  }

  ~ScopedGuestFpcr() {
    // Guest MXCSR instructions may have changed FPCR after construction, so
    // re-check at the boundary rather than relying on the initial value.
    if (lsx4_fex2608_read_fpcr() != host_fpcr_) {
      lsx4_fex2608_write_fpcr(host_fpcr_);
    }
  }

  ScopedGuestFpcr(const ScopedGuestFpcr &) = delete;
  ScopedGuestFpcr &operator=(const ScopedGuestFpcr &) = delete;

private:
  uint64_t host_fpcr_{};
};

void ExecuteThreadWithGuestFpcr(Runtime *runtime, ThreadRecord *record) {
  ScopedGuestFpcr fpcr{record};
  runtime->context->ExecuteThread(record->thread);
  (void)PublishDeferredWriteEpochs(runtime, record);
}

void HandleCallbackWithGuestFpcr(Runtime *runtime, ThreadRecord *record,
                                 uint64_t guest_function) {
  ScopedGuestFpcr fpcr{record};
  runtime->context->HandleCallback(record->thread, guest_function);
  (void)PublishDeferredWriteEpochs(runtime, record);
}

uint32_t GuestMxcsrMask(const Runtime *runtime) noexcept {
  return runtime != nullptr && runtime->exact_guest_cpu_profile
             ? runtime->guest_cpu_profile.mxcsr_mask
             : kDefaultSupportedMxcsrMask;
}

bool IsGuestCpuProfileValid(const Lsx4FexGuestCpuProfileV6 *profile) noexcept {
  if (profile == nullptr) {
    return true;
  }
  if (profile->struct_size < sizeof(*profile) ||
      profile->abi_version != LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6 ||
      profile->identity_low == 0 || profile->identity_high == 0 ||
      profile->profile_id == nullptr || profile->profile_id[0] == '\0' ||
      ::strnlen(profile->profile_id, kGuestCpuProfileIdCapacity) >=
          kGuestCpuProfileIdCapacity ||
      profile->query_cpuid == nullptr || profile->query_xcr == nullptr ||
      profile->mxcsr_mask == 0 ||
      (profile->mxcsr_reset_value & ~profile->mxcsr_mask) != 0 ||
      profile->legacy_save_area_bytes != 512 || profile->profile_flags != 0) {
    return false;
  }
  return true;
}

__uint128_t QueryGuestCpuid(void *opaque, uint64_t function,
                            uint64_t subleaf) noexcept {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || !runtime->exact_guest_cpu_profile) {
    return 0;
  }
  const Lsx4FexCpuidResultV6 value = runtime->guest_cpu_profile.query_cpuid(
      runtime->guest_cpu_profile.profile_context,
      static_cast<uint32_t>(function), static_cast<uint32_t>(subleaf));
  const uint64_t low = static_cast<uint64_t>(value.eax) |
                       (static_cast<uint64_t>(value.ebx) << 32);
  const uint64_t high = static_cast<uint64_t>(value.ecx) |
                        (static_cast<uint64_t>(value.edx) << 32);
  return (static_cast<__uint128_t>(high) << 64) | low;
}

uint64_t QueryGuestXcr(void *opaque, uint32_t index) noexcept {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || !runtime->exact_guest_cpu_profile) {
    return 0;
  }
  return runtime->guest_cpu_profile.query_xcr(
      runtime->guest_cpu_profile.profile_context, index);
}

void Log(Runtime *runtime, int32_t priority, const char *component,
         const char *message) {
  if (runtime != nullptr && runtime->callbacks.log != nullptr) {
    runtime->callbacks.log(runtime->callbacks.callback_context, priority,
                           component, message);
  }
}

void LogHleResultContractFailure(Runtime *runtime, const char *path,
                                 const int32_t status,
                                 const Lsx4FexHleCallFrameV3 &call,
                                 const Lsx4FexHleResultFrameV3 &result) {
  char message[640]{};
  std::snprintf(
      message, sizeof(message),
      "path=%s status=%d native=0x%llx rip=0x%llx rsp=0x%llx "
      "callId=%llu resultSize=%u resultAbi=%u resultCallId=%llu "
      "resultStatus=%d resultFlags=0x%x inputRax=0x%llx "
      "gpr=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]",
      path != nullptr ? path : "-", status,
      static_cast<unsigned long long>(call.native_function),
      static_cast<unsigned long long>(call.guest_rip),
      static_cast<unsigned long long>(call.guest_rsp),
      static_cast<unsigned long long>(call.call_id), result.struct_size,
      result.abi_version,
      static_cast<unsigned long long>(result.call_id), result.status,
      result.result_flags,
      static_cast<unsigned long long>(call.input_rax),
      static_cast<unsigned long long>(call.gpr[0]),
      static_cast<unsigned long long>(call.gpr[1]),
      static_cast<unsigned long long>(call.gpr[2]),
      static_cast<unsigned long long>(call.gpr[3]),
      static_cast<unsigned long long>(call.gpr[4]),
      static_cast<unsigned long long>(call.gpr[5]));
  Log(runtime, 6, "hle-result-contract", message);
}

void LogCallbackCheckpoint(Runtime *runtime, ThreadRecord *record,
                           uint32_t sequence, const char *stage,
                           uint64_t guest_function) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      sequence >= 8) {
    return;
  }
  const auto *frame = record->thread->CurrentFrame;
  const uint64_t rsp = frame->State.gregs[FEXCore::X86State::REG_RSP];
  std::array<uint64_t, 4> stack{};
  bool stack_valid = false;
  if (record->stack_size >= sizeof(stack) && rsp >= record->stack_base &&
      rsp - record->stack_base <= record->stack_size - sizeof(stack)) {
    std::memcpy(stack.data(), reinterpret_cast<const void *>(rsp),
                sizeof(stack));
    stack_valid = true;
  }
  char message[640]{};
  std::snprintf(
      message, sizeof(message),
      "seq=%u stage=%s fn=0x%llx rip=0x%llx rsp=0x%llx cbret=0x%llx "
      "callret_sp=0x%llx signal_ref=%u capture=%u host_pc=0x%llx "
      "host_sp=0x%llx in_jit=%u rdi=0x%llx rsi=0x%llx r10=0x%llx "
      "r11=0x%llx stack_valid=%u stack=[0x%llx,0x%llx,0x%llx,0x%llx]",
      sequence, stage != nullptr ? stage : "-",
      static_cast<unsigned long long>(guest_function),
      static_cast<unsigned long long>(frame->State.rip),
      static_cast<unsigned long long>(rsp),
      static_cast<unsigned long long>(frame->Pointers.ThunkCallbackRet),
      static_cast<unsigned long long>(frame->State.callret_sp),
      frame->SignalHandlerRefCounter,
      static_cast<uint32_t>(
          record->host_signal.gate.phase.load(std::memory_order_acquire)),
      static_cast<unsigned long long>(record->host_signal.host_pc),
      static_cast<unsigned long long>(record->host_signal.host_sp),
      record->host_signal.was_in_jit,
      static_cast<unsigned long long>(
          frame->State.gregs[FEXCore::X86State::REG_RDI]),
      static_cast<unsigned long long>(
          frame->State.gregs[FEXCore::X86State::REG_RSI]),
      static_cast<unsigned long long>(
          frame->State.gregs[FEXCore::X86State::REG_R10]),
      static_cast<unsigned long long>(
          frame->State.gregs[FEXCore::X86State::REG_R11]),
      stack_valid ? 1U : 0U, static_cast<unsigned long long>(stack[0]),
      static_cast<unsigned long long>(stack[1]),
      static_cast<unsigned long long>(stack[2]),
      static_cast<unsigned long long>(stack[3]));
  Log(runtime, 4, "callback", message);
}

void Fatal(Runtime *runtime, int32_t status, const char *message) {
  Log(runtime, 6, "bridge", message);
  if (runtime != nullptr && runtime->callbacks.fatal_error != nullptr) {
    runtime->callbacks.fatal_error(runtime->callbacks.callback_context, status,
                                   message);
  }
}

bool ArmGuestSignalInterrupt(ThreadRecord *record) {
  if (record == nullptr || record->thread == nullptr) {
    return false;
  }
  std::lock_guard lock{record->signal_mutex};
  if (record->signal_queue_read == record->signal_queue_write ||
      !record->active.load(std::memory_order_acquire) ||
      record->signal_drain_active ||
      record->interrupt_page_armed.load(std::memory_order_acquire)) {
    return true;
  }
  record->interrupt_page_armed.store(true, std::memory_order_release);
  if (::mprotect(record->thread->InterruptFaultPage,
                 sizeof(record->thread->InterruptFaultPage), PROT_NONE) != 0) {
    record->interrupt_page_armed.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

bool ArmDeferredWriteEpochInterrupt(ThreadRecord *record,
                                    const bool was_queue_empty) noexcept {
  if (record == nullptr || record->thread == nullptr ||
      !record->active.load(std::memory_order_acquire)) {
    return false;
  }
  if (record->interrupt_page_armed.load(std::memory_order_acquire)) {
    if (!was_queue_empty) {
      return true;
    }
    /*
     * A remote guest-signal producer publishes interrupt_page_armed before
     * entering mprotect.  Repeat the idempotent syscall for the first epoch
     * token so returning from this signal handler proves the page is already
     * protected; later epoch tokens in the same block avoid the syscall.
     */
    return ::mprotect( record->thread->InterruptFaultPage,
                     sizeof(record->thread->InterruptFaultPage),
                     PROT_NONE) == 0;
  }
  if (::mprotect( record->thread->InterruptFaultPage,
                sizeof(record->thread->InterruptFaultPage), PROT_NONE) != 0) {
    return false;
  }
  record->interrupt_page_armed.store(true, std::memory_order_release);
  return true;
}

bool ArmDeferredPinReleaseInterrupt(ThreadRecord *record) {
  if (record == nullptr || record->thread == nullptr) {
    return false;
  }
  std::lock_guard lock{record->signal_mutex};
  if (!record->active.load(std::memory_order_acquire)) {
    return false;
  }
  record->deferred_pin_release_interrupt.store(true, std::memory_order_release);
  if (record->interrupt_page_armed.load(std::memory_order_acquire)) {
    return true;
  }
  record->interrupt_page_armed.store(true, std::memory_order_release);
  if (::mprotect( record->thread->InterruptFaultPage,
                sizeof(record->thread->InterruptFaultPage), PROT_NONE) != 0) {
    record->interrupt_page_armed.store(false, std::memory_order_release);
    record->deferred_pin_release_interrupt.store(false,
                                                 std::memory_order_release);
    return false;
  }
  return true;
}

void CompleteDeferredPinAtDispatcher(ThreadRecord *record) noexcept {
  if (record == nullptr || !record->deferred_pin_release_interrupt.exchange(
                               false, std::memory_order_acq_rel)) {
    return;
  }
  if (record->deferred_signal.phase.load(std::memory_order_acquire) ==
      DeferredPhase(DeferredSignalPhase::ResumeOriginal)) {
    ReleaseDeferredPin(record);
    record->deferred_signal.phase.store(
        DeferredPhase(DeferredSignalPhase::Idle), std::memory_order_release);
  }
}

bool DisarmGuestSignalInterrupt(ThreadRecord *record) {
  if (record == nullptr || record->thread == nullptr) {
    return false;
  }
  if (!record->interrupt_page_armed.load(std::memory_order_acquire)) {
    return true;
  }
  std::lock_guard lock{record->signal_mutex};
  if (!record->interrupt_page_armed.load(std::memory_order_acquire)) {
    return true;
  }
  if (::mprotect(record->thread->InterruptFaultPage,
                 sizeof(record->thread->InterruptFaultPage),
                 PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  record->interrupt_page_armed.store(false, std::memory_order_release);
  return true;
}

enum class GuestSignalDrainBegin {
  Started,
  AlreadyActive,
  Failed,
};

GuestSignalDrainBegin BeginGuestSignalDrain(ThreadRecord *record) {
  if (record == nullptr || record->thread == nullptr) {
    return GuestSignalDrainBegin::Failed;
  }
  std::lock_guard lock{record->signal_mutex};
  if (record->signal_drain_active) {
    return GuestSignalDrainBegin::AlreadyActive;
  }
  record->signal_drain_active = true;
  if (!record->interrupt_page_armed.load(std::memory_order_acquire)) {
    return GuestSignalDrainBegin::Started;
  }
  if (::mprotect(record->thread->InterruptFaultPage,
                 sizeof(record->thread->InterruptFaultPage),
                 PROT_READ | PROT_WRITE) != 0) {
    record->signal_drain_active = false;
    return GuestSignalDrainBegin::Failed;
  }
  record->interrupt_page_armed.store(false, std::memory_order_release);
  return GuestSignalDrainBegin::Started;
}

bool IsGuestSignalInterruptFault(const ThreadRecord *record, int32_t signum,
                                 uint64_t fault_address) {
  if (record == nullptr || record->thread == nullptr || signum != SIGSEGV) {
    return false;
  }
  const uint64_t begin =
      reinterpret_cast<uint64_t>(record->thread->InterruptFaultPage);
  return fault_address >= begin &&
         fault_address < begin + sizeof(record->thread->InterruptFaultPage);
}

// Called only before an exact adapter-owned PROT_NONE guard-range check.
// Darwin can report a reserved PROT_NONE page as a translation fault (ESR
// DFSC 0x07), normalized to SEGV_MAPERR. Linux retains its ACCERR contract.
bool IsOwnedGuardPageFault(int32_t signum, const siginfo_t* info) noexcept {
  if (signum != SIGSEGV || info == nullptr || info->si_signo != SIGSEGV) {
    return false;
  }
#if defined(__APPLE__)
  return info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR;
#else
  return info->si_code == SEGV_ACCERR;
#endif
}

bool IsCallRetStackGuardAddress(const ThreadRecord *record, size_t page_size,
                                uintptr_t address) noexcept {
  if (record == nullptr || record->callret_allocation == nullptr ||
      record->callret_allocation_size == 0 ||
      record->callret_reset_sp == 0 || page_size == 0 ||
      page_size > record->callret_allocation_size / 2) {
    return false;
  }
  const uintptr_t begin =
      reinterpret_cast<uintptr_t>(record->callret_allocation);
  if (address < begin) {
    return false;
  }
  const uintptr_t offset = address - begin;
  return offset < page_size ||
         (offset < record->callret_allocation_size &&
          offset >= record->callret_allocation_size - page_size);
}

bool IsDeferredInterruptProbeStore(const ucontext_t *context,
                                   uint64_t fault_address) noexcept {
#if defined(__aarch64__)
  if (context == nullptr || lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)) == 0) {
    return false;
  }
  const uint32_t instruction =
      *reinterpret_cast<const uint32_t *>(lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)));
  // FEX emits one of these exact stores after dropping the outermost
  // DeferredSignalRefCount:
  //   STR  XZR, [Xn|SP, #imm12*8] in host-side scope guards;
  //   STRB WZR, [Xn|SP, #imm12]   in dispatcher guarded regions.
  // Both are pure interrupt probes and may be skipped in place. Redirecting
  // the dispatcher form through ThreadStop loses its live branch target and
  // replays the preceding guest block after the outer loop resumes.
  const bool is_str_xzr =
      (instruction & UINT32_C(0xffc0001f)) == UINT32_C(0xf900001f);
  const bool is_strb_wzr =
      (instruction & UINT32_C(0xffc0001f)) == UINT32_C(0x3900001f);
  if (!is_str_xzr && !is_strb_wzr) {
    return false;
  }
  const uint32_t base_index = (instruction >> 5) & 31U;
  const uint64_t base = base_index == 31U
                            ? lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context))
                            : lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), base_index);
  const uint64_t scale = is_str_xzr ? 8U : 1U;
  const uint64_t offset =
      static_cast<uint64_t>((instruction >> 10) & UINT32_C(0xfff)) * scale;
  return !AddOverflows(base, offset) && base + offset == fault_address;
#else
  (void)context;
  (void)fault_address;
  return false;
#endif
}

bool ThreadMatches(const ThreadRecord *record,
                   const Lsx4FexThreadIdentityV3 *requested) {
  if (record == nullptr || record->finalized.load(std::memory_order_acquire)) {
    return false;
  }
  if (requested == nullptr) {
    return true;
  }
  if (!IsThreadIdentityHeaderValid(requested)) {
    return false;
  }
  if (requested->backend_thread != 0 &&
      requested->backend_thread != record->identity.backend_thread) {
    return false;
  }
  if (requested->guest_thread != 0 &&
      requested->guest_thread != record->identity.guest_thread) {
    return false;
  }
  if (requested->pthread_handle != 0 &&
      requested->pthread_handle != record->identity.pthread_handle) {
    return false;
  }
  return requested->host_tid == 0 ||
         requested->host_tid == record->identity.host_tid;
}

ThreadRecord *CurrentRecord(Runtime *runtime,
                            const Lsx4FexThreadIdentityV3 *requested,
                            bool require_thread) {
  ThreadRecord *record =
      g_thread_binding.runtime == runtime ? g_thread_binding.record : nullptr;
  if (!ThreadMatches(record, requested)) {
    return nullptr;
  }
  if (require_thread && (record == nullptr || record->thread == nullptr)) {
    return nullptr;
  }
  return record;
}

void ResetCallRetShadowStack(ThreadRecord *record);

void ForceCurrentToSentinel(Runtime *runtime, ThreadRecord *record,
                            int32_t status, const char *message) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr) {
    return;
  }
  record->controlled_failure = true;
  record->failure_status = status;
  record->thread->CurrentFrame->State.rip = runtime->exit_sentinel;
  record->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] =
      runtime->abort_stack_pointer;
  ResetCallRetShadowStack(record);
  record->context_epoch.fetch_add(1, std::memory_order_release);
  if (g_thread_binding.runtime == runtime &&
      g_thread_binding.record == record && g_thread_binding.thunk_depth != 0) {
    record->nonlocal_pending = true;
  }
  Fatal(runtime, status, message);
}

bool ValidateAndSortRanges(std::vector<Lsx4FexExecutableRangeV5> &ranges) {
  for (const auto &range : ranges) {
    if (range.size == 0 || AddOverflows(range.address, range.size) ||
        (range.range_flags & LSX4_FEX_RANGE_EXECUTABLE) == 0) {
      return false;
    }
  }
  std::sort(ranges.begin(), ranges.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.address < rhs.address;
  });
  for (size_t index = 1; index < ranges.size(); ++index) {
    const auto &previous = ranges[index - 1];
    if (previous.address + previous.size > ranges[index].address) {
      return false;
    }
  }
  return true;
}

bool PreservesPublishedRanges(
    const ExecutableRangeStore *current,
    const std::vector<Lsx4FexExecutableRangeV5> &replacement) {
  if (current == nullptr) {
    return true;
  }
  size_t replacement_index = 0;
  for (const auto &published : current->ranges) {
    while (replacement_index < replacement.size() &&
           replacement[replacement_index].address < published.address) {
      ++replacement_index;
    }
    if (replacement_index == replacement.size()) {
      return false;
    }
    const auto &candidate = replacement[replacement_index++];
    if (candidate.address != published.address ||
        candidate.size != published.size ||
        candidate.range_cookie != published.range_cookie ||
        candidate.protection != published.protection ||
        candidate.range_flags != published.range_flags) {
      return false;
    }
  }
  return true;
}

int32_t ReplaceRanges(Runtime *runtime,
                      const Lsx4FexExecutableRangeSnapshotV5 *snapshot) {
  if (runtime == nullptr || !IsSnapshotHeaderValid(snapshot)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  std::vector<Lsx4FexExecutableRangeV5> replacement;
  if (snapshot->range_count != 0) {
    replacement.assign(snapshot->ranges,
                       snapshot->ranges + snapshot->range_count);
  }
  if (!ValidateAndSortRanges(replacement)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  std::lock_guard lock{runtime->range_update_mutex};
  const ExecutableRangeStore *current =
      runtime->executable_ranges.load(std::memory_order_acquire);
  const uint64_t current_generation =
      current != nullptr ? current->generation : 0;
  const uint64_t generation =
      snapshot->generation != 0 ? snapshot->generation : current_generation + 1;
  if (generation < current_generation) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  if (generation == current_generation) {
    if (current == nullptr || replacement.size() != current->ranges.size()) {
      return LSX4_FEX_STATUS_INVALID_STATE;
    }
    for (size_t index = 0; index < replacement.size(); ++index) {
      const auto &lhs = replacement[index];
      const auto &rhs = current->ranges[index];
      if (lhs.address != rhs.address || lhs.size != rhs.size ||
          lhs.range_cookie != rhs.range_cookie ||
          lhs.protection != rhs.protection ||
          lhs.range_flags != rhs.range_flags) {
        return LSX4_FEX_STATUS_INVALID_STATE;
      }
    }
    return LSX4_FEX_STATUS_OK;
  }
  // Existing translations embed executable-range policy.  A plain snapshot
  // replacement may only add disjoint ranges; removal or policy changes must
  // first pass through invalidate_executable_ranges so stale blocks cannot
  // retain an obsolete interrupt contract.
  if (!PreservesPublishedRanges(current, replacement)) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  auto *published = new (std::nothrow)
      ExecutableRangeStore{generation, std::move(replacement)};
  if (published == nullptr) {
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  /*
   * Readers run without locks, including from fault-adjacent paths.  This
   * process-resident backend intentionally retains superseded immutable
   * snapshots until process exit, so every acquire-loaded pointer remains
   * valid without epochs or signal-unsafe reclamation.
   */
  runtime->executable_ranges.store(published, std::memory_order_release);
  return LSX4_FEX_STATUS_OK;
}

int32_t RefreshRanges(Runtime *runtime) {
  if (runtime == nullptr || runtime->snapshot_ranges == nullptr) {
    return LSX4_FEX_STATUS_OK;
  }
  for (unsigned attempt = 0; attempt != 4; ++attempt) {
    Lsx4FexExecutableRangeSnapshotV5 query{};
    query.struct_size = sizeof(Lsx4FexExecutableRangeSnapshotV5);
    query.abi_version = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5;
    int32_t status = runtime->snapshot_ranges(runtime->range_context, &query);
    if (status != LSX4_FEX_STATUS_OK) {
      return status;
    }
    std::vector<Lsx4FexExecutableRangeV5> ranges(query.range_count);
    Lsx4FexExecutableRangeSnapshotV5 copy{};
    copy.struct_size = sizeof(Lsx4FexExecutableRangeSnapshotV5);
    copy.abi_version = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5;
    copy.generation = query.generation;
    copy.ranges = ranges.empty() ? nullptr : ranges.data();
    copy.capacity = static_cast<uint32_t>(ranges.size());
    status = runtime->snapshot_ranges(runtime->range_context, &copy);
    if (status == LSX4_FEX_STATUS_BUFFER_TOO_SMALL ||
        copy.generation != query.generation) {
      continue;
    }
    if (status != LSX4_FEX_STATUS_OK || copy.range_count > copy.capacity) {
      return status != LSX4_FEX_STATUS_OK ? status
                                          : LSX4_FEX_STATUS_INVALID_STATE;
    }
    ranges.resize(copy.range_count);
    copy.ranges = ranges.empty() ? nullptr : ranges.data();
    copy.capacity = static_cast<uint32_t>(ranges.size());
    return ReplaceRanges(runtime, &copy);
  }
  return LSX4_FEX_STATUS_INVALID_STATE;
}

int32_t ValidateRangeSubset(Runtime *runtime,
                            const Lsx4FexExecutableRangeSnapshotV5 *snapshot) {
  if (snapshot == nullptr) {
    return LSX4_FEX_STATUS_OK;
  }
  if (!IsSnapshotHeaderValid(snapshot)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const ExecutableRangeStore *store =
      runtime->executable_ranges.load(std::memory_order_acquire);
  if (store == nullptr) {
    return snapshot->range_count == 0 ? LSX4_FEX_STATUS_OK
                                      : LSX4_FEX_STATUS_NOT_FOUND;
  }
  for (uint32_t index = 0; index < snapshot->range_count; ++index) {
    const auto &requested = snapshot->ranges[index];
    if (requested.size == 0 ||
        AddOverflows(requested.address, requested.size) ||
        (requested.range_flags & LSX4_FEX_RANGE_EXECUTABLE) == 0) {
      return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    const auto found = std::upper_bound(
        store->ranges.begin(), store->ranges.end(), requested.address,
        [](uint64_t value, const Lsx4FexExecutableRangeV5 &range) {
          return value < range.address;
        });
    if (found == store->ranges.begin()) {
      return LSX4_FEX_STATUS_NOT_FOUND;
    }
    const auto &canonical = *std::prev(found);
    if (requested.address < canonical.address ||
        requested.address + requested.size >
            canonical.address + canonical.size ||
        (canonical.range_flags & LSX4_FEX_RANGE_EXECUTABLE) == 0 ||
        (requested.protection & canonical.protection) != requested.protection) {
      return LSX4_FEX_STATUS_NOT_FOUND;
    }
  }
  return LSX4_FEX_STATUS_OK;
}

bool IsExecutableAddress(Runtime *runtime, uint64_t address, uint64_t size) {
  if (runtime == nullptr || size == 0 || AddOverflows(address, size)) {
    return false;
  }
  if (address >= runtime->exit_sentinel &&
      address + size <= runtime->exit_sentinel + runtime->page_size) {
    return true;
  }
  const ExecutableRangeStore *store =
      runtime->executable_ranges.load(std::memory_order_acquire);
  if (store == nullptr) {
    return false;
  }
  const auto it = std::upper_bound(
      store->ranges.begin(), store->ranges.end(), address,
      [](uint64_t value, const Lsx4FexExecutableRangeV5 &range) {
        return value < range.address;
      });
  if (it == store->ranges.begin()) {
    return false;
  }
  const auto &range = *std::prev(it);
  return address >= range.address &&
         address + size <= range.address + range.size;
}

bool AddressDefersPendingInterrupt(Runtime *runtime, uint64_t address) {
  if (runtime == nullptr) {
    return true;
  }
  const ExecutableRangeStore *store =
      runtime->executable_ranges.load(std::memory_order_acquire);
  if (store == nullptr) {
    return true;
  }
  const auto it = std::upper_bound(
      store->ranges.begin(), store->ranges.end(), address,
      [](uint64_t value, const Lsx4FexExecutableRangeV5 &range) {
        return value < range.address;
      });
  if (it == store->ranges.begin()) {
    return true;
  }
  const auto &range = *std::prev(it);
  return address < range.address || address >= range.address + range.size ||
         (range.range_flags & LSX4_FEX_RANGE_HLE_TRAMPOLINE) != 0;
}

bool SegmentContains(const Lsx4FexMappedEntryRequestV5 &request,
                     uint64_t address, uint64_t size, uint32_t protection) {
  if (size == 0 || AddOverflows(address, size)) {
    return false;
  }
  for (uint32_t index = 0; index < request.segment_count; ++index) {
    const auto &segment = request.segments[index];
    if (segment.size == 0 || AddOverflows(segment.address, segment.size)) {
      continue;
    }
    if (address >= segment.address &&
        address + size <= segment.address + segment.size &&
        (segment.protection & protection) == protection) {
      return true;
    }
  }
  return false;
}

bool AllocateGuarded(Runtime *runtime, size_t usable_size, void **allocation,
                     size_t *allocation_size, uint64_t *usable_base) {
  if (runtime == nullptr || allocation == nullptr ||
      allocation_size == nullptr || usable_base == nullptr ||
      usable_size >
          std::numeric_limits<size_t>::max() - 2 * runtime->page_size) {
    return false;
  }
  const size_t total = usable_size + 2 * runtime->page_size;
  void *mapped = FEXCore::Allocator::mmap(nullptr, total, PROT_NONE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapped == MAP_FAILED) {
    return false;
  }
  auto *base = static_cast<uint8_t *>(mapped) + runtime->page_size;
  if (::mprotect(base, usable_size, PROT_READ | PROT_WRITE) != 0) {
    FEXCore::Allocator::munmap(mapped, total);
    return false;
  }
  *allocation = mapped;
  *allocation_size = total;
  *usable_base = reinterpret_cast<uint64_t>(base);
  return true;
}

bool ReleaseThreadMappings(ThreadRecord *record) {
  if (!DeferredSignalStateIsQuiescent(record)) {
    return false;
  }
  if (record->callret_allocation != nullptr) {
    FEXCore::Allocator::munmap(record->callret_allocation,
                               record->callret_allocation_size);
    record->callret_allocation = nullptr;
    record->callret_allocation_size = 0;
  }
  if (record->scratch_allocation != nullptr) {
    FEXCore::Allocator::munmap(record->scratch_allocation,
                               record->scratch_allocation_size);
    record->scratch_allocation = nullptr;
    record->scratch_allocation_size = 0;
    record->scratch_base = 0;
    record->scratch_size = 0;
  }
  if (record->deferred_stack_allocation != nullptr) {
    FEXCore::Allocator::munmap(record->deferred_stack_allocation,
                               record->deferred_stack_allocation_size);
    record->deferred_stack_allocation = nullptr;
    record->deferred_stack_allocation_size = 0;
    record->deferred_stack_base = 0;
    record->deferred_stack_size = 0;
  }
  return true;
}

bool PrepareThreadMappings(Runtime *runtime, ThreadRecord *record) {
  uint64_t callret_base{};
  if (!AllocateGuarded(runtime,
                       FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE,
                       &record->callret_allocation,
                       &record->callret_allocation_size, &callret_base)) {
    return false;
  }
  if (!AllocateGuarded(
          runtime, kCallbackScratchSize, &record->scratch_allocation,
          &record->scratch_allocation_size, &record->scratch_base)) {
    (void)ReleaseThreadMappings(record);
    return false;
  }
  record->scratch_size = kCallbackScratchSize;
  if (!AllocateGuarded(runtime, kDeferredSignalStackSize,
                       &record->deferred_stack_allocation,
                       &record->deferred_stack_allocation_size,
                       &record->deferred_stack_base)) {
    (void)ReleaseThreadMappings(record);
    return false;
  }
  record->deferred_stack_size = kDeferredSignalStackSize;
  return true;
}

bool InitializeFexThreadRecord(Runtime *runtime, ThreadRecord *record) {
  auto *thread = record->thread;
  if (runtime->exact_guest_cpu_profile) {
    if (!lsx4::fex2608::InstallGuestCpuProfileIR(
            thread, runtime->guest_cpu_profile.mxcsr_mask)) {
      return false;
    }
    thread->CurrentFrame->Pointers.CPUIDObj =
        reinterpret_cast<uint64_t>(runtime);
    thread->CurrentFrame->Pointers.CPUIDFunction =
        reinterpret_cast<uint64_t>(&QueryGuestCpuid);
    thread->CurrentFrame->Pointers.XCRFunction =
        reinterpret_cast<uint64_t>(&QueryGuestXcr);
    thread->CurrentFrame->State.mxcsr =
        runtime->guest_cpu_profile.mxcsr_reset_value;
  }
  if (runtime->guest_store_trace &&
      !lsx4::fex2608::InstallGuestStoreTraceIR(
          thread, reinterpret_cast<uint64_t>(&record->identity.host_tid))) {
    return false;
  }
  if (runtime->guest_write_op_coverage &&
      !lsx4::fex2608::InstallGuestWriteOpCoverageIR(thread)) {
    return false;
  }
  if (!lsx4::fex2608::InstallProofMetricsIR(
          thread, runtime->proof_metrics_epoch,
          runtime->proof_metrics_mode)) {
    return false;
  }
  const uint64_t callret_base =
      reinterpret_cast<uint64_t>(record->callret_allocation) +
      runtime->page_size;
  thread->CallRetStackBase = reinterpret_cast<void *>(callret_base);
  record->callret_reset_sp =
      callret_base + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
  thread->CurrentFrame->State.callret_sp = record->callret_reset_sp;
  thread->FrontendPtr = record;
  thread->CurrentFrame->State
      .segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] =
      record->gdt;
  thread->CurrentFrame->State
      .segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] =
      record->gdt;
  thread->CurrentFrame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS
                                       << 3;
  auto *code = FEXCore::Core::CPUState::GetSegmentFromIndex(
      thread->CurrentFrame->State, thread->CurrentFrame->State.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(code, 0);
  FEXCore::Core::CPUState::SetGDTLimit(code, UINT32_C(0x000fffff));
  // Match FEX's 64-bit Linux frontend thread bootstrap.  Merely selecting
  // CONFIG_IS64BIT_MODE is not sufficient: callback decoding also observes
  // the cached code-segment descriptor.
  code->L = 1;
  code->D = 0;
  thread->CurrentFrame->State.cs_cached =
      FEXCore::Core::CPUState::CalculateGDTBase(*code);
  thread->CurrentFrame->Pointers.ThunkCallbackRet =
      runtime->callback_return_sentinel;
  return true;
}

void ResetCallRetShadowStack(ThreadRecord *record) {
  if (record == nullptr || record->thread == nullptr ||
      record->thread->CallRetStackBase == nullptr ||
      record->callret_reset_sp == 0) {
    return;
  }
  record->thread->CurrentFrame->State.callret_sp = record->callret_reset_sp;
  FEXCore::Allocator::VirtualDontNeed(
      record->thread->CallRetStackBase,
      FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE);
}

bool CreatePersistentThread(Runtime *runtime, ThreadRecord *record) {
  if (record->thread != nullptr) {
    return true;
  }
  if (!PrepareThreadMappings(runtime, record)) {
    return false;
  }
  record->thread = runtime->context->CreateThread(0, 0, nullptr);
  if (record->thread == nullptr) {
    (void)ReleaseThreadMappings(record);
    return false;
  }
  if (!InitializeFexThreadRecord(runtime, record)) {
    runtime->context->DestroyThread(record->thread);
    record->thread = nullptr;
    (void)ReleaseThreadMappings(record);
    return false;
  }
  record->identity.backend_thread = reinterpret_cast<uint64_t>(record->thread);
  record->tls.target = record->identity;
  if (runtime->callbacks.thread_event != nullptr) {
    runtime->callbacks.thread_event(runtime->callbacks.callback_context,
                                    LSX4_FEX2608_THREAD_EVENT_CREATED,
                                    &record->identity);
  }
  return true;
}

bool ResetPersistentThreadForEntry(Runtime *runtime, ThreadRecord *record,
                                   uint64_t entry, uint64_t guest_rsp) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      !DeferredSignalStateIsQuiescent(record)) {
    return false;
  }
  auto &state = record->thread->CurrentFrame->State;
  const uint64_t l1_pointer = state.L1Pointer;
  const uint64_t l1_mask = state.L1Mask;
  const uint16_t cs_idx = state.cs_idx;
  const uint32_t cs_cached = state.cs_cached;
  auto *gdt =
      state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT];
  auto *ldt =
      state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT];
  FEXCore::Core::CPUState fresh{};
  std::memcpy(&state, &fresh, sizeof(state));
  state.L1Pointer = l1_pointer;
  state.L1Mask = l1_mask;
  state.callret_sp = record->callret_reset_sp;
  state.cs_idx = cs_idx;
  state.cs_cached = cs_cached;
  state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt;
  state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = ldt;
  state.rip = entry;
  state.gregs[FEXCore::X86State::REG_RSP] = guest_rsp;
  state.DeferredSignalRefCount.Store(0);
  ResetCallRetShadowStack(record);
  return true;
}

int32_t ExportContext(Runtime *runtime, ThreadRecord *record,
                      Lsx4FexGuestContextV3 *output) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      output == nullptr || output->struct_size < sizeof(*output) ||
      output->abi_version != LSX4_FEX_BACKEND_ABI_V3) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  auto *thread = record->thread;
  const auto &state = thread->CurrentFrame->State;
  const uint32_t capacity = output->struct_size;
  std::memset(output, 0, sizeof(*output));
  output->struct_size = capacity;
  output->abi_version = LSX4_FEX_BACKEND_ABI_V3;
  std::memcpy(output->gpr, state.gregs, sizeof(output->gpr));
  output->rip = state.rip;
  output->rflags =
      runtime->context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
  output->fs_base = state.fs_cached;
  output->gs_base = state.gs_cached;
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> lows{};
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> highs{};
  runtime->context->ReconstructXMMRegisters(thread, lows.data(), highs.data());
  for (size_t index = 0; index < lows.size(); ++index) {
    std::memcpy(&output->xmm[index], &lows[index], sizeof(output->xmm[index]));
  }
  output->mxcsr = state.mxcsr;
  output->context_flags = kAllContextFlags;
  output->guest_thread = record->identity.guest_thread;
  return LSX4_FEX_STATUS_OK;
}

int32_t ImportContext(Runtime *runtime, ThreadRecord *record,
                      const Lsx4FexGuestContextV3 *input, bool allow_rip) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      input == nullptr || input->struct_size < sizeof(*input) ||
      input->abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      (input->context_flags & ~kAllContextFlags) != 0) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  auto *thread = record->thread;
  auto &state = thread->CurrentFrame->State;
  if ((input->context_flags & LSX4_FEX_CONTEXT_GPR_VALID) != 0) {
    std::memcpy(state.gregs, input->gpr, sizeof(state.gregs));
    if (allow_rip) {
      state.rip = input->rip;
    }
  }
  if ((input->context_flags & LSX4_FEX_CONTEXT_FLAGS_VALID) != 0) {
    runtime->context->SetFlagsFromCompactedEFLAGS(
        thread, static_cast<uint32_t>(input->rflags));
  }
  if ((input->context_flags & LSX4_FEX_CONTEXT_TLS_VALID) != 0) {
    state.fs_cached = input->fs_base;
    state.gs_cached = input->gs_base;
  }
  if ((input->context_flags & LSX4_FEX_CONTEXT_XMM_VALID) != 0) {
    std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> lows{};
    std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> highs{};
    /* V3 carries only XMM lows.  Preserve the live AVX/YMM upper halves. */
    runtime->context->ReconstructXMMRegisters(thread, lows.data(),
                                              highs.data());
    for (size_t index = 0; index < lows.size(); ++index) {
      std::memcpy(&lows[index], &input->xmm[index], sizeof(input->xmm[index]));
    }
    runtime->context->SetXMMRegistersFromState(thread, lows.data(),
                                               highs.data());
    state.mxcsr = input->mxcsr;
  }
  return LSX4_FEX_STATUS_OK;
}

FEXCore::ThunkControlResult BridgeThunk(void *args,
                                        FEXCore::Core::CpuStateFrame *frame);
FEXCore::ThunkControlResult BridgeTypedTimeThunkLegacy(
    void *args, FEXCore::Core::CpuStateFrame *frame);
FEXCore::ThunkControlResult BridgeTypedTimeThunkFast(
    void *args, FEXCore::Core::CpuStateFrame *frame);
FEXCore::ThunkControlResult
RejectContextThunk(void *args, FEXCore::Core::CpuStateFrame *frame);
void RejectThunk(void *args);
int32_t ProcessQueuedSignals(Runtime *runtime, ThreadRecord *record);
int32_t ProcessHostSignalCapture(Runtime *runtime, ThreadRecord *record);
int32_t ProcessNormalInterruptPending(Runtime *runtime, ThreadRecord *record);

int32_t ExecuteGuestCallbackWithSignalResume(Runtime *runtime,
                                             ThreadRecord *record,
                                             uint64_t guest_function) {
  auto *const frame = record->thread->CurrentFrame;
  const uint64_t prior_returning_stack = frame->ReturningStackLocation;
  const uint64_t prior_guest_rsp =
      frame->State.gregs[FEXCore::X86State::REG_RSP];
  const uint64_t prior_callret_sp = frame->State.callret_sp;
  const uint32_t prior_signal_ref = frame->SignalHandlerRefCounter;
  struct TerminalCallbackFrameUnwind {
    ThreadRecord *record;
    FEXCore::Core::CpuStateFrame *frame;
    uint64_t returning_stack;
    uint64_t guest_rsp;
    uint64_t callret_sp;
    uint32_t signal_ref;

    ~TerminalCallbackFrameUnwind() {
      if (record == nullptr || frame == nullptr) {
        return;
      }
      frame->ReturningStackLocation = returning_stack;
      if ((!record->guest_exit_requested && !record->controlled_failure)) {
        return;
      }
      /*
       * ResumeContext deliberately bypasses FEX's guest CALLBACKRET. Balance
       * the callback-only bookkeeping that CALLBACKRET would have consumed,
       * while preserving the terminal RIP and guest result state.
       */
      frame->State.gregs[FEXCore::X86State::REG_RSP] = guest_rsp;
      frame->State.callret_sp = callret_sp;
      frame->SignalHandlerRefCounter = signal_ref;
    }
  } terminal_unwind{
      record,           frame,           prior_returning_stack, prior_guest_rsp,
      prior_callret_sp, prior_signal_ref};
  HandleCallbackWithGuestFpcr(runtime, record, guest_function);
  frame->ReturningStackLocation = prior_returning_stack;
  // A terminal callback may still own a captured host fault. Drain it first:
  // ProcessHostSignalCapture restores the exact pre-fault mask and closes the
  // capture gate before observing terminal state. No guest code is re-entered.
  while (lsx4::fex2608::SignalCaptureReady(&record->host_signal.gate)) {
    const int32_t status = ProcessHostSignalCapture(runtime, record);
    if (status != LSX4_FEX_STATUS_OK || record->guest_exit_requested ||
        record->controlled_failure) {
      return status;
    }

    /*
     * ThreadStop has already unwound the original callback dispatcher frame,
     * but its guest CALLBACKRET and SignalHandlerRefCounter are still live.
     * Resume through ExecuteThread: the original CALLBACKRET then consumes
     * exactly that frame.  Re-entering HandleCallback here would push a second
     * callback return sentinel and leak the first callback frame.
     */
    ExecuteThreadWithGuestFpcr(runtime, record);
  }
  if (record->guest_exit_requested || record->controlled_failure) {
    return LSX4_FEX_STATUS_OK;
  }
  return ProcessNormalInterruptPending(runtime, record);
}

FEXCore::ThunkedFunction *
BridgeThunkHandler::LookupThunk(const FEXCore::IR::SHA256Sum &sha256) {
  // This bridge requires the context-aware ABI.  If an old/corrupt IR path
  // asks for the legacy callback, reject it instead of silently consuming the
  // replacement context in the normal guest POP/RET epilogue.
  (void)sha256;
  return RejectThunk;
}

FEXCore::ContextThunkedFunction *
BridgeThunkHandler::LookupContextThunk(const FEXCore::IR::SHA256Sum &sha256) {
  if (std::memcmp(sha256.data, runtime_->thunk_identity.data(),
                  runtime_->thunk_identity.size()) == 0) {
    return BridgeThunk;
  }
  if (IsTypedTimeThunkIdentity(sha256)) {
    return (runtime_->create_flags &
            LSX4_FEX2608_CREATE_PRESERVE_TYPED_TIME_COMMON_FRAME_AB) != 0
               ? BridgeTypedTimeThunkLegacy
               : BridgeTypedTimeThunkFast;
  }
  return RejectContextThunk;
}

bool BridgeThunkHandler::IsTypedTimeThunkIdentity(
    const FEXCore::IR::SHA256Sum &sha256) const {
  const auto typed = DeriveTypedTimeThunkIdentity(runtime_->thunk_identity);
  return sha256 == typed;
}

FEXCore::HLE::ExecutableRangeInfo
BridgeSyscallHandler::QueryGuestExecutableRange(
    FEXCore::Core::InternalThreadState *, uint64_t address) {
  if (address >= runtime_->exit_sentinel &&
      address < runtime_->exit_sentinel + runtime_->page_size) {
    return {runtime_->exit_sentinel, runtime_->page_size, false, false};
  }
  const ExecutableRangeStore *store =
      runtime_->executable_ranges.load(std::memory_order_acquire);
  if (store == nullptr) {
    return {0, 0, false, false};
  }
  const auto it = std::upper_bound(
      store->ranges.begin(), store->ranges.end(), address,
      [](uint64_t value, const Lsx4FexExecutableRangeV5 &range) {
        return value < range.address;
      });
  if (it == store->ranges.begin()) {
    return {0, 0, false, false};
  }
  const auto &range = *std::prev(it);
  if (address < range.address || address >= range.address + range.size) {
    return {0, 0, false, false};
  }
  const bool writable = (range.range_flags & LSX4_FEX_RANGE_WRITABLE) != 0 ||
                        (range.protection & PROT_WRITE) != 0;
  const bool defer_pending_interrupt =
      (range.range_flags & LSX4_FEX_RANGE_HLE_TRAMPOLINE) != 0;
  return {range.address, range.size, writable, defer_pending_interrupt};
}

uint64_t
BridgeSyscallHandler::HandleSyscall(FEXCore::Core::CpuStateFrame *frame,
                                    FEXCore::HLE::SyscallArguments *) {
  ThreadRecord *record = CurrentRecord(runtime_, nullptr, true);
  if (frame == nullptr || record == nullptr ||
      frame != record->thread->CurrentFrame) {
    return 0;
  }
  const uint64_t syscall = frame->State.gregs[FEXCore::X86State::REG_RAX];
  if (syscall == kGuestExitSyscall) {
    record->guest_exit_requested = true;
    record->guest_exit_code = frame->State.gregs[FEXCore::X86State::REG_RDI];
    frame->State.rip = runtime_->exit_sentinel;
    return 0;
  }
  record->controlled_failure = true;
  record->failure_status = LSX4_FEX_STATUS_UNSUPPORTED;
  frame->State.rip = runtime_->exit_sentinel;
  Fatal(runtime_, LSX4_FEX_STATUS_UNSUPPORTED,
        "unknown guest syscall rejected by FEX bridge");
  return 0;
}

void RejectThunk(void *) {
  Runtime *runtime = g_thread_binding.runtime;
  ThreadRecord *record = g_thread_binding.record;
  ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_NOT_FOUND,
                         "unknown 0F3F thunk identity rejected");
}

FEXCore::ThunkControlResult
RejectContextThunk(void *, FEXCore::Core::CpuStateFrame *frame) {
  Runtime *runtime = g_thread_binding.runtime;
  ThreadRecord *record = g_thread_binding.record;
  if (record == nullptr || record->thread == nullptr ||
      frame != record->thread->CurrentFrame) {
    return FEXCore::ThunkControlResult::ResumeContext;
  }
  ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_NOT_FOUND,
                         "unknown 0F3F thunk identity rejected");
  return FEXCore::ThunkControlResult::ResumeContext;
}

bool CaptureMappedHleFramePools(
    const Lsx4FexMappedEntryRequestV5 &mapped,
    lsx4::fex2608::ResumeHleFramePools *output) noexcept {
  if (output == nullptr) {
    return false;
  }
  *output = {};
  constexpr uint32_t kForbiddenPoolFlags = LSX4_FEX_RANGE_EXECUTABLE |
                                           LSX4_FEX_RANGE_HLE_TRAMPOLINE |
                                           LSX4_FEX_RANGE_SELF_MODIFYING;
  for (uint32_t index = 0; index < mapped.segment_count; ++index) {
    const auto &segment = mapped.segments[index];
    if ((segment.range_flags & LSX4_FEX_RANGE_HLE_FRAME_POOL) == 0) {
      continue;
    }
    if (output->count >= output->values.size() ||
        segment.size != Lsx4::FexHleLease::PoolSize ||
        AddOverflows(segment.address, segment.size) ||
        (segment.address & (Lsx4::FexHleLease::SlotSize - 1)) != 0 ||
        (segment.protection & (PROT_READ | PROT_WRITE)) !=
            (PROT_READ | PROT_WRITE) ||
        (segment.protection & PROT_EXEC) != 0 ||
        (segment.range_flags & LSX4_FEX_RANGE_WRITABLE) == 0 ||
        (segment.range_flags & kForbiddenPoolFlags) != 0) {
      return false;
    }

    const Lsx4::FexHleLease::PoolGeometry candidate{
        .base = segment.address,
        .size = segment.size,
        .slot_size = Lsx4::FexHleLease::SlotSize,
        .frame_offset = Lsx4::FexHleLease::FrameOffset,
    };
    for (uint32_t prior = 0; prior < output->count; ++prior) {
      const auto &existing = output->values[prior];
      if (candidate.base < existing.base + existing.size &&
          existing.base < candidate.base + candidate.size) {
        return false;
      }
    }
    output->values[output->count++] = candidate;
  }
  return true;
}

bool SameHleFramePools(const lsx4::fex2608::ResumeHleFramePools &lhs,
                       const lsx4::fex2608::ResumeHleFramePools &rhs) noexcept {
  if (lhs.count != rhs.count || lhs.count > lhs.values.size() ||
      rhs.count > rhs.values.size()) {
    return false;
  }
  for (uint32_t index = 0; index < lhs.count; ++index) {
    const auto &left = lhs.values[index];
    const auto &right = rhs.values[index];
    if (left.base != right.base || left.size != right.size ||
        left.slot_size != right.slot_size ||
        left.frame_offset != right.frame_offset) {
      return false;
    }
  }
  return true;
}

bool PublishHleFramePools(
    Runtime *runtime,
    const lsx4::fex2608::ResumeHleFramePools &pools) noexcept {
  if (runtime == nullptr || pools.count > pools.values.size()) {
    return false;
  }
  if (pools.count == 0) {
    return true;
  }
  const auto *published =
      runtime->hle_frame_pools.load(std::memory_order_acquire);
  if (published != nullptr) {
    return SameHleFramePools(*published, pools);
  }

  auto *candidate =
      new (std::nothrow) lsx4::fex2608::ResumeHleFramePools{pools};
  if (candidate == nullptr) {
    return false;
  }
  if (runtime->hle_frame_pools.compare_exchange_strong(
          published, candidate, std::memory_order_release,
          std::memory_order_acquire)) {
    // Runtime instances are process-resident; the immutable pool publication
    // intentionally has the same lifetime as executable-range snapshots.
    return true;
  }
  delete candidate;
  return published != nullptr && SameHleFramePools(*published, pools);
}

bool IsExactPublishedHleStub(Runtime *runtime,
                             const uint64_t stub_begin) noexcept {
  if (runtime == nullptr || stub_begin == 0) {
    return false;
  }
  const ExecutableRangeStore *store =
      runtime->executable_ranges.load(std::memory_order_acquire);
  if (store == nullptr) {
    return false;
  }
  const auto found = std::upper_bound(
      store->ranges.begin(), store->ranges.end(), stub_begin,
      [](uint64_t value, const Lsx4FexExecutableRangeV5 &range) {
        return value < range.address;
      });
  if (found == store->ranges.begin()) {
    return false;
  }
  const auto &range = *std::prev(found);
  return range.address == stub_begin &&
         range.size == Lsx4::FexHleLease::StubCodeBytes &&
         (range.protection & PROT_EXEC) != 0 &&
         (range.range_flags &
          (LSX4_FEX_RANGE_EXECUTABLE | LSX4_FEX_RANGE_HLE_TRAMPOLINE)) ==
             (LSX4_FEX_RANGE_EXECUTABLE | LSX4_FEX_RANGE_HLE_TRAMPOLINE);
}

FEXCore::ThunkControlResult
ResumeBridgeThunk(Runtime *runtime, ThreadRecord *record, void *args) {
  if (runtime != nullptr && record != nullptr && args != nullptr) {
    const lsx4::fex2608::ResumeHleFramePools *pools = &record->hle_frame_pools;
    if (pools->count == 0) {
      const auto *runtime_pools =
          runtime->hle_frame_pools.load(std::memory_order_acquire);
      if (runtime_pools != nullptr) {
        pools = runtime_pools;
      }
    }
    const uint64_t frame_address = reinterpret_cast<uint64_t>(args);
    const uint64_t guest_rsp =
        record->thread != nullptr
            ? record->thread->CurrentFrame->State
                  .gregs[FEXCore::X86State::REG_RSP]
            : 0;
    // ResumeContext can continue a nested guest callback on the lower half of
    // the safe-thunk slot. Releasing that exact slot here lets another HLE
    // boundary reuse its stack while the original guest stack is still live.
    // The generated thunk remains the owner and releases the guard when the
    // callback unwinds back through it. Contexts that resume anywhere else
    // retain the existing fail-closed cleanup below.
    if (lsx4::fex2608::ResumeContextKeepsHleFrameSlotLive(
            *pools, frame_address, guest_rsp)) {
      return FEXCore::ThunkControlResult::ResumeContext;
    }
    const auto cleanup = lsx4::fex2608::CleanupResumeHleFrameLease(
        *pools, frame_address,
        [runtime](const uint64_t stub_begin) noexcept {
          return IsExactPublishedHleStub(runtime, stub_begin);
        });
    if (cleanup != lsx4::fex2608::ResumeHleLeaseCleanupResult::NotPoolFrame &&
        cleanup != lsx4::fex2608::ResumeHleLeaseCleanupResult::Released) {
      char message[384]{};
      std::snprintf(
          message, sizeof(message),
          "result=%u frame=0x%llx pools=%u "
          "pool0=[0x%llx,0x%llx] pool1=[0x%llx,0x%llx]",
          static_cast<unsigned>(cleanup),
          static_cast<unsigned long long>(frame_address),
          pools->count,
          static_cast<unsigned long long>(pools->values[0].base),
          static_cast<unsigned long long>(pools->values[0].size),
          static_cast<unsigned long long>(pools->values[1].base),
          static_cast<unsigned long long>(pools->values[1].size));
      Log(runtime, 6, "hle-resume-cleanup", message);
      ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
                             "HLE ResumeContext frame-lease cleanup rejected");
    }
  }
  return FEXCore::ThunkControlResult::ResumeContext;
}

std::array<uint64_t, LSX4_FEX_HLE_GPR_ARGUMENT_COUNT>
CaptureBridgeHleGprs(const uint64_t *words,
                     const FEXCore::Core::CpuStateFrame *) noexcept {
  // The generated SysV shim snapshots every guest argument before replacing
  // RDI with the sideband pointer used to enter the bridge.  That snapshot is
  // the single boundary authority: the architectural frame may already
  // contain helper-temporary values by the time the controlled thunk runs.
  return {
      words[2],
      words[3],
      words[4],
      words[5],
      words[6],
      words[7],
  };
}

__attribute__((noinline)) FEXCore::ThunkControlResult
BridgeThunkCommon(void *args, FEXCore::Core::CpuStateFrame *frame,
                  const bool integer_only) {
  Runtime *runtime = g_thread_binding.runtime;
  ThreadRecord *record = g_thread_binding.record;
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      frame == nullptr || frame != record->thread->CurrentFrame ||
      args == nullptr || runtime->callbacks.dispatch_hle == nullptr) {
    ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_INVALID_STATE,
                           "HLE thunk entered without an active bridge thread");
    return ResumeBridgeThunk(runtime, record, args);
  }

  if (record->nonlocal_pending) {
    return ResumeBridgeThunk(runtime, record, args);
  }

  struct GuestFpcrRefreshGuard {
    const ThreadRecord *record;
    bool refresh;
    ~GuestFpcrRefreshGuard() {
      if (refresh) {
        ApplyGuestFpcr(record);
      }
    }
  } fpcr_refresh_guard{record, !integer_only};

  ++g_thread_binding.thunk_depth;
  struct ThunkDepthGuard {
    ThreadRecord *record;
    uint32_t *depth;
    ~ThunkDepthGuard() {
      if (--*depth == 0) {
        record->nonlocal_pending = false;
      }
    }
  } thunk_depth_guard{record, &g_thread_binding.thunk_depth};

  const uint64_t entry_epoch =
      record->context_epoch.load(std::memory_order_acquire);
  auto *words = static_cast<uint64_t *>(args);
  const auto captured_gpr = CaptureBridgeHleGprs(words, frame);
  if (integer_only &&
      runtime->callbacks.try_dispatch_typed_time_hle != nullptr) {
    uint64_t direct_rax{};
    (void)PublishDeferredWriteEpochs(runtime, record);
    const int32_t direct_status =
        runtime->callbacks.try_dispatch_typed_time_hle(
            runtime->callbacks.callback_context, words[1], words[16], words[8],
            captured_gpr.data(), &direct_rax);
    // Exact time writes through a guest pointer. Keep the same pre/post epoch
    // publication and context-replacement gate as the full HLE ABI even though
    // its generic call/result frames are elided.
    (void)PublishDeferredWriteEpochs(runtime, record);
    if (direct_status == 1) {
      if (record->guest_exit_requested || record->controlled_failure) {
        frame->State.rip = runtime->exit_sentinel;
        return ResumeBridgeThunk(runtime, record, args);
      }
      if (record->context_epoch.load(std::memory_order_acquire) != entry_epoch) {
        return ResumeBridgeThunk(runtime, record, args);
      }
      words[0] = direct_rax;
      return FEXCore::ThunkControlResult::Continue;
    }
    if (direct_status != 0) {
      ForceCurrentToSentinel(
          runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
          "host typed-time HLE dispatch returned an invalid disposition");
      return ResumeBridgeThunk(runtime, record, args);
    }
  }
  if (runtime->callbacks.try_dispatch_leaf_hle != nullptr) {
    const uint64_t native_function = words[1];
    const size_t disposition_index =
        static_cast<size_t>(((native_function >> 4U) ^
                             (native_function >> 17U)) &
                            (record->leaf_hle_dispositions.size() - 1U));
    auto &disposition = record->leaf_hle_dispositions[disposition_index];
    if (disposition.native_function != native_function ||
        !disposition.rejected) {
      uint64_t direct_rax{};
      (void)PublishDeferredWriteEpochs(runtime, record);
      const int32_t direct_status =
          runtime->callbacks.try_dispatch_leaf_hle(
              runtime->callbacks.callback_context, native_function, words[16],
              words[8], captured_gpr.data(), &direct_rax);
      // Match the full HLE path: native leaf implementations can fault guest
      // backing pages, so publish their completed writes at the first safe
      // post-callback boundary.
      (void)PublishDeferredWriteEpochs(runtime, record);
      if (direct_status == 1) {
        disposition.native_function = native_function;
        disposition.rejected = false;
        if (record->guest_exit_requested || record->controlled_failure) {
          frame->State.rip = runtime->exit_sentinel;
          return ResumeBridgeThunk(runtime, record, args);
        }
        if (record->context_epoch.load(std::memory_order_acquire) !=
            entry_epoch) {
          return ResumeBridgeThunk(runtime, record, args);
        }
        words[0] = direct_rax;
        return FEXCore::ThunkControlResult::Continue;
      }
      if (direct_status == 0) {
        disposition.native_function = native_function;
        disposition.rejected = true;
      } else {
        ForceCurrentToSentinel(
            runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
            "host direct leaf HLE dispatch returned an invalid disposition");
        return ResumeBridgeThunk(runtime, record, args);
      }
    }
  }
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> lows{};
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> highs{};
  bool xmm_reconstructed = false;
  const auto reconstruct_xmm = [&] {
    if (!xmm_reconstructed) {
      runtime->context->ReconstructXMMRegisters(record->thread, lows.data(),
                                                highs.data());
      xmm_reconstructed = true;
    }
  };
  if (!integer_only &&
      runtime->callbacks.try_dispatch_scalar_fp_hle != nullptr) {
    const uint64_t native_function = words[1];
    const size_t disposition_index =
        static_cast<size_t>(((native_function >> 4U) ^
                             (native_function >> 17U)) &
                            (record->scalar_fp_hle_dispositions.size() - 1U));
    auto &disposition =
        record->scalar_fp_hle_dispositions[disposition_index];
    if (disposition.native_function != native_function ||
        !disposition.rejected) {
      reconstruct_xmm();
      std::array<Lsx4FexXmm128, 2> scalar_inputs{};
      std::memcpy(&scalar_inputs[0], &lows[0], sizeof(scalar_inputs[0]));
      std::memcpy(&scalar_inputs[1], &lows[1], sizeof(scalar_inputs[1]));
      Lsx4FexXmm128 direct_xmm0{};
      (void)PublishDeferredWriteEpochs(runtime, record);
      const int32_t direct_status =
          runtime->callbacks.try_dispatch_scalar_fp_hle(
              runtime->callbacks.callback_context, native_function, words[16],
              words[8], captured_gpr.data(), scalar_inputs.data(),
              &direct_xmm0);
      (void)PublishDeferredWriteEpochs(runtime, record);
      if (direct_status == 1) {
        disposition.native_function = native_function;
        disposition.rejected = false;
        if (record->guest_exit_requested || record->controlled_failure) {
          frame->State.rip = runtime->exit_sentinel;
          return ResumeBridgeThunk(runtime, record, args);
        }
        if (record->context_epoch.load(std::memory_order_acquire) !=
            entry_epoch) {
          return ResumeBridgeThunk(runtime, record, args);
        }
        // Match the generic scalar-SSE result contract exactly: integer
        // returns and XMM1 are zero, XMM0 carries the scalar value, and the
        // upper AVX halves retain their pre-call state.
        words[0] = 0;
        record->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RDX] =
            0;
        std::memcpy(&lows[0], &direct_xmm0, sizeof(direct_xmm0));
        lows[1] = 0;
        runtime->context->SetXMMRegistersFromState(record->thread, lows.data(),
                                                   highs.data());
        return FEXCore::ThunkControlResult::Continue;
      }
      if (direct_status == 0) {
        disposition.native_function = native_function;
        disposition.rejected = true;
      } else {
        ForceCurrentToSentinel(
            runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
            "host direct scalar-FP HLE dispatch returned an invalid disposition");
        return ResumeBridgeThunk(runtime, record, args);
      }
    }
  }
  Lsx4FexHleCallFrameV3 call{};
  call.struct_size = sizeof(call);
  call.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  call.call_id = runtime->next_call_id.fetch_add(1, std::memory_order_relaxed);
  call.native_function = words[1];
  call.guest_rip = words[16];
  call.guest_rsp = words[8];
  for (size_t index = 0; index < LSX4_FEX_HLE_GPR_ARGUMENT_COUNT; ++index) {
    call.gpr[index] = captured_gpr[index];
  }
  if (!integer_only) {
    reconstruct_xmm();
    for (size_t index = 0; index < LSX4_FEX_HLE_XMM_ARGUMENT_COUNT; ++index) {
      std::memcpy(&call.xmm[index], &lows[index], sizeof(call.xmm[index]));
    }
  }
  call.input_rax = words[0];

  Lsx4FexHleResultFrameV3 result{};
  result.struct_size = sizeof(result);
  result.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  result.call_id = call.call_id;
  (void)PublishDeferredWriteEpochs(runtime, record);
  const int32_t status = runtime->callbacks.dispatch_hle(
      runtime->callbacks.callback_context, &call, &result);
  // Guest backing-memory writes can fault inside the native HLE callback after
  // the entry publication above.  The callback return is the first proven
  // safe point after those stores have retired.  Publish here before any
  // result-contract exit so a long-running HLE call cannot leave its page
  // epoch Faulted until an unrelated dispatcher/JIT boundary.
  (void)PublishDeferredWriteEpochs(runtime, record);
  if (status != LSX4_FEX_STATUS_OK || result.struct_size < sizeof(result) ||
      result.abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      result.call_id != call.call_id || result.status != LSX4_FEX_STATUS_OK ||
      (result.result_flags & ~kAllHleResultFlags) != 0) {
    LogHleResultContractFailure(runtime, "common", status, call, result);
    ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
                           "host HLE dispatch failed its result contract");
    return ResumeBridgeThunk(runtime, record, args);
  }

  if ((result.result_flags & LSX4_FEX_HLE_RESULT_GUEST_EXIT) != 0) {
    record->guest_exit_requested = true;
    record->guest_exit_code = result.rax;
    frame->State.rip = runtime->exit_sentinel;
    return ResumeBridgeThunk(runtime, record, args);
  }
  if (record->guest_exit_requested || record->controlled_failure) {
    frame->State.rip = runtime->exit_sentinel;
    return ResumeBridgeThunk(runtime, record, args);
  }

  if (record->context_epoch.load(std::memory_order_acquire) != entry_epoch) {
    return ResumeBridgeThunk(runtime, record, args);
  }

  words[0] = result.rax;
  record->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RDX] =
      result.rdx;
  if (!integer_only) {
    std::memcpy(&lows[0], &result.xmm0, sizeof(result.xmm0));
    std::memcpy(&lows[1], &result.xmm1, sizeof(result.xmm1));
    runtime->context->SetXMMRegistersFromState(record->thread, lows.data(),
                                               highs.data());
  }
  return FEXCore::ThunkControlResult::Continue;
}

__attribute__((noinline)) FEXCore::ThunkControlResult
BridgeTypedTimeFallback(Runtime *runtime, ThreadRecord *record, void *args,
                        FEXCore::Core::CpuStateFrame *frame,
                        const uint64_t entry_epoch) {
  auto *words = static_cast<uint64_t *>(args);
  const auto captured_gpr = CaptureBridgeHleGprs(words, frame);
  if (runtime->callbacks.try_dispatch_leaf_hle != nullptr) {
    const uint64_t native_function = words[1];
    const size_t disposition_index =
        static_cast<size_t>(((native_function >> 4U) ^
                             (native_function >> 17U)) &
                            (record->leaf_hle_dispositions.size() - 1U));
    auto &disposition = record->leaf_hle_dispositions[disposition_index];
    if (disposition.native_function != native_function ||
        !disposition.rejected) {
      uint64_t direct_rax{};
      (void)PublishDeferredWriteEpochs(runtime, record);
      const int32_t direct_status =
          runtime->callbacks.try_dispatch_leaf_hle(
              runtime->callbacks.callback_context, native_function, words[16],
              words[8], captured_gpr.data(), &direct_rax);
      (void)PublishDeferredWriteEpochs(runtime, record);
      if (direct_status == 1) {
        disposition.native_function = native_function;
        disposition.rejected = false;
        if (record->guest_exit_requested || record->controlled_failure) {
          frame->State.rip = runtime->exit_sentinel;
          return ResumeBridgeThunk(runtime, record, args);
        }
        if (record->context_epoch.load(std::memory_order_acquire) !=
            entry_epoch) {
          return ResumeBridgeThunk(runtime, record, args);
        }
        words[0] = direct_rax;
        return FEXCore::ThunkControlResult::Continue;
      }
      if (direct_status == 0) {
        disposition.native_function = native_function;
        disposition.rejected = true;
      } else {
        ForceCurrentToSentinel(
            runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
            "host direct leaf HLE dispatch returned an invalid disposition");
        return ResumeBridgeThunk(runtime, record, args);
      }
    }
  }

  // This fallback is reached only by the exact integer-only identity. Its
  // generic ABI still publishes zero XMM inputs and ignores XMM results, but
  // the large call/result frames remain fully validated on this rare path.
  Lsx4FexHleCallFrameV3 call{};
  call.struct_size = sizeof(call);
  call.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  call.call_id = runtime->next_call_id.fetch_add(1, std::memory_order_relaxed);
  call.native_function = words[1];
  call.guest_rip = words[16];
  call.guest_rsp = words[8];
  for (size_t index = 0; index < LSX4_FEX_HLE_GPR_ARGUMENT_COUNT; ++index) {
    call.gpr[index] = captured_gpr[index];
  }
  call.input_rax = words[0];

  Lsx4FexHleResultFrameV3 result{};
  result.struct_size = sizeof(result);
  result.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  result.call_id = call.call_id;
  (void)PublishDeferredWriteEpochs(runtime, record);
  const int32_t status = runtime->callbacks.dispatch_hle(
      runtime->callbacks.callback_context, &call, &result);
  (void)PublishDeferredWriteEpochs(runtime, record);
  if (status != LSX4_FEX_STATUS_OK || result.struct_size < sizeof(result) ||
      result.abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      result.call_id != call.call_id || result.status != LSX4_FEX_STATUS_OK ||
      (result.result_flags & ~kAllHleResultFlags) != 0) {
    LogHleResultContractFailure(runtime, "typed-time-fallback", status, call,
                                result);
    ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
                           "host HLE dispatch failed its result contract");
    return ResumeBridgeThunk(runtime, record, args);
  }

  if ((result.result_flags & LSX4_FEX_HLE_RESULT_GUEST_EXIT) != 0) {
    record->guest_exit_requested = true;
    record->guest_exit_code = result.rax;
    frame->State.rip = runtime->exit_sentinel;
    return ResumeBridgeThunk(runtime, record, args);
  }
  if (record->guest_exit_requested || record->controlled_failure) {
    frame->State.rip = runtime->exit_sentinel;
    return ResumeBridgeThunk(runtime, record, args);
  }
  if (record->context_epoch.load(std::memory_order_acquire) != entry_epoch) {
    return ResumeBridgeThunk(runtime, record, args);
  }

  words[0] = result.rax;
  record->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RDX] =
      result.rdx;
  return FEXCore::ThunkControlResult::Continue;
}

__attribute__((noinline)) FEXCore::ThunkControlResult
BridgeTypedTimeThunkFast(void *args,
                         FEXCore::Core::CpuStateFrame *frame) {
  Runtime *runtime = g_thread_binding.runtime;
  ThreadRecord *record = g_thread_binding.record;
  if (runtime == nullptr || record == nullptr || record->thread == nullptr ||
      frame == nullptr || frame != record->thread->CurrentFrame ||
      args == nullptr || runtime->callbacks.dispatch_hle == nullptr) {
    ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_INVALID_STATE,
                           "HLE thunk entered without an active bridge thread");
    return ResumeBridgeThunk(runtime, record, args);
  }
  if (record->nonlocal_pending) {
    return ResumeBridgeThunk(runtime, record, args);
  }

  ++g_thread_binding.thunk_depth;
  struct ThunkDepthGuard {
    ThreadRecord *record;
    uint32_t *depth;
    ~ThunkDepthGuard() {
      if (--*depth == 0) {
        record->nonlocal_pending = false;
      }
    }
  } thunk_depth_guard{record, &g_thread_binding.thunk_depth};

  const uint64_t entry_epoch =
      record->context_epoch.load(std::memory_order_acquire);
  auto *words = static_cast<uint64_t *>(args);
  const auto captured_gpr = CaptureBridgeHleGprs(words, frame);
  if (runtime->callbacks.try_dispatch_typed_time_hle != nullptr) {
    uint64_t direct_rax{};
    (void)PublishDeferredWriteEpochs(runtime, record);
    const int32_t direct_status =
        runtime->callbacks.try_dispatch_typed_time_hle(
            runtime->callbacks.callback_context, words[1], words[16], words[8],
            captured_gpr.data(), &direct_rax);
    (void)PublishDeferredWriteEpochs(runtime, record);
    if (direct_status == 1) {
      if (record->guest_exit_requested || record->controlled_failure) {
        frame->State.rip = runtime->exit_sentinel;
        return ResumeBridgeThunk(runtime, record, args);
      }
      if (record->context_epoch.load(std::memory_order_acquire) != entry_epoch) {
        return ResumeBridgeThunk(runtime, record, args);
      }
      words[0] = direct_rax;
      return FEXCore::ThunkControlResult::Continue;
    }
    if (direct_status != 0) {
      ForceCurrentToSentinel(
          runtime, record, LSX4_FEX_STATUS_BACKEND_FAILURE,
          "host typed-time HLE dispatch returned an invalid disposition");
      return ResumeBridgeThunk(runtime, record, args);
    }
  }
  return BridgeTypedTimeFallback(runtime, record, args, frame, entry_epoch);
}

FEXCore::ThunkControlResult BridgeThunk(
    void *args, FEXCore::Core::CpuStateFrame *frame) {
  return BridgeThunkCommon(args, frame, false);
}

FEXCore::ThunkControlResult BridgeTypedTimeThunkLegacy(
    void *args, FEXCore::Core::CpuStateFrame *frame) {
  return BridgeThunkCommon(args, frame, true);
}

bool ValidateMappedRequest(Runtime *runtime,
                           const Lsx4FexMappedEntryRequestV5 *request) {
  if (runtime == nullptr || request == nullptr ||
      request->struct_size < sizeof(*request) ||
      request->abi_version != LSX4_FEX_MAPPED_ENTRY_ABI_V5 ||
      request->request_flags != 0 || request->timeout_ms != 0 ||
      request->reserved != 0 || request->entry == 0 ||
      request->stack_base == 0 || request->stack_size < 0x4000 ||
      AddOverflows(request->stack_base, request->stack_size) ||
      (request->segment_count != 0 && request->segments == nullptr)) {
    return false;
  }
  if (!IsExecutableAddress(runtime, request->entry, 1) ||
      (request->exit_function != 0 &&
       !IsExecutableAddress(runtime, request->exit_function, 1)) ||
      !SegmentContains(*request, request->stack_base, request->stack_size,
                       PROT_READ | PROT_WRITE)) {
    return false;
  }
  return request->hle_trampoline_size == 0 ||
         IsExecutableAddress(runtime, request->hle_trampoline_base,
                             request->hle_trampoline_size);
}

bool BuildProcessEntryStack(Runtime *runtime, ThreadRecord *record,
                            const Lsx4FexMappedEntryRequestV5 &request,
                            uint64_t *guest_rsp) {
  const uint64_t top =
      (request.stack_base + request.stack_size) & ~UINT64_C(15);
  if (top < request.stack_base + 32) {
    return false;
  }
  uint64_t rsp = top - sizeof(uint64_t);
  if (request.entry_params != 0 &&
      SegmentContains(request, request.entry_params, 2 * sizeof(uint64_t),
                      PROT_READ)) {
    std::array<uint64_t, 2> parameters{};
    std::memcpy(parameters.data(),
                reinterpret_cast<const void *>(request.entry_params),
                sizeof(parameters));
    rsp = rsp + sizeof(uint64_t) - 3 * sizeof(uint64_t);
    std::memcpy(reinterpret_cast<void *>(rsp), parameters.data(),
                sizeof(parameters));
    const uint64_t terminator = 0;
    std::memcpy(reinterpret_cast<void *>(rsp + sizeof(parameters)), &terminator,
                sizeof(terminator));
  } else {
    const uint64_t sentinel = runtime->exit_sentinel;
    std::memcpy(reinterpret_cast<void *>(rsp), &sentinel, sizeof(sentinel));
  }
  record->stack_base = request.stack_base;
  record->stack_size = request.stack_size;
  *guest_rsp = rsp;
  return true;
}

bool UnblockBridgeWakeSignal(ThreadRecord *record) {
  if (record == nullptr) {
    return false;
  }
  sigset_t wake{};
  sigset_t previous{};
  sigemptyset(&wake);
  sigaddset(&wake, kBridgeWakeSignal);
  if (pthread_sigmask(SIG_UNBLOCK, &wake, &previous) != 0) {
    return false;
  }
  record->wake_signal_was_blocked =
      sigismember(&previous, kBridgeWakeSignal) == 1;
  return true;
}

void RestoreBridgeWakeSignalMask(ThreadRecord *record) {
  if (record == nullptr || !record->wake_signal_was_blocked) {
    return;
  }
  sigset_t wake{};
  sigemptyset(&wake);
  sigaddset(&wake, kBridgeWakeSignal);
  (void)pthread_sigmask(SIG_BLOCK, &wake, nullptr);
  record->wake_signal_was_blocked = false;
}

int32_t BindHostThread(void *opaque, const Lsx4FexThreadIdentityV3 *requested) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || !IsThreadIdentityHeaderValid(requested) ||
      g_thread_binding.runtime != nullptr) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const uint64_t tid = CurrentTid();
  if (requested->host_tid != 0 &&
      requested->host_tid != static_cast<int64_t>(tid)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  if (runtime->hardware_tso_bind_failed.load(std::memory_order_acquire)) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  if (runtime->strong_memory_mode == LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO) {
    const HardwareTsoAttempt attempt = TryEnableHardwareTsoForCurrentThread();
    const uint64_t sequence = attempt.enabled
                                  ? runtime->hardware_tso_bind_count.fetch_add(
                                        1, std::memory_order_relaxed) +
                                        1
                                  : 0;
    // Successful binds are sampled to avoid turning callback re-entry into a
    // logcat hot path.  Failures are always reported and always fail closed.
    if (!attempt.enabled || sequence <= 32 ||
        (sequence & (sequence - 1)) == 0) {
      LogHardwareTsoAttempt(&runtime->callbacks, "bind", attempt, sequence);
    }
    if (!attempt.enabled) {
      // Fail before allocating or publishing a ThreadRecord.  The context was
      // compiled without software ordering, so a mixed-mode bind is unsafe.
      runtime->hardware_tso_bind_failed.store(true, std::memory_order_release);
      if (runtime->callbacks.fatal_error != nullptr) {
        runtime->callbacks.fatal_error(runtime->callbacks.callback_context,
                                       LSX4_FEX_STATUS_BACKEND_FAILURE,
                                       kHardwareTsoBindFailed);
      }
      return LSX4_FEX_STATUS_BACKEND_FAILURE;
    }
  }

  std::lock_guard lock{runtime->records_mutex};
  ThreadRecord *record{};
  for (auto *candidate : runtime->records) {
    if (candidate->finalized.load(std::memory_order_acquire)) {
      continue;
    }
    if ((requested->backend_thread != 0 &&
         candidate->identity.backend_thread == requested->backend_thread) ||
        (requested->backend_thread == 0 && requested->guest_thread != 0 &&
         candidate->identity.guest_thread == requested->guest_thread)) {
      record = candidate;
      break;
    }
  }
  if (record == nullptr) {
    record = new (std::nothrow) ThreadRecord{};
    if (record == nullptr) {
      return LSX4_FEX_STATUS_BACKEND_FAILURE;
    }
    record->host_callbacks = &runtime->callbacks;
    record->identity.struct_size = sizeof(Lsx4FexThreadIdentityV3);
    record->identity.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    record->tls.struct_size = sizeof(Lsx4FexGuestTlsStateV3);
    record->tls.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    record->identity.guest_thread = requested->guest_thread;
    runtime->records.push_back(record);
  }
  if (record->host_callbacks != &runtime->callbacks) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  std::lock_guard record_lock{record->mutex};
  if (record->active.load(std::memory_order_acquire) ||
      record->bound_tid.load(std::memory_order_acquire) != 0 ||
      !DeferredSignalStateIsQuiescent(record)) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  record->bound_tid.store(tid, std::memory_order_release);
  record->identity.pthread_handle = CurrentPthreadHandle();
  record->identity.host_tid = static_cast<int64_t>(tid);
  if (!CreatePersistentThread(runtime, record)) {
    record->bound_tid.store(0, std::memory_order_release);
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  if (!DisarmGuestSignalInterrupt(record)) {
    record->bound_tid.store(0, std::memory_order_release);
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  record->tls.target = record->identity;
  g_thread_binding = {runtime, record};
  FEXCore::Allocator::RegisterTLSData(record->thread);
  if (!PublishSignalBinding(tid, runtime, record)) {
    FEXCore::Allocator::UninstallTLSData(record->thread);
    g_thread_binding = {};
    record->bound_tid.store(0, std::memory_order_release);
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  if (!UnblockBridgeWakeSignal(record)) {
    RemoveSignalBinding(tid, runtime, record);
    FEXCore::Allocator::UninstallTLSData(record->thread);
    g_thread_binding = {};
    record->bound_tid.store(0, std::memory_order_release);
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  if (const char *retire_vs_bus =
          std::getenv("LSX4_FEX2608_TEST_RETIRE_VS_BUS");
      retire_vs_bus != nullptr && retire_vs_bus[0] == '1' &&
      !RunRetireVsBusAdversarialTest(record)) {
    RemoveSignalBinding(tid, runtime, record);
    FEXCore::Allocator::UninstallTLSData(record->thread);
    g_thread_binding = {};
    record->bound_tid.store(0, std::memory_order_release);
    RestoreBridgeWakeSignalMask(record);
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t UnbindHostThread(void *opaque) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *record = CurrentRecord(runtime, nullptr, false);
  if (record == nullptr) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  std::lock_guard lock{record->mutex};
  if (record->active.load(std::memory_order_acquire) ||
      !DeferredSignalStateIsQuiescent(record)) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  if (!DisarmGuestSignalInterrupt(record)) {
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  RemoveSignalBinding(CurrentTid(), runtime, record);
  FEXCore::Allocator::UninstallTLSData(record->thread);
  RestoreBridgeWakeSignalMask(record);
  record->bound_tid.store(0, std::memory_order_release);
  g_thread_binding = {};
  return LSX4_FEX_STATUS_OK;
}

bool ReleaseIdentityMatches(const ThreadRecord *record,
                            const Lsx4FexThreadIdentityV3 &requested) {
  if (record == nullptr || requested.guest_thread == 0 ||
      record->identity.guest_thread != requested.guest_thread) {
    return false;
  }
  if (requested.backend_thread != 0 &&
      record->identity.backend_thread != requested.backend_thread) {
    return false;
  }
  if (requested.pthread_handle != 0 &&
      record->identity.pthread_handle != requested.pthread_handle) {
    return false;
  }
  return requested.host_tid == 0 ||
         record->identity.host_tid == requested.host_tid;
}

int32_t ReleaseGuestThread(void *opaque,
                           const Lsx4FexThreadIdentityV3 *requested) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || !IsThreadIdentityHeaderValid(requested) ||
      requested->guest_thread == 0) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }

  std::lock_guard records_lock{runtime->records_mutex};
  ThreadRecord *record{};
  for (auto it = runtime->records.rbegin(); it != runtime->records.rend();
       ++it) {
    if (ReleaseIdentityMatches(*it, *requested)) {
      record = *it;
      break;
    }
  }
  if (record == nullptr) {
#if defined(__APPLE__)
    // Funnel also finalizes native HLE workers which never entered FEX.
    // They own no adapter resource. Accept only the host-finalizer identity;
    // a bound caller, stale guest identity, or live pthread alias still fails.
    bool native_only = requested->backend_thread == 0 &&
                       requested->host_tid == 0 &&
                       requested->pthread_handle != 0 &&
                       g_thread_binding.runtime == nullptr;
    for (const auto* candidate : runtime->records) {
      // Pthread storage and native handles may be recycled after successful
      // finalization. A tombstone is harmless only after all owned resources
      // are gone; a half-finished retirement must still retain the identity.
      const bool retired = candidate->finalized.load(std::memory_order_acquire) &&
                           candidate->thread == nullptr &&
                           candidate->callret_allocation == nullptr &&
                           candidate->scratch_allocation == nullptr &&
                           candidate->deferred_stack_allocation == nullptr &&
                           DeferredSignalStateIsQuiescent(candidate);
      if (!retired &&
          (candidate->identity.guest_thread == requested->guest_thread ||
           candidate->identity.pthread_handle == requested->pthread_handle)) {
        native_only = false;
        break;
      }
    }
    if (native_only) {
      static std::atomic<unsigned> native_release_logs{};
      if (native_release_logs.load(std::memory_order_relaxed) < 4 &&
          native_release_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
        std::fprintf(stderr, "[APPLE_FEX_THREAD_RELEASE] native_only=1 "
                     "guest=0x%llx pthread=0x%llx\n",
                     static_cast<unsigned long long>(requested->guest_thread),
                     static_cast<unsigned long long>(requested->pthread_handle));
      }
      return LSX4_FEX_STATUS_OK;
    }
#endif
    return LSX4_FEX_STATUS_INVALID_STATE;
  }

  std::lock_guard record_lock{record->mutex};
  if (record->finalized.load(std::memory_order_acquire)) {
    return LSX4_FEX_STATUS_OK;
  }
  if (record->active.load(std::memory_order_acquire) ||
      record->bound_tid.load(std::memory_order_acquire) != 0 ||
      !DeferredSignalStateIsQuiescent(record) ||
      g_thread_binding.record == record) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  if (!DisarmGuestSignalInterrupt(record)) {
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }

  // Publish the tombstone before destroying FEX state. Signal-target lookups
  // then stop matching this record, while the process-resident ThreadRecord
  // itself prevents dangling-pointer races with a lookup that just completed.
  record->finalized.store(true, std::memory_order_release);
  if (runtime->callbacks.thread_event != nullptr) {
    runtime->callbacks.thread_event(runtime->callbacks.callback_context,
                                    LSX4_FEX2608_THREAD_EVENT_DESTROYING,
                                    &record->identity);
  }
  if (record->thread != nullptr) {
    runtime->context->DestroyThread(record->thread);
    record->thread = nullptr;
  }
  if (!ReleaseThreadMappings(record)) {
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  {
    std::lock_guard signal_lock{record->signal_mutex};
    record->signal_queue = {};
    record->signal_queue_read = 0;
    record->signal_queue_write = 0;
    record->signal_drain_active = false;
  }
  record->tls_configured = false;
  record->guest_exit_requested = false;
  record->guest_exit_code = 0;
  record->controlled_failure = false;
  record->failure_status = LSX4_FEX_STATUS_OK;
  record->nonlocal_pending = false;
  record->hle_frame_pools = {};
  return LSX4_FEX_STATUS_OK;
}

int32_t GetGuestContext(void *opaque, const Lsx4FexThreadIdentityV3 *thread,
                        Lsx4FexGuestContextV3 *context) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *record = CurrentRecord(runtime, thread, true);
  return record != nullptr ? ExportContext(runtime, record, context)
                           : LSX4_FEX_STATUS_INVALID_STATE;
}

int32_t SetGuestContext(void *opaque, const Lsx4FexThreadIdentityV3 *thread,
                        const Lsx4FexGuestContextV3 *context) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *record = CurrentRecord(runtime, thread, true);
  if (record == nullptr) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  if (g_thread_binding.thunk_depth > 1) {
    ForceCurrentToSentinel(runtime, record, LSX4_FEX_STATUS_INVALID_STATE,
                           "nested HLE context replacement rejected");
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  const int32_t status = ImportContext(runtime, record, context, true);
  if (status == LSX4_FEX_STATUS_OK) {
    ResetCallRetShadowStack(record);
    record->context_epoch.fetch_add(1, std::memory_order_release);
    if (g_thread_binding.thunk_depth == 1) {
      record->nonlocal_pending = true;
    }
  }
  return status;
}

int32_t GetGuestTls(void *opaque, const Lsx4FexThreadIdentityV3 *thread,
                    Lsx4FexGuestTlsStateV3 *output) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *record = CurrentRecord(runtime, thread, false);
  if (record == nullptr || output == nullptr ||
      output->struct_size < sizeof(*output) ||
      output->abi_version != LSX4_FEX_BACKEND_ABI_V3) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const uint32_t capacity = output->struct_size;
  *output = record->tls;
  output->struct_size = capacity;
  output->abi_version = LSX4_FEX_BACKEND_ABI_V3;
  output->target = record->identity;
  if (record->thread != nullptr) {
    output->fs_base = record->thread->CurrentFrame->State.fs_cached;
    output->gs_base = record->thread->CurrentFrame->State.gs_cached;
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t SetGuestTls(void *opaque, const Lsx4FexGuestTlsStateV3 *input) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (input == nullptr || input->struct_size < sizeof(*input) ||
      input->abi_version != LSX4_FEX_BACKEND_ABI_V3 || input->reserved != 0) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  ThreadRecord *record = CurrentRecord(runtime, &input->target, false);
  if (record == nullptr) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  record->tls = *input;
  record->tls.target = record->identity;
  record->tls_configured = true;
  if (record->thread != nullptr) {
    record->thread->CurrentFrame->State.fs_cached = input->fs_base;
    record->thread->CurrentFrame->State.gs_cached = input->gs_base;
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t RunGuestCallback(void *opaque,
                         const Lsx4FexGuestCallbackRequestV3 *request,
                         Lsx4FexGuestCallbackResultV3 *result) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *record = CurrentRecord(runtime, nullptr, true);
  if (record == nullptr || request == nullptr || result == nullptr ||
      request->struct_size < sizeof(*request) ||
      request->abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      request->guest_function == 0 ||
      !IsExecutableAddress(runtime, request->guest_function, 1)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  if (record->nonlocal_pending) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  const uint64_t entry_epoch =
      record->context_epoch.load(std::memory_order_acquire);

  bool owns_activity = false;
  bool expected_inactive = false;
  if (record->active.compare_exchange_strong(expected_inactive, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    owns_activity = true;
    record->guest_exit_requested = false;
    record->guest_exit_code = 0;
    record->controlled_failure = false;
    record->failure_status = LSX4_FEX_STATUS_OK;
    lsx4::fex2608::ResetSignalCaptureGate(&record->host_signal.gate);
    runtime->active_threads.fetch_add(1, std::memory_order_relaxed);
  }
  struct ActivityGuard {
    Runtime *runtime;
    ThreadRecord *record;
    bool owned;
    ~ActivityGuard() {
      if (owned) {
        (void)DisarmGuestSignalInterrupt(record);
        record->active.store(false, std::memory_order_release);
        runtime->active_threads.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  } activity_guard{runtime, record, owns_activity};

  const uint32_t diagnostic_sequence =
      runtime->callback_diagnostic_sequence.fetch_add(
          1, std::memory_order_relaxed);
  LogCallbackCheckpoint(runtime, record, diagnostic_sequence, "enter",
                        request->guest_function);

  auto &state = record->thread->CurrentFrame->State;
  const auto complete_guest_exit = [&]() {
    const uint32_t capacity = result->struct_size;
    std::memset(result, 0, sizeof(*result));
    result->struct_size = capacity;
    result->abi_version = LSX4_FEX_BACKEND_ABI_V3;
    result->call_id = request->callback_id;
    result->rax = record->guest_exit_code;
    result->rdx = state.gregs[FEXCore::X86State::REG_RDX];
    result->sret_address = request->sret_address;
    result->sret_size = request->sret_size;
    result->return_class[0] = request->return_class[0];
    result->return_class[1] = request->return_class[1];
    result->return_eightbyte_count = request->return_eightbyte_count;
    result->sret_flags = request->sret_flags;
    result->result_flags = LSX4_FEX_HLE_RESULT_GUEST_EXIT;
    result->abi_descriptor_id = request->abi_descriptor_id;
    result->status = LSX4_FEX_STATUS_OK;
    return LSX4_FEX_STATUS_OK;
  };
  if (record->controlled_failure) {
    return record->failure_status;
  }
  if (record->guest_exit_requested) {
    return complete_guest_exit();
  }
  int32_t signal_status = ProcessQueuedSignals(runtime, record);
  if (signal_status != LSX4_FEX_STATUS_OK) {
    return signal_status;
  }
  if (record->controlled_failure) {
    return record->failure_status;
  }
  if (record->guest_exit_requested) {
    return complete_guest_exit();
  }
  FEXCore::Core::CPUState nested_prior_state{};
  if (!owns_activity) {
    std::memcpy(&nested_prior_state, &state, sizeof(nested_prior_state));
  }
  struct NestedGuestStateRestore {
    ThreadRecord *record;
    uint64_t entry_epoch;
    const FEXCore::Core::CPUState *prior_state;
    bool enabled;

    ~NestedGuestStateRestore() {
      if (!enabled || record == nullptr || record->thread == nullptr ||
          record->nonlocal_pending ||
          record->context_epoch.load(std::memory_order_acquire) !=
              entry_epoch) {
        return;
      }
      /*
       * HandleCallback is re-entrant, but it executes against the same
       * CpuStateFrame that the suspended controlled thunk will refill from on
       * Continue.  A completed nested callback therefore must not leave its
       * RIP, register file, flags, vector state, TLS bases, deferred-signal
       * depth or call/return shadow-stack pointer in that outer frame.  The
       * callback result is copied out before this transaction is destroyed.
       * Terminal/failure state lives on ThreadRecord and deliberately survives
       * this CPUState restore, so the suspended outer thunk can land on the
       * exit sentinel without inheriting the nested callback's register file.
       *
       * A context replacement owns the new frame and advances context_epoch;
       * never overwrite that authoritative non-local state.
       */
      std::memcpy(&record->thread->CurrentFrame->State, prior_state,
                  sizeof(*prior_state));
    }
  } nested_guest_state_restore{record, entry_epoch, &nested_prior_state,
                               !owns_activity};
  if (!owns_activity && request->guest_rsp == 0 && record->stack_size != 0) {
    const lsx4::fex2608::ResumeHleFramePools *pools =
        &record->hle_frame_pools;
    if (pools->count == 0) {
      const auto *runtime_pools =
          runtime->hle_frame_pools.load(std::memory_order_acquire);
      if (runtime_pools != nullptr) {
        pools = runtime_pools;
      }
    }
    std::uint64_t callback_rsp{};
    const auto stack_result =
        lsx4::fex2608::ResolveNestedHleCallbackStack(
            *pools, state.gregs[FEXCore::X86State::REG_RDI],
            record->stack_base, record->stack_size,
            [runtime](const std::uint64_t stub_begin) noexcept {
              return IsExactPublishedHleStub(runtime, stub_begin);
            },
            &callback_rsp);
    if (stack_result ==
        lsx4::fex2608::NestedHleCallbackStackResult::Resolved) {
      state.gregs[FEXCore::X86State::REG_RSP] = callback_rsp;
    } else if (stack_result !=
               lsx4::fex2608::NestedHleCallbackStackResult::NotPoolFrame) {
      char message[384]{};
      std::snprintf(
          message, sizeof(message),
          "result=%u frame=0x%llx currentRsp=0x%llx stack=[0x%llx,0x%llx]",
          static_cast<unsigned>(stack_result),
          static_cast<unsigned long long>(
              state.gregs[FEXCore::X86State::REG_RDI]),
          static_cast<unsigned long long>(
              state.gregs[FEXCore::X86State::REG_RSP]),
          static_cast<unsigned long long>(record->stack_base),
          static_cast<unsigned long long>(record->stack_size));
      Log(runtime, 6, "hle-callback-stack", message);
      return LSX4_FEX_STATUS_INVALID_STATE;
    }
  }
  if (request->guest_rsp != 0) {
    /*
     * HandleCallback consumes 16 bytes below an explicit RSP. Mapped-entry
     * records have authoritative stack geometry, so reject an interval that
     * escapes it before FEX writes the callback return landing. Callback-only
     * hosts (including the PS5 bridge) validate their own guest mapping and do
     * not populate record stack bounds; preserve that V3 contract here.
     */
    if (record->stack_size != 0 &&
        (request->guest_rsp < record->stack_base ||
         request->guest_rsp - record->stack_base < 16 ||
         request->guest_rsp - record->stack_base > record->stack_size)) {
      return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    state.gregs[FEXCore::X86State::REG_RSP] = request->guest_rsp;
  } else if (owns_activity) {
    uint64_t rsp =
        (record->scratch_base + record->scratch_size) & ~UINT64_C(15);
    rsp -= sizeof(uint64_t);
    const uint64_t sentinel = runtime->exit_sentinel;
    std::memcpy(reinterpret_cast<void *>(rsp), &sentinel, sizeof(sentinel));
    state.gregs[FEXCore::X86State::REG_RSP] = rsp;
  }
  constexpr std::array<unsigned, LSX4_FEX_HLE_GPR_ARGUMENT_COUNT> registers{
      FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI,
      FEXCore::X86State::REG_RDX, FEXCore::X86State::REG_RCX,
      FEXCore::X86State::REG_R8,  FEXCore::X86State::REG_R9,
  };
  for (size_t index = 0; index < registers.size(); ++index) {
    state.gregs[registers[index]] = request->gpr[index];
  }
  state.gregs[FEXCore::X86State::REG_RAX] = request->input_rax;
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> lows{};
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> highs{};
  runtime->context->ReconstructXMMRegisters(record->thread, lows.data(),
                                            highs.data());
  for (size_t index = 0; index < LSX4_FEX_HLE_XMM_ARGUMENT_COUNT; ++index) {
    std::memcpy(&lows[index], &request->xmm[index],
                sizeof(request->xmm[index]));
  }
  runtime->context->SetXMMRegistersFromState(record->thread, lows.data(),
                                             highs.data());
  LogCallbackCheckpoint(runtime, record, diagnostic_sequence, "before-handle",
                        request->guest_function);
  signal_status = ExecuteGuestCallbackWithSignalResume(runtime, record,
                                                       request->guest_function);
  LogCallbackCheckpoint(runtime, record, diagnostic_sequence, "after-handle",
                        request->guest_function);
  if (signal_status != LSX4_FEX_STATUS_OK) {
    return signal_status;
  }
  if (!record->guest_exit_requested && !record->controlled_failure) {
    signal_status = ProcessQueuedSignals(runtime, record);
    if (signal_status != LSX4_FEX_STATUS_OK) {
      return signal_status;
    }
  }
  if (record->controlled_failure) {
    return record->failure_status;
  }
  if (record->nonlocal_pending ||
      record->context_epoch.load(std::memory_order_acquire) != entry_epoch) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  runtime->context->ReconstructXMMRegisters(record->thread, lows.data(),
                                            highs.data());

  const uint32_t capacity = result->struct_size;
  std::memset(result, 0, sizeof(*result));
  result->struct_size = capacity;
  result->abi_version = LSX4_FEX_BACKEND_ABI_V3;
  result->call_id = request->callback_id;
  result->rax = state.gregs[FEXCore::X86State::REG_RAX];
  if (record->guest_exit_requested) {
    result->rax = record->guest_exit_code;
  }
  result->rdx = state.gregs[FEXCore::X86State::REG_RDX];
  std::memcpy(&result->xmm0, &lows[0], sizeof(result->xmm0));
  std::memcpy(&result->xmm1, &lows[1], sizeof(result->xmm1));
  result->sret_address = request->sret_address;
  result->sret_size = request->sret_size;
  result->return_class[0] = request->return_class[0];
  result->return_class[1] = request->return_class[1];
  result->return_eightbyte_count = request->return_eightbyte_count;
  result->sret_flags = request->sret_flags;
  if (record->guest_exit_requested) {
    result->result_flags = LSX4_FEX_HLE_RESULT_GUEST_EXIT;
  }
  result->abi_descriptor_id = request->abi_descriptor_id;
  result->status = LSX4_FEX_STATUS_OK;
  return LSX4_FEX_STATUS_OK;
}

int32_t RunMappedEntry(void *opaque,
                       const Lsx4Fex2608RunMappedEntryRequestV2 *request,
                       Lsx4Fex2608RunMappedEntryResultV2 *result) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *record = CurrentRecord(runtime, nullptr, false);
  if (runtime == nullptr || record == nullptr || request == nullptr ||
      result == nullptr || request->struct_size < sizeof(*request) ||
      request->abi_version != LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2 ||
      request->run_flags != 0 || request->reserved != 0 ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  int32_t status = RefreshRanges(runtime);
  if (status == LSX4_FEX_STATUS_OK && request->mapped_entry != nullptr &&
      request->mapped_entry->executable_ranges != nullptr) {
    status =
        ValidateRangeSubset(runtime, request->mapped_entry->executable_ranges);
  }
  if (status != LSX4_FEX_STATUS_OK ||
      !ValidateMappedRequest(runtime, request->mapped_entry)) {
    return status != LSX4_FEX_STATUS_OK ? status
                                        : LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const auto &mapped = *request->mapped_entry;
  lsx4::fex2608::ResumeHleFramePools mapped_hle_frame_pools{};
  if (!CaptureMappedHleFramePools(mapped, &mapped_hle_frame_pools) ||
      !PublishHleFramePools(runtime, mapped_hle_frame_pools)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  if (request->initial_context != nullptr &&
      (request->initial_context->struct_size < sizeof(Lsx4FexGuestContextV3) ||
       request->initial_context->abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
       (request->initial_context->context_flags & ~kAllContextFlags) != 0 ||
       (request->initial_context->rip != 0 &&
        request->initial_context->rip != mapped.entry))) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }

  std::lock_guard record_lock{record->mutex};
  if (record->active.load(std::memory_order_acquire) ||
      record->thread == nullptr ||
      record->bound_tid.load(std::memory_order_acquire) != CurrentTid()) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }

  uint64_t guest_rsp{};
  if (!BuildProcessEntryStack(runtime, record, mapped, &guest_rsp)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  if (!ResetPersistentThreadForEntry(runtime, record, mapped.entry,
                                     guest_rsp)) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  PrewarmPersistentRipSeeds(runtime, record);
  record->tls.target = record->identity;
  record->active.store(true, std::memory_order_release);
  record->guest_exit_requested = false;
  record->guest_exit_code = 0;
  record->controlled_failure = false;
  record->failure_status = LSX4_FEX_STATUS_OK;
  record->nonlocal_pending = false;
  record->hle_frame_pools = mapped_hle_frame_pools;
  lsx4::fex2608::ResetSignalCaptureGate(&record->host_signal.gate);
  runtime->active_threads.fetch_add(1, std::memory_order_relaxed);

  auto &state = record->thread->CurrentFrame->State;
  state.gregs[FEXCore::X86State::REG_RDI] = mapped.entry_params;
  state.gregs[FEXCore::X86State::REG_RSI] = mapped.exit_function;
  state.fs_cached =
      record->tls_configured ? record->tls.fs_base : mapped.tcb_base;
  state.gs_cached =
      record->tls_configured ? record->tls.gs_base : mapped.tcb_base;
  if (request->initial_context != nullptr) {
    status = ImportContext(runtime, record, request->initial_context, false);
    state.rip = mapped.entry;
  }
  while (status == LSX4_FEX_STATUS_OK) {
    status = ProcessQueuedSignals(runtime, record);
    if (status != LSX4_FEX_STATUS_OK || record->guest_exit_requested ||
        record->controlled_failure) {
      break;
    }
    ExecuteThreadWithGuestFpcr(runtime, record);
    if (lsx4::fex2608::SignalCaptureReady(&record->host_signal.gate)) {
      status = ProcessHostSignalCapture(runtime, record);
      continue;
    }
    if (record->normal_interrupt_pending.load(std::memory_order_acquire)) {
      status = ProcessNormalInterruptPending(runtime, record);
      continue;
    }
    if (record->guest_exit_requested || record->controlled_failure) {
      break;
    }
    status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    Fatal(runtime, status,
          "FEX ExecuteThread returned without an exit or signal landing");
  }

  const uint32_t result_capacity = result->struct_size;
  std::memset(result, 0, sizeof(*result));
  result->struct_size = result_capacity;
  result->abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  result->mapped_entry.struct_size = sizeof(result->mapped_entry);
  result->mapped_entry.abi_version = LSX4_FEX_MAPPED_ENTRY_ABI_V5;
  result->mapped_entry.request_cookie = mapped.request_cookie;
  result->final_context.struct_size = sizeof(result->final_context);
  result->final_context.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  if (status == LSX4_FEX_STATUS_OK) {
    status = ExportContext(runtime, record, &result->final_context);
  }
  if (status == LSX4_FEX_STATUS_OK && record->controlled_failure) {
    status = record->failure_status;
  } else if (status == LSX4_FEX_STATUS_OK && !record->guest_exit_requested) {
    status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    Fatal(runtime, status,
          "FEX ExecuteThread returned without the explicit guest-exit syscall");
  }
  result->mapped_entry.status = status;
  result->mapped_entry.rax = state.gregs[FEXCore::X86State::REG_RAX];
  result->mapped_entry.final_rip = state.rip;
  result->mapped_entry.guest_exit_code = record->guest_exit_code;
  result->diagnostics = runtime->diagnostics;
  record->tls.fs_base = state.fs_cached;
  record->tls.gs_base = state.gs_cached;

  if (!DisarmGuestSignalInterrupt(record) && status == LSX4_FEX_STATUS_OK) {
    status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->mapped_entry.status = status;
  }
  if (!DeferredSignalStateIsQuiescent(record)) {
    // Returning ownership of this ThreadRecord while a landing, helper, or
    // restoration lease remains live would allow reset/unbind to tear down its
    // stack or code mapping.  This state is unreachable on a valid sigreturn
    // path, so fail closed after releasing the pin instead of hiding it as an
    // ordinary backend error.
    (void)AbortDeferredSignalLanding(record);
    TerminateOnNestedHostFault(SIGBUS, FailClosedExit::NonQuiescentReturn);
  }
  record->active.store(false, std::memory_order_release);
  runtime->active_threads.fetch_sub(1, std::memory_order_relaxed);
  return status;
}

int32_t RegisterGuestSignalAction(void *opaque,
                                  const Lsx4FexGuestSignalActionV3 *action) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || action == nullptr ||
      action->struct_size < sizeof(*action) ||
      action->abi_version != LSX4_FEX_BACKEND_ABI_V3 || action->signum <= 0 ||
      action->signum >= 130 || action->restorer != 0) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  std::lock_guard lock{runtime->signal_actions_mutex};
  runtime->signal_actions[action->signum] = *action;
  runtime->signal_action_valid[action->signum] = true;
  return LSX4_FEX_STATUS_OK;
}

ThreadRecord *FindSignalTarget(Runtime *runtime,
                               const Lsx4FexThreadIdentityV3 &target) {
  if (g_thread_binding.runtime == runtime &&
      ThreadMatches(g_thread_binding.record, &target)) {
    return g_thread_binding.record;
  }
  std::lock_guard lock{runtime->records_mutex};
  for (auto *record : runtime->records) {
    if (ThreadMatches(record, &target)) {
      return record;
    }
  }
  return nullptr;
}

bool EnqueueQueuedSignal(ThreadRecord *record,
                         const ThreadRecord::QueuedSignal &queued) {
  std::lock_guard lock{record->signal_mutex};
  if (record->finalized.load(std::memory_order_acquire)) {
    return false;
  }
  const uint32_t next =
      (record->signal_queue_write + 1) % record->signal_queue.size();
  if (next == record->signal_queue_read) {
    return false;
  }
  record->signal_queue[record->signal_queue_write] = queued;
  record->signal_queue[record->signal_queue_write].delivery.explicit_context =
      nullptr;
  record->signal_queue_write = next;
  return true;
}

bool EnqueueSignal(ThreadRecord *record,
                   const Lsx4FexGuestSignalDeliveryV3 &delivery) {
  ThreadRecord::QueuedSignal queued{};
  queued.delivery = delivery;
  queued.delivery.explicit_context = nullptr;
  if (delivery.explicit_context != nullptr) {
    queued.explicit_context = *delivery.explicit_context;
    queued.has_explicit_context = true;
  }
  return EnqueueQueuedSignal(record, queued);
}

bool PopSignal(ThreadRecord *record, ThreadRecord::QueuedSignal *queued) {
  std::lock_guard lock{record->signal_mutex};
  if (record->signal_queue_read == record->signal_queue_write) {
    return false;
  }
  *queued = record->signal_queue[record->signal_queue_read];
  record->signal_queue_read =
      (record->signal_queue_read + 1) % record->signal_queue.size();
  return true;
}

bool SignalIsMasked(const ThreadRecord *record, int32_t signum) {
  if (signum > 0 && signum < 64) {
    return (record->signal_mask_low & (UINT64_C(1) << signum)) != 0;
  }
  if (signum >= 64 && signum < 128) {
    return (record->signal_mask_high & (UINT64_C(1) << (signum - 64))) != 0;
  }
  return false;
}

bool FinishGuestSignalDrain(ThreadRecord *record) {
  if (record == nullptr || record->thread == nullptr) {
    return false;
  }
  std::lock_guard lock{record->signal_mutex};
  bool deliverable = false;
  for (uint32_t index = record->signal_queue_read;
       index != record->signal_queue_write;
       index = (index + 1) % record->signal_queue.size()) {
    const auto &delivery = record->signal_queue[index].delivery;
    if ((delivery.delivery_flags & LSX4_FEX_SIGNAL_SYNCHRONOUS) != 0 ||
        !SignalIsMasked(record, delivery.signum)) {
      deliverable = true;
      break;
    }
  }
  // A deferred atomic helper can complete while a guest signal callback is
  // running.  That callback is still guest code and therefore still needs the
  // one-shot dispatcher probe which releases the executable-range pin.  Do
  // not let the queue-drain epilogue silently disarm that independent token.
  const bool should_arm =
      record->active.load(std::memory_order_acquire) &&
      (deliverable ||
       record->deferred_pin_release_interrupt.load(std::memory_order_acquire));
  const bool armed =
      record->interrupt_page_armed.load(std::memory_order_acquire);
  if (should_arm != armed) {
    const int protection = should_arm ? PROT_NONE : PROT_READ | PROT_WRITE;
    if (::mprotect(record->thread->InterruptFaultPage,
                   sizeof(record->thread->InterruptFaultPage),
                   protection) != 0) {
      record->signal_drain_active = false;
      return false;
    }
    record->interrupt_page_armed.store(should_arm, std::memory_order_release);
  }
  record->signal_drain_active = false;
  return true;
}

void AddSignalToMask(uint64_t *low, uint64_t *high, int32_t signum) {
  if (signum > 0 && signum < 64) {
    *low |= UINT64_C(1) << signum;
  } else if (signum >= 64 && signum < 128) {
    *high |= UINT64_C(1) << (signum - 64);
  }
}

int32_t HostSignalToGuestSignal(int32_t signum) {
  return signum == SIGBUS ? kOrbisSigBus : signum;
}

struct GuestFpState {
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> xmm_low{};
  std::array<__uint128_t, LSX4_FEX_GUEST_XMM_COUNT> ymm_high{};
  uint64_t mm[8][2]{};
  uint32_t mxcsr{};
  uint16_t fcw{};
  uint16_t fsw{};
  uint8_t abridged_ftw{};
};

uint16_t BuildGuestFsw(const FEXCore::Core::CPUState &state) {
  using namespace FEXCore::X86State;
  return static_cast<uint16_t>((state.flags[X87FLAG_IE_LOC] & 1) << 0 |
                               (state.flags[X87FLAG_DE_LOC] & 1) << 1 |
                               (state.flags[X87FLAG_ZE_LOC] & 1) << 2 |
                               (state.flags[X87FLAG_OE_LOC] & 1) << 3 |
                               (state.flags[X87FLAG_UE_LOC] & 1) << 4 |
                               (state.flags[X87FLAG_PE_LOC] & 1) << 5 |
                               (state.flags[X87FLAG_SF_LOC] & 1) << 6 |
                               (state.flags[X87FLAG_ES_LOC] & 1) << 7 |
                               (state.flags[X87FLAG_C0_LOC] & 1) << 8 |
                               (state.flags[X87FLAG_C1_LOC] & 1) << 9 |
                               (state.flags[X87FLAG_C2_LOC] & 1) << 10 |
                               (state.flags[X87FLAG_TOP_LOC] & 7) << 11 |
                               (state.flags[X87FLAG_C3_LOC] & 1) << 14 |
                               (state.flags[X87FLAG_B_LOC] & 1) << 15);
}

void SetGuestFsw(FEXCore::Core::CPUState *state, uint16_t fsw) {
  using namespace FEXCore::X86State;
  state->flags[X87FLAG_IE_LOC] = (fsw >> 0) & 1;
  state->flags[X87FLAG_DE_LOC] = (fsw >> 1) & 1;
  state->flags[X87FLAG_ZE_LOC] = (fsw >> 2) & 1;
  state->flags[X87FLAG_OE_LOC] = (fsw >> 3) & 1;
  state->flags[X87FLAG_UE_LOC] = (fsw >> 4) & 1;
  state->flags[X87FLAG_PE_LOC] = (fsw >> 5) & 1;
  state->flags[X87FLAG_SF_LOC] = (fsw >> 6) & 1;
  state->flags[X87FLAG_ES_LOC] = (fsw >> 7) & 1;
  state->flags[X87FLAG_C0_LOC] = (fsw >> 8) & 1;
  state->flags[X87FLAG_C1_LOC] = (fsw >> 9) & 1;
  state->flags[X87FLAG_C2_LOC] = (fsw >> 10) & 1;
  state->flags[X87FLAG_TOP_LOC] = (fsw >> 11) & 7;
  state->flags[X87FLAG_C3_LOC] = (fsw >> 14) & 1;
  state->flags[X87FLAG_B_LOC] = (fsw >> 15) & 1;
}

GuestFpState SnapshotGuestFpState(Runtime *runtime, ThreadRecord *record) {
  GuestFpState result{};
  runtime->context->ReconstructXMMRegisters(
      record->thread, result.xmm_low.data(), result.ymm_high.data());
  const auto &state = record->thread->CurrentFrame->State;
  std::memcpy(result.mm, state.mm, sizeof(result.mm));
  result.mxcsr = state.mxcsr;
  result.fcw = state.FCW;
  result.fsw = BuildGuestFsw(state);
  result.abridged_ftw = state.AbridgedFTW;
  return result;
}

GuestFpState DefaultGuestFpState() {
  GuestFpState result{};
  result.fcw = UINT16_C(0x037f);
  result.mxcsr = UINT32_C(0x00001f80);
  return result;
}

void ApplyGuestFpState(Runtime *runtime, ThreadRecord *record,
                       const GuestFpState &saved) {
  runtime->context->SetXMMRegistersFromState(
      record->thread, saved.xmm_low.data(), saved.ymm_high.data());
  auto &state = record->thread->CurrentFrame->State;
  std::memcpy(state.mm, saved.mm, sizeof(saved.mm));
  state.mxcsr = saved.mxcsr;
  state.FCW = saved.fcw;
  state.AbridgedFTW = saved.abridged_ftw;
  SetGuestFsw(&state, saved.fsw);
}

void RestoreV3OmittedGuestFpState(Runtime *runtime, ThreadRecord *record,
                                  const GuestFpState &saved) {
  GuestFpState controlled = SnapshotGuestFpState(runtime, record);
  controlled.ymm_high = saved.ymm_high;
  std::memcpy(controlled.mm, saved.mm, sizeof(controlled.mm));
  controlled.fcw = saved.fcw;
  controlled.fsw = saved.fsw;
  controlled.abridged_ftw = saved.abridged_ftw;
  ApplyGuestFpState(runtime, record, controlled);
}

enum class GuestFpRestoreMode {
  Full,
  V3Omitted,
  Committed,
};

struct GuestFpTransaction {
  Runtime *runtime;
  ThreadRecord *record;
  GuestFpState interrupted;
  GuestFpRestoreMode mode{GuestFpRestoreMode::Full};

  ~GuestFpTransaction() {
    if (mode == GuestFpRestoreMode::Full) {
      ApplyGuestFpState(runtime, record, interrupted);
    } else if (mode == GuestFpRestoreMode::V3Omitted) {
      RestoreV3OmittedGuestFpState(runtime, record, interrupted);
    }
  }
};

struct GuestContextTransaction {
  Runtime *runtime;
  ThreadRecord *record;
  Lsx4FexGuestContextV3 interrupted;
  bool rollback{true};

  ~GuestContextTransaction() {
    if (rollback) {
      (void)ImportContext(runtime, record, &interrupted, true);
    }
  }
};

void EncodeOrbisInlineFpState(const GuestFpState &guest,
                              uint64_t (&storage)[104], uint32_t mxcsr_mask) {
  OrbisInlineFpState output{};
  output.fcw = guest.fcw;
  output.fsw = guest.fsw;
  output.ftw = guest.abridged_ftw;
  output.mxcsr = guest.mxcsr;
  output.mxcsr_mask = mxcsr_mask;
  const size_t top = (guest.fsw >> 11) & 7;
  for (size_t index = 0; index < std::size(output.st); ++index) {
    const size_t physical = (index + top) & 7;
    std::memcpy(output.st[index].bytes, guest.mm[physical],
                sizeof(output.st[index].bytes));
  }
  for (size_t index = 0; index < std::size(output.xmm); ++index) {
    std::memcpy(output.xmm[index].bytes, &guest.xmm_low[index],
                sizeof(output.xmm[index].bytes));
    std::memcpy(output.ymmh[index].bytes, &guest.ymm_high[index],
                sizeof(output.ymmh[index].bytes));
  }
  output.xstate.xstate_bv = kOrbisSupportedXstate;
  std::memcpy(storage, &output, sizeof(output));
}

bool BytesAreZero(const uint8_t *bytes, size_t size) {
  return std::all_of(bytes, bytes + size,
                     [](uint8_t value) { return value == 0; });
}

bool DecodeOrbisInlineFpState(const OrbisMcontext &machine,
                              const GuestFpState &current, uint32_t mxcsr_mask,
                              GuestFpState *decoded) {
  if (machine.mc_fpformat == kOrbisFpFormatNoDevice) {
    *decoded = current;
    return true;
  }
  if (machine.mc_fpformat != kOrbisFpFormatXmm) {
    return false;
  }
  if (machine.mc_ownedfp == kOrbisFpOwnedNone) {
    *decoded = DefaultGuestFpState();
    return true;
  }
  if (machine.mc_ownedfp != kOrbisFpOwnedFpu &&
      machine.mc_ownedfp != kOrbisFpOwnedPcb) {
    return false;
  }

  OrbisInlineFpState input{};
  std::memcpy(&input, machine.mc_fpstate, sizeof(input));
  if ((input.xstate.xstate_bv & ~kOrbisSupportedXstate) != 0 ||
      input.xstate.xcomp_bv != 0 || input.reserved0 != 0 ||
      !BytesAreZero(input.padding, sizeof(input.padding)) ||
      !BytesAreZero(input.xstate.reserved, sizeof(input.xstate.reserved)) ||
      input.mxcsr_mask == 0 || (input.mxcsr_mask & ~mxcsr_mask) != 0 ||
      (input.mxcsr & ~input.mxcsr_mask) != 0) {
    return false;
  }

  GuestFpState result = DefaultGuestFpState();
  if ((input.xstate.xstate_bv & UINT64_C(1)) != 0) {
    result.fcw = input.fcw;
    result.fsw = input.fsw;
    result.abridged_ftw = input.ftw;
    const size_t top = (result.fsw >> 11) & 7;
    for (size_t index = 0; index < std::size(input.st); ++index) {
      const size_t physical = (index + top) & 7;
      std::memcpy(result.mm[physical], input.st[index].bytes,
                  sizeof(input.st[index].bytes));
    }
  }
  if ((input.xstate.xstate_bv & UINT64_C(2)) != 0) {
    result.mxcsr = input.mxcsr;
    for (size_t index = 0; index < std::size(input.xmm); ++index) {
      std::memcpy(&result.xmm_low[index], input.xmm[index].bytes,
                  sizeof(input.xmm[index].bytes));
    }
  }
  if ((input.xstate.xstate_bv & UINT64_C(4)) != 0) {
    for (size_t index = 0; index < std::size(input.ymmh); ++index) {
      std::memcpy(&result.ymm_high[index], input.ymmh[index].bytes,
                  sizeof(input.ymmh[index].bytes));
    }
  }
  *decoded = result;
  return true;
}

void GuestContextToOrbisUcontext(const Lsx4FexGuestContextV3 &guest,
                                 const GuestFpState &fp_state,
                                 uint32_t mxcsr_mask, uint64_t signal_mask_low,
                                 uint64_t signal_mask_high,
                                 uint64_t fault_address, int32_t signum,
                                 uint8_t trap_number, uint16_t error_code,
                                 OrbisUcontext *output) {
  *output = {};
  output->signal_mask.bits[0] = signal_mask_low;
  output->signal_mask.bits[1] = signal_mask_high;
  auto &machine = output->machine;
  machine.mc_rax = guest.gpr[FEXCore::X86State::REG_RAX];
  machine.mc_rcx = guest.gpr[FEXCore::X86State::REG_RCX];
  machine.mc_rdx = guest.gpr[FEXCore::X86State::REG_RDX];
  machine.mc_rbx = guest.gpr[FEXCore::X86State::REG_RBX];
  machine.mc_rsp = guest.gpr[FEXCore::X86State::REG_RSP];
  machine.mc_rbp = guest.gpr[FEXCore::X86State::REG_RBP];
  machine.mc_rsi = guest.gpr[FEXCore::X86State::REG_RSI];
  machine.mc_rdi = guest.gpr[FEXCore::X86State::REG_RDI];
  machine.mc_r8 = guest.gpr[FEXCore::X86State::REG_R8];
  machine.mc_r9 = guest.gpr[FEXCore::X86State::REG_R9];
  machine.mc_r10 = guest.gpr[FEXCore::X86State::REG_R10];
  machine.mc_r11 = guest.gpr[FEXCore::X86State::REG_R11];
  machine.mc_r12 = guest.gpr[FEXCore::X86State::REG_R12];
  machine.mc_r13 = guest.gpr[FEXCore::X86State::REG_R13];
  machine.mc_r14 = guest.gpr[FEXCore::X86State::REG_R14];
  machine.mc_r15 = guest.gpr[FEXCore::X86State::REG_R15];
  machine.mc_rip = guest.rip;
  machine.mc_rflags = guest.rflags != 0 ? guest.rflags : UINT64_C(0x202);
  machine.mc_fs = static_cast<uint16_t>(guest.fs_base);
  machine.mc_gs = static_cast<uint16_t>(guest.gs_base);
  machine.mc_fsbase = guest.fs_base;
  machine.mc_gsbase = guest.gs_base;
  machine.mc_trapno =
      trap_number != 0 ? trap_number : (signum == SIGILL ? 6 : 14);
  machine.mc_err = error_code;
  machine.mc_addr = fault_address;
  machine.mc_flags = 3;
  machine.mc_cs = UINT64_C(0x43);
  machine.mc_ss = UINT64_C(0x3b);
  machine.mc_ds = UINT16_C(0x3b);
  machine.mc_es = UINT16_C(0x3b);
  machine.mc_len = sizeof(OrbisMcontext);
  machine.mc_fpformat = kOrbisFpFormatXmm;
  machine.mc_ownedfp = kOrbisFpOwnedFpu;
  EncodeOrbisInlineFpState(fp_state, machine.mc_fpstate, mxcsr_mask);
}

bool OrbisUcontextToGuestContext(const OrbisUcontext &source,
                                 const Lsx4FexGuestContextV3 &current,
                                 const GuestFpState &current_fp,
                                 uint32_t mxcsr_mask,
                                 Lsx4FexGuestContextV3 *guest,
                                 GuestFpState *fp_state) {
  const auto &machine = source.machine;
  if (machine.mc_len != sizeof(OrbisMcontext) ||
      (static_cast<uint32_t>(machine.mc_flags) & ~kOrbisMcKnownFlags) != 0 ||
      !DecodeOrbisInlineFpState(machine, current_fp, mxcsr_mask, fp_state)) {
    return false;
  }
  *guest = current;
  guest->gpr[FEXCore::X86State::REG_RAX] = machine.mc_rax;
  guest->gpr[FEXCore::X86State::REG_RCX] = machine.mc_rcx;
  guest->gpr[FEXCore::X86State::REG_RDX] = machine.mc_rdx;
  guest->gpr[FEXCore::X86State::REG_RBX] = machine.mc_rbx;
  guest->gpr[FEXCore::X86State::REG_RSP] = machine.mc_rsp;
  guest->gpr[FEXCore::X86State::REG_RBP] = machine.mc_rbp;
  guest->gpr[FEXCore::X86State::REG_RSI] = machine.mc_rsi;
  guest->gpr[FEXCore::X86State::REG_RDI] = machine.mc_rdi;
  guest->gpr[FEXCore::X86State::REG_R8] = machine.mc_r8;
  guest->gpr[FEXCore::X86State::REG_R9] = machine.mc_r9;
  guest->gpr[FEXCore::X86State::REG_R10] = machine.mc_r10;
  guest->gpr[FEXCore::X86State::REG_R11] = machine.mc_r11;
  guest->gpr[FEXCore::X86State::REG_R12] = machine.mc_r12;
  guest->gpr[FEXCore::X86State::REG_R13] = machine.mc_r13;
  guest->gpr[FEXCore::X86State::REG_R14] = machine.mc_r14;
  guest->gpr[FEXCore::X86State::REG_R15] = machine.mc_r15;
  guest->rip = machine.mc_rip;
  const uint64_t baseline_rflags =
      current.rflags != 0 ? current.rflags : UINT64_C(0x202);
  guest->rflags = (machine.mc_rflags & kOrbisRflagsUserChange) |
                  (baseline_rflags & ~kOrbisRflagsUserChange) | UINT64_C(2);
  if ((machine.mc_flags & kOrbisMcHasBases) != 0) {
    guest->fs_base = machine.mc_fsbase;
    guest->gs_base = machine.mc_gsbase;
  }
  for (size_t index = 0; index < fp_state->xmm_low.size(); ++index) {
    std::memcpy(&guest->xmm[index], &fp_state->xmm_low[index],
                sizeof(guest->xmm[index]));
  }
  guest->mxcsr = fp_state->mxcsr;
  return true;
}

int32_t DeliverSignal(Runtime *runtime, ThreadRecord *record,
                      const ThreadRecord::QueuedSignal &queued) {
  if (runtime == nullptr || record == nullptr || record->thread == nullptr) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  const int32_t signum = queued.delivery.signum;
  const bool context_arg1 =
      (queued.delivery.delivery_flags & LSX4_FEX_SIGNAL_CONTEXT_ARG1) != 0;
  Lsx4FexGuestSignalActionV3 action{};
  {
    std::lock_guard lock{runtime->signal_actions_mutex};
    if (!runtime->signal_action_valid[signum]) {
      const auto &state = record->thread->CurrentFrame->State;
      char message[1280]{};
      std::snprintf(
          message, sizeof(message),
          "delivery rejected: no guest action signum=%d code=%d fault=0x%llx "
          "explicitRip=0x%llx explicitRsp=0x%llx synchronous=%u "
          "stateRip=0x%llx regs=[rax:0x%llx,rcx:0x%llx,rdx:0x%llx,"
          "rbx:0x%llx,rsp:0x%llx,rbp:0x%llx,rsi:0x%llx,rdi:0x%llx,"
          "r8:0x%llx,r9:0x%llx,r10:0x%llx,r11:0x%llx,r12:0x%llx,"
          "r13:0x%llx,r14:0x%llx,r15:0x%llx] explicitContext=%u",
          signum, queued.signal_code,
          static_cast<unsigned long long>(queued.fault_address),
          static_cast<unsigned long long>(queued.delivery.explicit_rip),
          static_cast<unsigned long long>(queued.delivery.explicit_rsp),
          (queued.delivery.delivery_flags & LSX4_FEX_SIGNAL_SYNCHRONOUS) != 0,
          static_cast<unsigned long long>(state.rip),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RAX]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RCX]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RDX]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RBX]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RSP]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RBP]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RSI]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RDI]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R8]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R9]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R10]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R11]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R12]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R13]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R14]),
          static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_R15]),
          queued.has_explicit_context ? 1U : 0U);
      Log(runtime, 6, "signal", message);
      return LSX4_FEX_STATUS_NOT_FOUND;
    }
    action = runtime->signal_actions[signum];
    if (context_arg1 && (action.action_flags & kOrbisSaSigInfo) == 0) {
      return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    if ((action.action_flags & kOrbisSaResetHand) != 0) {
      runtime->signal_action_valid[signum] = false;
      runtime->signal_actions[signum] = {};
    }
  }
  if (action.handler == 1) {
    return LSX4_FEX_STATUS_OK;
  }
  if (action.handler == 0 || !IsExecutableAddress(runtime, action.handler, 1)) {
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  Lsx4FexGuestContextV3 interrupted{};
  interrupted.struct_size = sizeof(interrupted);
  interrupted.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  int32_t status = ExportContext(runtime, record, &interrupted);
  if (status != LSX4_FEX_STATUS_OK) {
    return status;
  }
  if (queued.has_explicit_context) {
    status = ImportContext(runtime, record, &queued.explicit_context, true);
    if (status != LSX4_FEX_STATUS_OK) {
      return status;
    }
    status = ExportContext(runtime, record, &interrupted);
    if (status != LSX4_FEX_STATUS_OK) {
      return status;
    }
  }
  auto &state = record->thread->CurrentFrame->State;
  if (queued.delivery.explicit_rip != 0) {
    state.rip = queued.delivery.explicit_rip;
    interrupted.rip = queued.delivery.explicit_rip;
  }
  if (queued.delivery.explicit_rsp != 0) {
    state.gregs[FEXCore::X86State::REG_RSP] = queued.delivery.explicit_rsp;
    interrupted.gpr[FEXCore::X86State::REG_RSP] = queued.delivery.explicit_rsp;
  }
  const GuestFpState interrupted_fp = SnapshotGuestFpState(runtime, record);
  GuestContextTransaction context_transaction{runtime, record, interrupted};
  GuestFpTransaction fp_transaction{runtime, record, interrupted_fp};

  const uint64_t prior_mask_low = record->signal_mask_low;
  const uint64_t prior_mask_high = record->signal_mask_high;
  record->signal_mask_low |= action.mask_low;
  record->signal_mask_high |= action.mask_high;
  if ((action.action_flags & static_cast<uint32_t>(kOrbisSaNoDefer)) == 0) {
    AddSignalToMask(&record->signal_mask_low, &record->signal_mask_high,
                    signum);
  }

  const uint64_t siginfo_address = record->scratch_base + runtime->page_size;
  const uint64_t context_address =
      (siginfo_address + sizeof(OrbisSiginfo) + 63) & ~UINT64_C(63);
  if (context_address + sizeof(OrbisUcontext) >
      record->scratch_base + record->scratch_size) {
    record->signal_mask_low = prior_mask_low;
    record->signal_mask_high = prior_mask_high;
    return LSX4_FEX_STATUS_BUFFER_TOO_SMALL;
  }
  OrbisSiginfo signal_info{};
  signal_info.signo = signum;
  signal_info.code = queued.signal_code;
  signal_info.address = queued.fault_address;
  OrbisUcontext signal_context{};
  GuestContextToOrbisUcontext(
      interrupted, interrupted_fp, GuestMxcsrMask(runtime), prior_mask_low,
      prior_mask_high, signal_info.address, signum, queued.trap_number,
      queued.error_code, &signal_context);
  std::memcpy(reinterpret_cast<void *>(siginfo_address), &signal_info,
              sizeof(signal_info));
  std::memcpy(reinterpret_cast<void *>(context_address), &signal_context,
              sizeof(signal_context));
  state.gregs[FEXCore::X86State::REG_RDI] = static_cast<uint64_t>(signum);
  if (context_arg1) {
    state.gregs[FEXCore::X86State::REG_RSI] = context_address;
    state.gregs[FEXCore::X86State::REG_RDX] = siginfo_address;
  } else if ((action.action_flags & kOrbisSaSigInfo) != 0) {
    state.gregs[FEXCore::X86State::REG_RSI] = siginfo_address;
    state.gregs[FEXCore::X86State::REG_RDX] = context_address;
  } else {
    state.gregs[FEXCore::X86State::REG_RSI] = 0;
    state.gregs[FEXCore::X86State::REG_RDX] = 0;
  }
  const uint64_t handler_entry_epoch =
      record->context_epoch.load(std::memory_order_acquire);
  status =
      ExecuteGuestCallbackWithSignalResume(runtime, record, action.handler);
  if (status != LSX4_FEX_STATUS_OK || record->guest_exit_requested ||
      record->controlled_failure) {
    record->signal_mask_low = prior_mask_low;
    record->signal_mask_high = prior_mask_high;
    return status;
  }
  if (record->context_epoch.load(std::memory_order_acquire) !=
      handler_entry_epoch) {
    /* A controlled set-context owns the resume state; do not overwrite it. */
    context_transaction.rollback = false;
    fp_transaction.mode = GuestFpRestoreMode::V3Omitted;
    record->signal_mask_low = prior_mask_low;
    record->signal_mask_high = prior_mask_high;
    return LSX4_FEX_STATUS_OK;
  }

  std::memcpy(&signal_context, reinterpret_cast<const void *>(context_address),
              sizeof(signal_context));
  Lsx4FexGuestContextV3 resumed{};
  GuestFpState resumed_fp{};
  if (!OrbisUcontextToGuestContext(signal_context, interrupted, interrupted_fp,
                                   GuestMxcsrMask(runtime), &resumed,
                                   &resumed_fp)) {
    record->signal_mask_low = prior_mask_low;
    record->signal_mask_high = prior_mask_high;
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  status = ImportContext(runtime, record, &resumed, true);
  if (status != LSX4_FEX_STATUS_OK) {
    record->signal_mask_low = prior_mask_low;
    record->signal_mask_high = prior_mask_high;
    return status;
  }
  ApplyGuestFpState(runtime, record, resumed_fp);
  record->signal_mask_low = signal_context.signal_mask.bits[0];
  record->signal_mask_high = signal_context.signal_mask.bits[1];
  context_transaction.rollback = false;
  fp_transaction.mode = GuestFpRestoreMode::Committed;
  return LSX4_FEX_STATUS_OK;
}

int32_t ProcessQueuedSignals(Runtime *runtime, ThreadRecord *record) {
  if ((runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_GUEST_SIGNAL_DELIVERY) == 0) {
    return LSX4_FEX_STATUS_OK;
  }
  /* Guest handlers may compile or enter another block, so the FEX interrupt
   * page must be writable before the first queued delivery, not merely after
   * the queue has drained. */
  const GuestSignalDrainBegin begin = BeginGuestSignalDrain(record);
  if (begin == GuestSignalDrainBegin::Failed) {
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  // A guest handler may re-enter through an HLE callback.  The outer drain
  // retains ownership; nested processing must neither fail nor finish it.
  if (begin == GuestSignalDrainBegin::AlreadyActive) {
    return LSX4_FEX_STATUS_OK;
  }
  const auto finish = [record](int32_t status) {
    return FinishGuestSignalDrain(record) ? status
                                          : LSX4_FEX_STATUS_BACKEND_FAILURE;
  };
  size_t deferred = 0;
  for (size_t examined = 0; examined < record->signal_queue.size();
       ++examined) {
    ThreadRecord::QueuedSignal queued{};
    if (!PopSignal(record, &queued)) {
      return finish(LSX4_FEX_STATUS_OK);
    }
    if ((queued.delivery.delivery_flags & LSX4_FEX_SIGNAL_SYNCHRONOUS) == 0 &&
        SignalIsMasked(record, queued.delivery.signum)) {
      if (!EnqueueQueuedSignal(record, queued)) {
        return finish(LSX4_FEX_STATUS_BUFFER_TOO_SMALL);
      }
      ++deferred;
      if (deferred == record->signal_queue.size() - 1) {
        return finish(LSX4_FEX_STATUS_OK);
      }
      continue;
    }
    deferred = 0;
    const int32_t status = DeliverSignal(runtime, record, queued);
    if (status != LSX4_FEX_STATUS_OK) {
      return finish(status);
    }
  }
  return finish(LSX4_FEX_STATUS_OK);
}

int32_t ProcessNormalInterruptPending(Runtime *runtime, ThreadRecord *record) {
  if (runtime == nullptr || record == nullptr ||
      !record->normal_interrupt_pending.exchange(false,
                                                 std::memory_order_acq_rel)) {
    return LSX4_FEX_STATUS_OK;
  }
  if (!DisarmGuestSignalInterrupt(record)) {
    record->controlled_failure = true;
    record->failure_status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  CompleteDeferredPinAtDispatcher(record);
  return ProcessQueuedSignals(runtime, record);
}

int32_t ProcessHostSignalCapture(Runtime *runtime, ThreadRecord *record) {
  auto &capture = record->host_signal;
  if (!lsx4::fex2608::BeginSignalCaptureProcessing(&capture.gate)) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  const int32_t host_signum = capture.signum;
  const int32_t signal_code = capture.signal_code;
  const bool was_in_jit = capture.was_in_jit != 0;
  const bool generated_guest_fault = capture.generated_guest_fault != 0;
  FEXCore::Core::CpuStateFrame::SynchronousFaultDataStruct synthetic_fault{};
  std::memcpy(&synthetic_fault, &capture.synchronous_fault_data,
              sizeof(synthetic_fault));
  const uint64_t guest_rip = capture.guest_rip;
  const uint64_t fault_address = capture.fault_address;
  const lsx4::fex2608::HostSignalMask saved_signal_mask = capture.saved_signal_mask;
  const bool interrupt_page_fault =
      IsGuestSignalInterruptFault(record, host_signum, fault_address);

  const uint64_t capture_sequence =
      runtime->signal_capture_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t interrupt_capture_count =
      interrupt_page_fault ? runtime->signal_interrupt_capture_count.fetch_add(
                                 1, std::memory_order_relaxed) +
                                 1
                           : runtime->signal_interrupt_capture_count.load(
                                 std::memory_order_relaxed);
  const bool state_rip_mismatch =
      (was_in_jit || generated_guest_fault) && guest_rip != 0 &&
      record->thread->CurrentFrame->State.rip != guest_rip;
  const uint64_t mismatch_count =
      state_rip_mismatch ? runtime->signal_state_rip_mismatch_count.fetch_add(
                               1, std::memory_order_relaxed) +
                               1
                         : runtime->signal_state_rip_mismatch_count.load(
                               std::memory_order_relaxed);
  const bool log_capture = generated_guest_fault || capture_sequence <= 8 ||
                           (capture_sequence & (capture_sequence - 1)) == 0;
  if (log_capture) {
    const auto &state = record->thread->CurrentFrame->State;
    char message[640]{};
    std::snprintf(
        message, sizeof(message),
        "captured seq=%llu interrupts=%llu ripMismatch=%llu signum=%d "
        "code=%d inJit=%u generated=%u interrupt=%u "
        "guestRip=0x%llx fault=0x%llx hostPc=0x%llx hostSp=0x%llx "
        "stateRip=0x%llx rsp=0x%llx rdi=0x%llx rsi=0x%llx r14=0x%llx "
        "r15=0x%llx deferredPhase=%u releasePending=%u pageArmed=%u",
        static_cast<unsigned long long>(capture_sequence),
        static_cast<unsigned long long>(interrupt_capture_count),
        static_cast<unsigned long long>(mismatch_count), host_signum,
        signal_code, was_in_jit, generated_guest_fault, interrupt_page_fault,
        static_cast<unsigned long long>(guest_rip),
        static_cast<unsigned long long>(fault_address),
        static_cast<unsigned long long>(capture.host_pc),
        static_cast<unsigned long long>(capture.host_sp),
        static_cast<unsigned long long>(state.rip),
        static_cast<unsigned long long>(
            state.gregs[FEXCore::X86State::REG_RSP]),
        static_cast<unsigned long long>(
            state.gregs[FEXCore::X86State::REG_RDI]),
        static_cast<unsigned long long>(
            state.gregs[FEXCore::X86State::REG_RSI]),
        static_cast<unsigned long long>(
            state.gregs[FEXCore::X86State::REG_R14]),
        static_cast<unsigned long long>(
            state.gregs[FEXCore::X86State::REG_R15]),
        record->deferred_signal.phase.load(std::memory_order_acquire),
        record->deferred_pin_release_interrupt.load(std::memory_order_acquire),
        record->interrupt_page_armed.load(std::memory_order_acquire));
    Log(runtime, 6, "signal", message);

    const uint64_t guest_rsp = state.gregs[FEXCore::X86State::REG_RSP];
    std::array<uint64_t, 8> guest_stack{};
    bool guest_stack_valid = false;
    if (record->stack_size >= sizeof(guest_stack) &&
        guest_rsp >= record->stack_base &&
        guest_rsp - record->stack_base <=
            record->stack_size - sizeof(guest_stack)) {
      std::memcpy(guest_stack.data(), reinterpret_cast<const void *>(guest_rsp),
                  sizeof(guest_stack));
      guest_stack_valid = true;
    }

    const uint64_t callret_base =
        reinterpret_cast<uint64_t>(record->thread->CallRetStackBase);
    const uint64_t callret_sp = state.callret_sp;
    std::array<uint64_t, 4> callret_window{};
    bool callret_window_valid = false;
    if (callret_sp >= callret_base + sizeof(callret_window) &&
        callret_sp - callret_base <=
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE) {
      std::memcpy(
          callret_window.data(),
          reinterpret_cast<const void *>(callret_sp - sizeof(callret_window)),
          sizeof(callret_window));
      callret_window_valid = true;
    }
    std::array<uint64_t, 8> callret_active{};
    bool callret_active_valid = false;
    if (callret_sp >= callret_base &&
        callret_sp - callret_base <=
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE -
                sizeof(callret_active)) {
      std::memcpy(callret_active.data(),
                  reinterpret_cast<const void *>(callret_sp),
                  sizeof(callret_active));
      callret_active_valid = true;
    }

    char state_message[1280]{};
    std::snprintf(state_message, sizeof(state_message),
                  "capture-state gpr="
                  "[%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,"
                  "%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx] "
                  "stackValid=%u stack="
                  "[%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx] "
                  "callretSp=0x%llx callretReset=0x%llx callretValid=%u "
                  "callret[-32..]=[%llx,%llx,%llx,%llx] activeValid=%u "
                  "active=[%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx]",
                  static_cast<unsigned long long>(state.gregs[0]),
                  static_cast<unsigned long long>(state.gregs[1]),
                  static_cast<unsigned long long>(state.gregs[2]),
                  static_cast<unsigned long long>(state.gregs[3]),
                  static_cast<unsigned long long>(state.gregs[4]),
                  static_cast<unsigned long long>(state.gregs[5]),
                  static_cast<unsigned long long>(state.gregs[6]),
                  static_cast<unsigned long long>(state.gregs[7]),
                  static_cast<unsigned long long>(state.gregs[8]),
                  static_cast<unsigned long long>(state.gregs[9]),
                  static_cast<unsigned long long>(state.gregs[10]),
                  static_cast<unsigned long long>(state.gregs[11]),
                  static_cast<unsigned long long>(state.gregs[12]),
                  static_cast<unsigned long long>(state.gregs[13]),
                  static_cast<unsigned long long>(state.gregs[14]),
                  static_cast<unsigned long long>(state.gregs[15]),
                  guest_stack_valid,
                  static_cast<unsigned long long>(guest_stack[0]),
                  static_cast<unsigned long long>(guest_stack[1]),
                  static_cast<unsigned long long>(guest_stack[2]),
                  static_cast<unsigned long long>(guest_stack[3]),
                  static_cast<unsigned long long>(guest_stack[4]),
                  static_cast<unsigned long long>(guest_stack[5]),
                  static_cast<unsigned long long>(guest_stack[6]),
                  static_cast<unsigned long long>(guest_stack[7]),
                  static_cast<unsigned long long>(callret_sp),
                  static_cast<unsigned long long>(record->callret_reset_sp),
                  callret_window_valid,
                  static_cast<unsigned long long>(callret_window[0]),
                  static_cast<unsigned long long>(callret_window[1]),
                  static_cast<unsigned long long>(callret_window[2]),
                  static_cast<unsigned long long>(callret_window[3]),
                  callret_active_valid,
                  static_cast<unsigned long long>(callret_active[0]),
                  static_cast<unsigned long long>(callret_active[1]),
                  static_cast<unsigned long long>(callret_active[2]),
                  static_cast<unsigned long long>(callret_active[3]),
                  static_cast<unsigned long long>(callret_active[4]),
                  static_cast<unsigned long long>(callret_active[5]),
                  static_cast<unsigned long long>(callret_active[6]),
                  static_cast<unsigned long long>(callret_active[7]));
    Log(runtime, 6, "signal", state_message);

    if (guest_rip >= 32 && IsExecutableAddress(runtime, guest_rip - 32, 96)) {
      std::array<uint8_t, 96> guest_code{};
      std::memcpy(guest_code.data(),
                  reinterpret_cast<const void *>(guest_rip - 32),
                  guest_code.size());
      char code_message[640]{};
      size_t used = static_cast<size_t>(
          std::snprintf(code_message, sizeof(code_message),
                        "capture-code base=0x%llx ripOffset=32 bytes=",
                        static_cast<unsigned long long>(guest_rip - 32)));
      for (const uint8_t byte : guest_code) {
        if (used + 4 >= sizeof(code_message)) {
          break;
        }
        const int written = std::snprintf(
            code_message + used, sizeof(code_message) - used, "%02x", byte);
        if (written <= 0) {
          break;
        }
        used += static_cast<size_t>(written);
      }
      Log(runtime, 6, "signal", code_message);
    }
  }

  if (generated_guest_fault) {
    record->thread->CurrentFrame->SynchronousFaultData
        .FaultToTopAndGeneratedException = false;
  }

  // ThreadStopHandlerSpillSRA has made all GPR, PF/AF, callret_sp, XMM and
  // NZCV machine-state authoritative for a raw JIT fault. The three synthetic
  // dispatcher landings spill before faulting and publish State.rip there.
  // Re-assert either exact RIP here before ExportContext so explicit signal
  // delivery never observes a stale dispatcher RIP.
  const bool has_exact_guest_rip = was_in_jit || generated_guest_fault;
  const bool invalid_guest_rip = has_exact_guest_rip && guest_rip == 0;
  if (has_exact_guest_rip && !invalid_guest_rip) {
    record->thread->CurrentFrame->State.rip = guest_rip;
  }
  lsx4::fex2608::FinishSignalCaptureProcessing(&capture.gate);

  // The kernel restores the handler-edited mask on signal return. Restore the
  // exact pre-fault mask before any path can compile or re-enter guest code.
  if (lsx4::fex2608::HostRestoreSignalMask(saved_signal_mask) != 0) {
    record->controlled_failure = true;
    record->failure_status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  if (invalid_guest_rip) {
    record->controlled_failure = true;
    record->failure_status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }
  const bool guest_memory_fault =
      (host_signum == SIGSEGV || host_signum == SIGBUS) &&
      (was_in_jit || generated_guest_fault) && !interrupt_page_fault;
  if (guest_memory_fault && runtime->cpu_tuning.memory_exceptions == 0) {
    Log(runtime, 6, "signal",
        "guest memory fault rejected by Fast memory-exception policy");
    record->controlled_failure = true;
    record->failure_status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }

  if (record->guest_exit_requested || record->controlled_failure) {
    return LSX4_FEX_STATUS_OK;
  }
  if (host_signum == kBridgeWakeSignal || interrupt_page_fault) {
    (void)record->normal_interrupt_pending.exchange(false,
                                                    std::memory_order_acq_rel);
    CompleteDeferredPinAtDispatcher(record);
    return ProcessQueuedSignals(runtime, record);
  }

  ThreadRecord::QueuedSignal queued{};
  queued.delivery.struct_size = sizeof(queued.delivery);
  queued.delivery.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  queued.delivery.target = record->identity;
  queued.delivery.signum = HostSignalToGuestSignal(host_signum);
  queued.delivery.delivery_flags =
      LSX4_FEX_SIGNAL_USE_EXPLICIT_CONTEXT | LSX4_FEX_SIGNAL_SYNCHRONOUS;
  queued.explicit_context.struct_size = sizeof(queued.explicit_context);
  queued.explicit_context.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  int32_t status = ExportContext(runtime, record, &queued.explicit_context);
  if (status != LSX4_FEX_STATUS_OK) {
    return status;
  }
  queued.has_explicit_context = true;
  queued.delivery.explicit_rip = queued.explicit_context.rip;
  queued.delivery.explicit_rsp =
      queued.explicit_context.gpr[FEXCore::X86State::REG_RSP];
  queued.signal_code = signal_code;
  queued.trap_number = generated_guest_fault ? synthetic_fault.TrapNo : 0;
  queued.error_code = generated_guest_fault ? synthetic_fault.err_code : 0;
  queued.fault_address = generated_guest_fault ? guest_rip : fault_address;
  return DeliverSignal(runtime, record, queued);
}

int32_t QueueGuestSignal(void *opaque,
                         const Lsx4FexGuestSignalDeliveryV3 *delivery) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || delivery == nullptr ||
      delivery->struct_size < sizeof(*delivery) ||
      delivery->abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      delivery->target.struct_size < sizeof(Lsx4FexThreadIdentityV3) ||
      delivery->target.abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      (delivery->target.guest_thread == 0 &&
       delivery->target.pthread_handle == 0 && delivery->target.host_tid == 0 &&
       delivery->target.backend_thread == 0) ||
      delivery->signum <= 0 || delivery->signum >= 130 ||
      (delivery->delivery_flags & ~(LSX4_FEX_SIGNAL_USE_EXPLICIT_CONTEXT |
                                    LSX4_FEX_SIGNAL_SYNCHRONOUS |
                                    LSX4_FEX_SIGNAL_CONTEXT_ARG1)) != 0 ||
      (delivery->explicit_context != nullptr &&
       (delivery->explicit_context->struct_size <
            sizeof(Lsx4FexGuestContextV3) ||
        delivery->explicit_context->abi_version != LSX4_FEX_BACKEND_ABI_V3))) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  if ((runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_GUEST_SIGNAL_DELIVERY) == 0) {
    return LSX4_FEX_STATUS_UNSUPPORTED;
  }
  ThreadRecord *record = FindSignalTarget(runtime, delivery->target);
  if (record == nullptr || !EnqueueSignal(record, *delivery)) {
    return record == nullptr ? LSX4_FEX_STATUS_NOT_FOUND
                             : LSX4_FEX_STATUS_BUFFER_TOO_SMALL;
  }
  const bool current_thread =
      g_thread_binding.runtime == runtime && g_thread_binding.record == record;
  const uint64_t target_tid = record->bound_tid.load(std::memory_order_acquire);
  if (record->active.load(std::memory_order_acquire) && target_tid != 0) {
    const bool interrupt_armed = ArmGuestSignalInterrupt(record);
    if (!interrupt_armed) {
      return LSX4_FEX_STATUS_BACKEND_FAILURE;
    }
    if (current_thread) {
      return LSX4_FEX_STATUS_OK;
    }
    if (lsx4::fex2608::HostWakeThread(target_tid, kBridgeWakeSignal) != 0) {
      return LSX4_FEX_STATUS_BACKEND_FAILURE;
    }
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t
ReplaceExecutableRanges(void *opaque,
                        const Lsx4FexExecutableRangeSnapshotV5 *snapshot) {
  return ReplaceRanges(static_cast<Runtime *>(opaque), snapshot);
}

int32_t InvalidateExecutableRanges(void *opaque,
                                   const Lsx4FexExecutableRangeV5 *ranges,
                                   uint32_t range_count,
                                   uint32_t invalidate_flags) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || (range_count != 0 && ranges == nullptr) ||
      (invalidate_flags & ~(LSX4_FEX_RANGE_INVALIDATE_TRANSLATIONS |
                            LSX4_FEX_RANGE_REMOVE_REGISTRATIONS)) != 0) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  for (uint32_t index = 0; index < range_count; ++index) {
    if (ranges[index].size == 0 ||
        AddOverflows(ranges[index].address, ranges[index].size)) {
      return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
  }
  if ((invalidate_flags & LSX4_FEX_RANGE_INVALIDATE_TRANSLATIONS) != 0) {
    /*
     * FEX requires its code-invalidation mutex to be exclusively held by the
     * frontend.  InvalidateCodeBuffersCodeRange only verifies that contract;
     * calling it unlocked makes try_lock acquire and strand the mutex, so the
     * next CompileRIP waits forever.  Invalidate both shared code buffers and
     * every persistent thread's L1/frontend range cache under one epoch.
     */
    std::lock_guard records_lock{runtime->records_mutex};
    auto &invalidation_mutex = runtime->context->GetCodeInvalidationMutex();
    std::unique_lock invalidation_lock{invalidation_mutex};
    for (uint32_t index = 0; index < range_count; ++index) {
      runtime->context->InvalidateCodeBuffersCodeRange(ranges[index].address,
                                                       ranges[index].size);
      for (auto *record : runtime->records) {
        if (record != nullptr && record->thread != nullptr) {
          runtime->context->InvalidateThreadCachedCodeRange(
              record->thread, ranges[index].address, ranges[index].size);
        }
      }
    }
  }
  if ((invalidate_flags & LSX4_FEX_RANGE_REMOVE_REGISTRATIONS) != 0) {
    std::lock_guard lock{runtime->range_update_mutex};
    const ExecutableRangeStore *current =
        runtime->executable_ranges.load(std::memory_order_acquire);
    std::vector<Lsx4FexExecutableRangeV5> replacement =
        current != nullptr ? current->ranges
                           : std::vector<Lsx4FexExecutableRangeV5>{};
    for (uint32_t index = 0; index < range_count; ++index) {
      const uint64_t begin = ranges[index].address;
      const uint64_t end = begin + ranges[index].size;
      replacement.erase(std::remove_if(replacement.begin(), replacement.end(),
                                       [begin, end](const auto &registered) {
                                         return registered.address < end &&
                                                begin < registered.address +
                                                            registered.size;
                                       }),
                        replacement.end());
    }
    auto *published = new (std::nothrow)
        ExecutableRangeStore{(current != nullptr ? current->generation : 0) + 1,
                             std::move(replacement)};
    if (published == nullptr) {
      return LSX4_FEX_STATUS_BACKEND_FAILURE;
    }
    runtime->executable_ranges.store(published, std::memory_order_release);
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t QueryRuntime(void *opaque, Lsx4Fex2608RuntimeInfoV2 *info) {
  auto *runtime = static_cast<Runtime *>(opaque);
  constexpr size_t kLegacyRuntimeInfoSize =
      offsetof(Lsx4Fex2608RuntimeInfoV2, published_executable_blocks);
  if (runtime == nullptr || info == nullptr ||
      info->struct_size < kLegacyRuntimeInfoSize ||
      info->abi_version != LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const uint32_t capacity = info->struct_size;
  const size_t writable = std::min<size_t>(capacity, sizeof(*info));
  std::memset(info, 0, writable);
  info->struct_size = static_cast<uint32_t>(writable);
  info->abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  info->status = LSX4_FEX_STATUS_OK;
  info->strong_memory_mode = runtime->strong_memory_mode;
  info->runtime_features = runtime->runtime_features;
  info->diagnostics = runtime->diagnostics;
  const ExecutableRangeStore *ranges =
      runtime->executable_ranges.load(std::memory_order_acquire);
  info->executable_range_generation =
      ranges != nullptr ? ranges->generation : 0;
  info->active_guest_threads =
      runtime->active_threads.load(std::memory_order_relaxed);
  info->strong_memory_name =
      MemoryModelName(runtime->strong_memory_mode, runtime->create_flags);
  info->status_message = (runtime->runtime_features &
                          LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER) != 0
                             ? kSignalOwnerActive
                             : kSignalOwnerUnavailable;
  if (writable >=
      offsetof(Lsx4Fex2608RuntimeInfoV2, published_executable_blocks) +
          sizeof(info->published_executable_blocks)) {
    info->published_executable_blocks =
        g_executable_registry_store.published_block_count();
  }
  if (writable >= sizeof(*info)) {
    auto *context_impl =
        static_cast<FEXCore::Context::ContextImpl *>(runtime->context);
    const auto cache = context_impl->SnapshotTelemetry();
    const auto jit = context_impl->SnapshotJITTelemetry();
    info->code_cache_generation = cache.Generation;
    info->code_cache_retained_rotations = cache.RetainedRotations;
    info->code_cache_generation_resets = cache.GenerationResets;
    info->code_cache_current_segments = cache.CurrentSegments;
    info->code_cache_retired_generations = cache.RetiredGenerations;
    info->code_cache_retired_segments = cache.RetiredSegments;
    info->code_cache_used_bytes = cache.CurrentUsedBytes;
    info->code_cache_capacity_bytes = cache.CurrentCapacityBytes;
    info->code_cache_total_allocated_bytes = cache.TotalAllocatedBytes;
    info->jit_cache_misses = jit.CacheMisses;
    info->jit_compilations = jit.Compilations;
    info->jit_compile_time_ns = jit.CompileTimeNs;
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t ClassifySignalContextPc(void *opaque, const void *raw_context) {
#if defined(__aarch64__)
  struct ErrnoRestorer {
    int saved;
    ~ErrnoRestorer() { errno = saved; }
  } errno_restorer{errno};


  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || raw_context == nullptr ||
      (runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER) == 0) {
    return 0;
  }
  const ThreadBinding binding = FindSignalBinding(CurrentTid());
  ThreadRecord *record = binding.record;
  if (binding.runtime != runtime || record == nullptr ||
      record->thread == nullptr ||
      !record->active.load(std::memory_order_acquire)) {
    return 0;
  }

  const auto *context = static_cast<const ucontext_t *>(raw_context);
  const auto *frame = record->thread->CurrentFrame;
  if (frame == nullptr ||
      lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 28) != reinterpret_cast<uint64_t>(frame)) {
    return 0;
  }

  const uintptr_t host_pc = lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context));
  ExecutableRegistryReader executable_reader;
  if (!executable_reader.active()) {
    return 0;
  }
  const auto published_block =
      FindPublishedExecutableBlock(executable_reader, host_pc);
  // This API answers only whether the interrupted PC belongs to current,
  // published JIT code.  The registry already publishes exact block bounds
  // and retires them through its signal-safe reader protocol, so decoding the
  // guest-RIP metadata here adds no provenance information.  In particular,
  // PageManager calls this path for every watched-page write fault and does
  // not consume a guest RIP.  Keep the full metadata decode in
  // DecodeSignalContextGuestRip and in HandleHostSignal, where it is needed.
  return published_block.has_value() ? 1 : 0;
#else
  (void)opaque;
  (void)raw_context;
  return 0;
#endif
}

uint64_t DecodeSignalContextGuestRip(void *opaque,
                                     const void *raw_context) noexcept {
#if defined(__aarch64__)
  struct ErrnoRestorer {
    int saved;
    ~ErrnoRestorer() { errno = saved; }
  } errno_restorer{errno};


  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || raw_context == nullptr ||
      (runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER) == 0) {
    return 0;
  }
  const ThreadBinding binding = FindSignalBinding(CurrentTid());
  ThreadRecord *record = binding.record;
  if (binding.runtime != runtime || record == nullptr ||
      record->thread == nullptr ||
      !record->active.load(std::memory_order_acquire)) {
    return 0;
  }

  const auto *context = static_cast<const ucontext_t *>(raw_context);
  const auto *frame = record->thread->CurrentFrame;
  if (frame == nullptr ||
      lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 28) != reinterpret_cast<uint64_t>(frame)) {
    return 0;
  }

  const uintptr_t host_pc = lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context));
  ExecutableRegistryReader executable_reader;
  if (!executable_reader.active()) {
    return 0;
  }
  const auto published_block =
      FindPublishedExecutableBlock(executable_reader, host_pc);
  if (!published_block.has_value()) {
    return 0;
  }
  FEXCore::ArchHelpers::Arm64::ExactJITCodeRange exact_code_range{};
  return DecodeCurrentJitGuestRip(record, context, host_pc, *published_block,
                                  &exact_code_range)
      .value_or(0);
#else
  (void)opaque;
  (void)raw_context;
  return 0;
#endif
}

int32_t HandleHostSignal(void *opaque, int32_t signum, void *signal_info,
                         void *raw_context) {
#if defined(__aarch64__)
  struct ErrnoRestorer {
    int saved;
    ~ErrnoRestorer() { errno = saved; }
  } errno_restorer{errno};
  siginfo_t platform_info{};
  if (signal_info != nullptr) {
    platform_info = *static_cast<siginfo_t*>(signal_info);
    lsx4::fex2608::HostNormalizeMemoryFault(signum, platform_info, static_cast<const ucontext_t*>(raw_context));
    signal_info = &platform_info;
  }


  auto *runtime = static_cast<Runtime *>(opaque);
  const ThreadBinding signal_binding = FindSignalBinding(CurrentTid());
  const bool kernel_bus_adraln =
      runtime != nullptr &&
      (runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER) != 0 &&
      signum == SIGBUS && signal_info != nullptr &&
      static_cast<siginfo_t *>(signal_info)->si_signo == SIGBUS &&
      static_cast<siginfo_t *>(signal_info)->si_code == BUS_ADRALN;
  if (runtime == nullptr ||
      (runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER) == 0 ||
      (signum != SIGSEGV && signum != SIGBUS && signum != SIGILL &&
       signum != SIGTRAP && signum != kBridgeWakeSignal) ||
      raw_context == nullptr || !lsx4::fex2608::HostContextIsSelfContained(static_cast<const ucontext_t*>(raw_context)) || signal_binding.runtime != runtime ||
      signal_binding.record == nullptr) {
    if (kernel_bus_adraln) {
      RecordBusAdralnReject(
          runtime,
          raw_context == nullptr
              ? lsx4::fex2608::BusAdralnRejectReason::InvalidOwnerContext
              : lsx4::fex2608::BusAdralnRejectReason::SignalBinding);
    }
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }
  ThreadRecord *record = signal_binding.record;
  if (record->thread == nullptr) {
    if (kernel_bus_adraln) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::ThreadMissing);
    }
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }
  if (!record->active.load(std::memory_order_acquire)) {
    if (kernel_bus_adraln) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::ThreadInactive);
    }
    return signum == kBridgeWakeSignal ? LSX4_FEX2608_HOST_SIGNAL_HANDLED
                                        : LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }

  // FEX intentionally terminates the temporary compilation buffer with a
  // protected page.  Its Arm64 emitter can touch that page when the initial
  // SSA-size estimate is too small; the frontend must convert this exact
  // fault into the pre-established JIT restart, before applying guest-fault
  // ownership or published-code tests.  This is a host compiler fault, so its
  // PC is neither dispatcher nor published JIT code.
  if (IsOwnedGuardPageFault(signum, static_cast<const siginfo_t*>(signal_info))) {
    const uintptr_t fault_address = reinterpret_cast<uintptr_t>(
        static_cast<siginfo_t *>(signal_info)->si_addr);
    if (lsx4::fex2608::IsAddressInArm64JitGuardPage(
            record->thread->JITGuardPage, fault_address,
            g_process_page_size)) {
      auto *context = static_cast<ucontext_t *>(raw_context);
      if (!lsx4::fex2608::HostLoadJumpBuffer(context, record->thread->RestartJump,
                                            record->thread->JITGuardOverflowArgument)) {
        return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
      }
      return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
    }
  }

  auto &deferred = record->deferred_signal;
  const uint32_t deferred_phase =
      deferred.phase.load(std::memory_order_acquire);
  const bool deferred_helper_active =
      deferred_phase == DeferredPhase(DeferredSignalPhase::HelperArmed);
  if (deferred_helper_active) {
    if (signum == kBridgeWakeSignal) {
      return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
    }
    const bool hardware_signal =
        signal_info != nullptr &&
        static_cast<siginfo_t *>(signal_info)->si_signo == signum &&
        lsx4::fex2608::HostIsHardwareSignalCode(static_cast<siginfo_t *>(signal_info)->si_code);
    if (hardware_signal) {
      // Never jump across a FEX/C++ helper frame. An unexpected synchronous
      // helper fault may follow lock acquisition or a partial multi-address
      // RMW, so the only truthful response is to release the range lease and
      // terminate. Valid guest page faults must be found by normal-context
      // operand preflight before the helper is entered.
      (void)AbortDeferredSignalLanding(record);
      TerminateOnNestedHostFault(signum,
                                 FailClosedExit::FaultInsideDeferredHelper);
    }
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }

  // FEX's call/return predictor is intentionally a best-effort shadow cache:
  // guest calls and returns are not required to remain balanced.  The stock
  // Linux frontend handles either guarded end of this allocation by restoring
  // the live AArch64 x25 cache pointer and retrying the faulting instruction.
  // Keep the same frontend contract here.  Only the interrupted ucontext is
  // changed; clearing memory or touching CpuState from a signal handler would
  // be both unnecessary and unsafe.  Restrict recovery to our two PROT_NONE
  // guard pages so an unrelated access inside the usable mapping still fails
  // closed instead of retrying forever.
  if (IsOwnedGuardPageFault(signum, static_cast<const siginfo_t*>(signal_info)) &&
      IsCallRetStackGuardAddress(
          record, runtime->page_size,
          reinterpret_cast<uintptr_t>(
              static_cast<siginfo_t *>(signal_info)->si_addr))) {
    constexpr size_t kCallRetStackPointerRegister = 25;
    lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(static_cast<ucontext_t *>(raw_context)), kCallRetStackPointerRegister) =
        record->callret_reset_sp;
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }

#if defined(__aarch64__)
  const uint64_t deferred_trap_address =
      reinterpret_cast<uint64_t>(&lsx4_fex2608_deferred_signal_resume_trap);
  const uint64_t signal_pc =
      lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(static_cast<ucontext_t *>(raw_context)));
  const int32_t trap_code = signal_info != nullptr
                                ? static_cast<siginfo_t *>(signal_info)->si_code
                                : 0;
  const bool deferred_restore_trap =
      signum == SIGTRAP && signal_info != nullptr &&
      static_cast<siginfo_t *>(signal_info)->si_signo == SIGTRAP &&
      lsx4::fex2608::HostIsBreakpointCode(trap_code) &&
      (signal_pc == deferred_trap_address ||
       signal_pc == deferred_trap_address + sizeof(uint32_t)) &&
      *reinterpret_cast<const uint32_t *>(deferred_trap_address) ==
          kDeferredSignalResumeBrk;
  if (deferred_restore_trap) {
    const bool resume_original =
        deferred_phase == DeferredPhase(DeferredSignalPhase::ResumeOriginal);
    const bool resume_signal_capture =
        deferred_phase ==
        DeferredPhase(DeferredSignalPhase::ResumeSignalCapture);
    const bool resume = resume_original || resume_signal_capture;
    if (!resume) {
      (void)AbortDeferredSignalLanding(record);
      TerminateOnNestedHostFault(signum,
                                 FailClosedExit::InvalidDeferredResumeTrap);
    }
    auto *trap_context = static_cast<ucontext_t *>(raw_context);
    lsx4::fex2608::HostContext(trap_context) = deferred.interrupted_context;
    std::memcpy(&trap_context->uc_sigmask, &deferred.interrupted_mask,
                sizeof(trap_context->uc_sigmask));
    const bool resume_original_without_bridge_pin =
        resume_original &&
        !record->deferred_pin_release_interrupt.load(
            std::memory_order_acquire) &&
        deferred.pinned_block.lease.slot == nullptr;
    if (resume_signal_capture || resume_original_without_bridge_pin) {
      deferred.phase.store(DeferredPhase(DeferredSignalPhase::Idle),
                           std::memory_order_release);
    }
    // Keep the pin through kernel sigreturn and subsequent execution inside
    // the block. It is released only at a later signal observed outside the
    // block, or by the explicit retire gateway above.
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }
#endif
  // Userspace-generated signals (SI_USER/SI_TKILL/SI_QUEUE and friends) are
  // not guest synchronous faults. Reject them before touching even one byte
  // of ucontext; the outer signal owner may route them elsewhere.
  if (signum != kBridgeWakeSignal &&
      (signal_info == nullptr ||
       static_cast<siginfo_t *>(signal_info)->si_signo != signum ||
       !lsx4::fex2608::HostIsHardwareSignalCode(static_cast<siginfo_t *>(signal_info)->si_code))) {
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }

  auto &capture = record->host_signal;
  if (signum == kBridgeWakeSignal &&
      !record->interrupt_page_armed.load(std::memory_order_acquire) &&
      !record->normal_interrupt_pending.load(std::memory_order_acquire) &&
      !record->deferred_pin_release_interrupt.load(std::memory_order_acquire) &&
      lsx4::fex2608::DeferredWriteEpochDepth(&record->deferred_write_epochs) == 0) {
    // A remote sender may reach pthread_kill after the protected-page path has
    // already drained its queue. Do not unwind a guest signal callback for
    // that stale notification; it could skip transactional frame validation.
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }
  if (signum == kBridgeWakeSignal &&
      capture.gate.phase.load(std::memory_order_acquire) !=
          static_cast<uint32_t>(lsx4::fex2608::SignalCapturePhase::Idle)) {
    // The protected-page interrupt and its asynchronous wake can arrive in
    // either order. A pending capture already owns the exact unwind context;
    // retain queue work without replacing it or treating this wake as a new
    // synchronous fault. An armed interrupt page remains the liveness fallback.
    record->normal_interrupt_pending.store(true, std::memory_order_release);
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }
  const auto handler_entry = lsx4::fex2608::EnterSignalHandler(&capture.gate);
  if (handler_entry != lsx4::fex2608::SignalHandlerEntry::Primary) {
    // A returning previous handler would resume the same faulting instruction
    // in this handler or overwrite a pending exact context.  The finite bound
    // is therefore enforced with a raw process exit, never recursion, locks,
    // allocation, logging, or a false HANDLED result.
    TerminateOnNestedHostFault(signum, FailClosedExit::SignalCaptureReentry);
  }
  ScopedPrimarySignalHandler handler_guard{&capture.gate};

  auto *context = static_cast<ucontext_t *>(raw_context);
  const uint64_t host_pc = lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context));
  const uint64_t fault_address =
      signum != kBridgeWakeSignal && signal_info != nullptr
          ? reinterpret_cast<uint64_t>(
                static_cast<siginfo_t *>(signal_info)->si_addr)
          : 0;
  const bool interrupt_page_fault =
      IsGuestSignalInterruptFault(record, signum, fault_address) &&
      record->interrupt_page_armed.load(std::memory_order_acquire);
  const auto &config = runtime->signal_delegator->GetConfig();
  const bool in_dispatcher =
      host_pc >= config.DispatcherBegin && host_pc < config.DispatcherEnd;
  auto *frame = record->thread->CurrentFrame;
  if (frame == nullptr) {
    if (kernel_bus_adraln) {
      RecordBusAdralnReject(runtime,
                            lsx4::fex2608::BusAdralnRejectReason::FrameMissing);
    }
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }
  // Keep this reader lease alive through exact code/metadata reads and capture
  // publication.  A generated safepoint store and FEX's host-side deferred
  // interrupt probe use the same AArch64 encoding, so published JIT ownership
  // must be established before considering the host-helper probe fallback.
  ExecutableRegistryReader executable_reader;
  const std::optional<PublishedExecutableBlock> published_block =
      !in_dispatcher && executable_reader.active()
          ? FindPublishedExecutableBlock(executable_reader, host_pc)
          : std::nullopt;
  const bool deferred_probe_fault =
      interrupt_page_fault &&
      (in_dispatcher ||
       (executable_reader.active() && !published_block.has_value())) &&
      signum == SIGSEGV &&
      (static_cast<siginfo_t *>(signal_info)->si_code == SEGV_ACCERR ||
       static_cast<siginfo_t *>(signal_info)->si_code == SEGV_MAPERR) &&
      fault_address ==
          reinterpret_cast<uint64_t>(record->thread->InterruptFaultPage) &&
      IsDeferredInterruptProbeStore(context, fault_address);
  if (deferred_probe_fault) {
    if (frame->State.DeferredSignalRefCount.Load() != 0) {
      // This is an inner DeferredSignalRefCountGuard. Match FEX's native
      // protocol: skip only this exact probe and let the outermost guard probe
      // the still-protected page again.
      lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)) += sizeof(uint32_t);
      return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
    }
    record->normal_interrupt_pending.store(true, std::memory_order_release);
    // Skip this exact zero-store while keeping the page protected. If the
    // helper returns to generated code, its next safepoint faults and unwinds
    // through ThreadStop; if ExecuteThread returns first, the outer loop
    // disarms the page and drains the queue in normal context.
    lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)) += sizeof(uint32_t);
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }
  const bool frame_matches =
      lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 28) == reinterpret_cast<uint64_t>(frame);
  // The dispatcher probes InterruptFaultPage before its prologue has loaded
  // the SRA frame register. The exact armed page plus the published dispatcher
  // extent identifies that one safe early landing; every JIT or synthetic
  // fault still requires x28 to match the bound thread's CurrentFrame.
  if (!frame_matches && !(interrupt_page_fault && in_dispatcher)) {
    if (kernel_bus_adraln) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::FrameMismatch);
    }
    return signum == kBridgeWakeSignal ? LSX4_FEX2608_HOST_SIGNAL_HANDLED
                                       : LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }
  FEXCore::ArchHelpers::Arm64::ExactJITCodeRange exact_code_range{};
  std::optional<uint64_t> decoded_guest_rip =
      published_block.has_value()
          ? DecodeCurrentJitGuestRip(record, context, host_pc, *published_block,
                                     &exact_code_range)
          : std::nullopt;
  if (!decoded_guest_rip.has_value() && kernel_bus_adraln &&
      published_block.has_value()) {
    /*
     * The tail and RIP table are immutable after block publication. A
     * concurrent half-barrier rewrite only holds SpinLockFutex while changing
     * instructions. The exact published block lease and the classifier/pin
     * below are the synchronization contract for that code race, so retry the
     * metadata decode without treating the busy lock as corruption.
     */
    decoded_guest_rip = DecodeCurrentJitGuestRip(
        record, context, host_pc, *published_block, &exact_code_range, true);
    if (decoded_guest_rip.has_value()) {
      RecordBusAdralnBusyMetadataRecovery(runtime);
    }
  }
  const bool in_jit = decoded_guest_rip.has_value();
  if (signum == kBridgeWakeSignal) {
    if ((!in_jit && !in_dispatcher) ||
        (in_jit &&
         AddressDefersPendingInterrupt(runtime, *decoded_guest_rip))) {
      // A host helper cannot be unwound asynchronously. Its outer deferred
      // guard will observe the protected page; HLE trampoline blocks use the
      // same rule to preserve their native call frame.
      return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
    }
    capture.signum = signum;
    capture.signal_code = signal_info != nullptr
                              ? static_cast<siginfo_t *>(signal_info)->si_code
                              : 0;
    capture.was_in_jit = in_jit ? 1U : 0U;
    capture.generated_guest_fault = 0;
    capture.fault_address = 0;
    capture.host_pc = host_pc;
    capture.host_sp = lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context));
    capture.guest_rip = decoded_guest_rip.value_or(0);
    capture.synchronous_fault_data = 0;
    std::memcpy(&capture.saved_signal_mask, &context->uc_sigmask,
                sizeof(capture.saved_signal_mask));
    lsx4::fex2608::HostSignalMask redirected_mask = capture.saved_signal_mask;
    for (const int owned_signal :
         {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, kBridgeWakeSignal}) {
      redirected_mask |= UINT64_C(1) << (owned_signal - 1);
    }
    std::memcpy(&context->uc_sigmask, &redirected_mask,
                sizeof(redirected_mask));
    handler_guard.Publish();
    lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context)) = frame->ReturningStackLocation;
    lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 28) = reinterpret_cast<uint64_t>(frame);
    lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)) = in_jit ? config.ThreadStopHandlerAddressSpillSRA
                                     : config.ThreadStopHandlerAddress;
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }
  // FEX's SIGBUS alignment trap is an internal atomic-emulation protocol, not
  // a guest signal. Admit only a classified instruction whose complete helper
  // footprint is covered by the frontend guest-VMA lease below. Shared JIT
  // backpatch races additionally require an exact published pair and pin.
  if (in_jit && kernel_bus_adraln) {
    // Pin the exact immutable block before the Reader lease is released, then
    // return into a dedicated normal-context landing.  No FEX helper, lock,
    // logging path, guest memory access, allocation or code mutation occurs in
    // the signal handler.
    if (AddressDefersPendingInterrupt(runtime, *decoded_guest_rip)) {
      RecordBusAdralnReject(
          runtime,
          lsx4::fex2608::BusAdralnRejectReason::GuestRangeDefersInterrupt);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    if (!lsx4::fex2608::HostContextIsSelfContained(context)) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::ContextRecordChain);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    const auto *atomic_pc = reinterpret_cast<const uint32_t *>(host_pc);
    const size_t instruction_before =
        (host_pc - exact_code_range.BlockBegin) / sizeof(uint32_t);
    const size_t instruction_after =
        (exact_code_range.InlineTail - host_pc) / sizeof(uint32_t);
    std::array<uint64_t, 31> interrupted_gprs{};
    for (size_t i = 0; i < 31; ++i) interrupted_gprs[i] = lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), i);
    const uint32_t published_instruction =
        lsx4::fex2608::LoadPublishedInstruction(atomic_pc);
    std::optional<lsx4::fex2608::SignalAtomicSite> atomic_site =
        lsx4::fex2608::DecodeDirectSignalAtomicSite(published_instruction,
                                                    interrupted_gprs.data());
    bool observed_backpatched = false;
    if (!atomic_site.has_value()) {
      atomic_site = lsx4::fex2608::DecodeBackpatchedHalfBarrierSite(
          atomic_pc, instruction_before, instruction_after,
          interrupted_gprs.data());
      observed_backpatched = atomic_site.has_value();
    }
    if (!atomic_site.has_value()) {
      atomic_site = lsx4::fex2608::DecodeExclusiveSignalAtomicSite(
          atomic_pc, instruction_before, instruction_after,
          interrupted_gprs.data());
    }
    if (!atomic_site.has_value()) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::AtomicClassifier);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    if (atomic_site->address != fault_address) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::FaultAddressMismatch);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    if (runtime->callbacks.acquire_guest_memory_lease == nullptr ||
        runtime->callbacks.release_guest_memory_lease == nullptr) {
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::MemoryLeaseCallbacks);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    const auto pinned = published_block.has_value()
                            ? g_executable_registry_store.TryPin(
                                  executable_reader.native(), *published_block)
                            : std::nullopt;
    if (!pinned.has_value()) {
      RecordBusAdralnReject(runtime,
                            lsx4::fex2608::BusAdralnRejectReason::BlockPin);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    if (observed_backpatched &&
        g_executable_registry_store.PinnedKind(*pinned) !=
            lsx4::fex2608::ExecutableRangeKind::SharedJIT) {
      auto mutable_pin = *pinned;
      g_executable_registry_store.ReleasePin(&mutable_pin);
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::AtomicClassifier);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    auto &deferred_bus = record->deferred_signal;
    const DeferredLandingClaim landing_claim =
        lsx4::fex2608::ClaimDeferredLanding(
            &deferred_bus.phase, &record->deferred_pin_release_interrupt);
    if (landing_claim == DeferredLandingClaim::PinHandoff) {
      // The current Reader lease and new pin were acquired before releasing
      // the preceding block, so retirement cannot open a lifetime gap here.
      // Keep InterruptFaultPage armed; the new helper reuses it and publishes
      // a fresh pending-release token for its own pin.
      ReleaseDeferredPin(record);
    } else if (landing_claim != DeferredLandingClaim::Fresh) {
      auto mutable_pin = *pinned;
      g_executable_registry_store.ReleasePin(&mutable_pin);
      (void)AbortDeferredSignalLanding(record);
      TerminateOnNestedHostFault(signum, FailClosedExit::ConcurrentBusLanding);
    }
    deferred_bus.pinned_block = *pinned;
    deferred_bus.interrupted_context = lsx4::fex2608::HostContext(context);
    std::memcpy(&deferred_bus.interrupted_mask, &context->uc_sigmask,
                sizeof(deferred_bus.interrupted_mask));
    deferred_bus.working_gprs = interrupted_gprs;
    deferred_bus.exact_code_range = exact_code_range;
    deferred_bus.atomic_site = *atomic_site;
    deferred_bus.working_pstate = lsx4::fex2608::HostPstate(lsx4::fex2608::HostContext(context));
    deferred_bus.guest_rip = *decoded_guest_rip;
    deferred_bus.original_host_pc = host_pc;
    deferred_bus.thread_stop_handler_address_spill_sra =
        config.ThreadStopHandlerAddressSpillSRA;
    sigset_t landing_mask = deferred_bus.interrupted_mask;
    sigdelset(&landing_mask, SIGSEGV);
    sigdelset(&landing_mask, SIGBUS);
    sigdelset(&landing_mask, SIGTRAP);
    std::memcpy(&context->uc_sigmask, &landing_mask,
                sizeof(context->uc_sigmask));
    lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 0) = reinterpret_cast<uint64_t>(record);
    lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context)) =
        (deferred_bus.original_host_pc != 0 &&
         record->deferred_stack_base != 0 && record->deferred_stack_size >= 16)
            ? (record->deferred_stack_base + record->deferred_stack_size) &
                  ~UINT64_C(15)
            : 0;
    lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)) =
        reinterpret_cast<uint64_t>(&lsx4_fex2608_deferred_signal_landing);
    if (lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context)) == 0) {
      ReleaseDeferredPin(record);
      deferred_bus.phase.store(DeferredPhase(DeferredSignalPhase::Idle),
                               std::memory_order_release);
      RecordBusAdralnReject(
          runtime, lsx4::fex2608::BusAdralnRejectReason::DeferredStack);
      return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
    }
    return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
  }

  const auto &sync_fault = frame->SynchronousFaultData;
  const bool exact_guest_landing =
      sync_fault.FaultToTopAndGeneratedException &&
      ((host_pc == config.GuestSignalSIGILLFaultAddress && signum == SIGILL &&
        static_cast<siginfo_t *>(signal_info)->si_code == ILL_ILLOPC &&
        sync_fault.Signal == SIGILL) ||
       (host_pc == config.GuestSignalSIGTRAPFaultAddress && signum == SIGTRAP &&
        static_cast<siginfo_t *>(signal_info)->si_code == TRAP_BRKPT &&
        sync_fault.Signal == SIGTRAP) ||
       (host_pc == config.GuestSignalSIGSEGVFaultAddress && signum == SIGSEGV &&
        static_cast<siginfo_t *>(signal_info)->si_code == SEGV_MAPERR &&
        sync_fault.Signal == SIGSEGV));
  const int32_t host_signal_code =
      static_cast<siginfo_t *>(signal_info)->si_code;
  const bool deliverable_jit_fault =
      in_jit && ((signum == SIGSEGV && (host_signal_code == SEGV_MAPERR ||
                                        host_signal_code == SEGV_ACCERR)) ||
                 (signum == SIGBUS && (host_signal_code == BUS_ADRERR ||
                                       host_signal_code == BUS_OBJERR
#if !defined(__APPLE__)
                                       || host_signal_code == BUS_MCEERR_AR
#endif
)));
  // Dispatcher state is accepted only at the interrupt page or one exact
  // synthetic guest-signal instruction with matching SynchronousFaultData.
  // Debug/assert BRKs, ARM64EC 0xCAFE and arbitrary dispatcher faults reject.
  const bool dispatcher_interrupt = in_dispatcher && interrupt_page_fault;
  if (!deliverable_jit_fault && !dispatcher_interrupt && !exact_guest_landing) {
    if (kernel_bus_adraln) {
      const auto reason =
          !executable_reader.active()
              ? lsx4::fex2608::BusAdralnRejectReason::ExecutableReaderInactive
          : !published_block.has_value()
              ? lsx4::fex2608::BusAdralnRejectReason::PublishedBlockMissing
              : lsx4::fex2608::BusAdralnRejectReason::MetadataDecodeFailed;
      RecordBusAdralnReject(runtime, reason);
    }
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }

  if (interrupt_page_fault &&
      ::mprotect( record->thread->InterruptFaultPage,
                sizeof(record->thread->InterruptFaultPage),
                PROT_READ | PROT_WRITE) != 0) {
    return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
  }
  if (interrupt_page_fault) {
    record->interrupt_page_armed.store(false, std::memory_order_release);
  }
  capture.signum = signum;
  capture.signal_code = exact_guest_landing
                            ? sync_fault.si_code
                            : static_cast<siginfo_t *>(signal_info)->si_code;
  capture.was_in_jit = deliverable_jit_fault ? 1U : 0U;
  capture.generated_guest_fault = exact_guest_landing ? 1U : 0U;
  capture.fault_address = fault_address;
  capture.host_pc = host_pc;
  capture.host_sp = lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context));
  capture.guest_rip =
      decoded_guest_rip.value_or(exact_guest_landing ? frame->State.rip : 0);
  std::memcpy(&capture.synchronous_fault_data, &sync_fault,
              sizeof(capture.synchronous_fault_data));
  std::memcpy(&capture.saved_signal_mask, &context->uc_sigmask,
              sizeof(capture.saved_signal_mask));

  lsx4::fex2608::HostSignalMask redirected_mask = capture.saved_signal_mask;
  for (const int owned_signal :
       {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, kBridgeWakeSignal}) {
    redirected_mask |= UINT64_C(1) << (owned_signal - 1);
  }
  std::memcpy(&context->uc_sigmask, &redirected_mask, sizeof(redirected_mask));
  handler_guard.Publish();

  lsx4::fex2608::HostSp(lsx4::fex2608::HostContext(context)) = frame->ReturningStackLocation;
  lsx4::fex2608::HostGpr(lsx4::fex2608::HostContext(context), 28) = reinterpret_cast<uint64_t>(frame);
  lsx4::fex2608::HostPc(lsx4::fex2608::HostContext(context)) = in_jit ? config.ThreadStopHandlerAddressSpillSRA
                                   : config.ThreadStopHandlerAddress;
  return LSX4_FEX2608_HOST_SIGNAL_HANDLED;
#else
  (void)opaque;
  (void)signum;
  (void)signal_info;
  (void)raw_context;
  return LSX4_FEX2608_HOST_SIGNAL_NOT_HANDLED;
#endif
}

bool AllocateSentinels(Runtime *runtime) {
  runtime->sentinel_allocation = FEXCore::Allocator::mmap(
      nullptr, runtime->page_size, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (runtime->sentinel_allocation == MAP_FAILED) {
    runtime->sentinel_allocation = nullptr;
    return false;
  }
  auto *bytes = static_cast<uint8_t *>(runtime->sentinel_allocation);
  runtime->exit_sentinel = reinterpret_cast<uint64_t>(bytes);
  runtime->callback_return_sentinel = reinterpret_cast<uint64_t>(bytes + 16);
  runtime->abort_stack_pointer = reinterpret_cast<uint64_t>(bytes + 32);
  bytes[0] = UINT8_C(0xf4);  // HLT: staged private execution exit.
  bytes[16] = UINT8_C(0x0f); // FEX CALLBACKRET pseudo-instruction.
  bytes[17] = UINT8_C(0x3e);
  std::memcpy(bytes + 32, &runtime->exit_sentinel,
              sizeof(runtime->exit_sentinel));
#if defined(__APPLE__)
  // These are x86 bytes consumed by the translator, never native ARM code.
  constexpr int sentinel_protection = PROT_READ;
#else
  constexpr int sentinel_protection = PROT_READ | PROT_EXEC;
#endif
  if (::mprotect(bytes, runtime->page_size, sentinel_protection) != 0) {
    FEXCore::Allocator::munmap(bytes, runtime->page_size);
    runtime->sentinel_allocation = nullptr;
    return false;
  }
  return true;
}

int32_t CreateRuntime(const Lsx4Fex2608RuntimeCreateRequestV2 *request,
                      Lsx4Fex2608RuntimeCreateResultV2 *result) {
  constexpr size_t kV2BaseRequestSize =
      offsetof(Lsx4Fex2608RuntimeCreateRequestV2, guest_cpu_profile_v6);
  constexpr size_t kGuestCpuProfileRequestSize =
      offsetof(Lsx4Fex2608RuntimeCreateRequestV2, guest_cpu_profile_v6) +
      sizeof(const Lsx4FexGuestCpuProfileV6 *);
  const Lsx4FexGuestCpuProfileV6 *guest_cpu_profile =
      request != nullptr && request->struct_size >= kGuestCpuProfileRequestSize
          ? request->guest_cpu_profile_v6
          : nullptr;
  constexpr size_t kCpuTuningRequestSize =
      offsetof(Lsx4Fex2608RuntimeCreateRequestV2, cpu_tuning_v1) +
      sizeof(const Lsx4FexCpuTuningV1 *);
  const Lsx4FexCpuTuningV1 default_cpu_tuning = BalancedCpuTuning();
  const Lsx4FexCpuTuningV1 *cpu_tuning =
      request != nullptr && request->struct_size >= kCpuTuningRequestSize &&
              request->cpu_tuning_v1 != nullptr
          ? request->cpu_tuning_v1
          : &default_cpu_tuning;
  const uint32_t requested_create_flags =
      request != nullptr ? request->create_flags : 0;
  const uint32_t requested_owner_flags =
      requested_create_flags & kSignalOwnerCreateFlags;
  const uint32_t requested_memory_model_flags =
      requested_create_flags & kMemoryModelAbCreateFlags;
  const bool persistent_rip_seed_requested =
      (requested_create_flags &
       LSX4_FEX2608_CREATE_PERSISTENT_RIP_SEED_AB) != 0;
  if (request == nullptr || result == nullptr ||
      request->struct_size < kV2BaseRequestSize ||
      request->abi_version != LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2 ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2 ||
      !IsHostFeatureRequestValid(&request->host_features) ||
      request->reserved != 0 ||
      (request->create_flags &
       ~(kSignalOwnerCreateFlags | kMemoryModelAbCreateFlags |
         kCodegenAbCreateFlags)) != 0 ||
      requested_owner_flags == kSignalOwnerCreateFlags ||
      ((request->create_flags & LSX4_FEX2608_CREATE_EXACT_MULTIBLOCK_AB) != 0 &&
       requested_owner_flags != LSX4_FEX2608_CREATE_FULL_HOST_SIGNAL_OWNER) ||
      ((request->create_flags & LSX4_FEX2608_CREATE_MAXINST_512_AB) != 0 &&
       (request->create_flags & LSX4_FEX2608_CREATE_EXACT_MULTIBLOCK_AB) == 0) ||
      ((request->create_flags & LSX4_FEX2608_CREATE_MAXINST_256_AB) != 0 &&
       (request->create_flags & LSX4_FEX2608_CREATE_EXACT_MULTIBLOCK_AB) == 0) ||
      ((request->create_flags & LSX4_FEX2608_CREATE_MAXINST_512_AB) != 0 &&
       (request->create_flags & LSX4_FEX2608_CREATE_MAXINST_256_AB) != 0) ||
      request->strong_memory_mode < LSX4_FEX2608_STRONG_MEMORY_SOFTWARE_TSO ||
      request->strong_memory_mode > LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO ||
      (request->strong_memory_mode == LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO &&
       (request->host_os_features & LSX4_FEX_HOST_OS_HARDWARE_TSO_ENABLED) ==
           0) ||
      (requested_memory_model_flags != 0 &&
       request->strong_memory_mode ==
           LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO) ||
      request->host_callbacks == nullptr ||
      request->host_callbacks->struct_size < sizeof(Lsx4FexHostCallbacksV3) ||
      request->host_callbacks->abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
      request->host_callbacks->dispatch_hle == nullptr ||
      ((request->create_flags & LSX4_FEX2608_CREATE_FULL_HOST_SIGNAL_OWNER) !=
           0 &&
       (request->host_callbacks->acquire_guest_memory_lease == nullptr ||
        request->host_callbacks->release_guest_memory_lease == nullptr)) ||
      request->hle_thunk_identity == nullptr ||
      request->hle_thunk_identity_size != kThunkIdentitySize ||
      !IsGuestCpuProfileValid(guest_cpu_profile) ||
      !IsCpuTuningValid(cpu_tuning) ||
      (persistent_rip_seed_requested &&
       (request->struct_size < sizeof(*request) ||
        request->runtime_root == nullptr || request->runtime_root[0] == '\0' ||
        request->cache_namespace == nullptr ||
        request->cache_namespace[0] == '\0' ||
        request->host_build_id == nullptr ||
        request->host_build_id[0] == '\0'))) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }

  uint32_t normalized_create_flags = requested_create_flags;
  if (requested_owner_flags == 0) {
    normalized_create_flags |= LSX4_FEX2608_CREATE_STAGED_PRIVATE_HLT_EXIT;
  }
  const bool full_signal_owner =
      (normalized_create_flags & LSX4_FEX2608_CREATE_FULL_HOST_SIGNAL_OWNER) !=
      0;
  const uint64_t requested_runtime_features =
      RuntimeFeaturesForCreateFlags(normalized_create_flags);
  const uint32_t result_capacity = result->struct_size;
  const uint64_t cookie = request->request_cookie;
  std::memset(result, 0, sizeof(*result));
  result->struct_size = result_capacity;
  result->abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  result->request_cookie = cookie;
  result->runtime_features = requested_runtime_features;
  result->diagnostics = kRuntimeDiagnostics;
  result->strong_memory_mode = request->strong_memory_mode;
  if (request->strong_memory_mode == LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO) {
    const HardwareTsoAttempt attempt = TryEnableHardwareTsoForCurrentThread();
    LogHardwareTsoAttempt(request->host_callbacks, "create-runtime", attempt);
    // This call proves capability only.  Guest execution is guarded by the
    // mandatory per-bind enable above, so don't contaminate native children.
    RestoreHardwareTsoAfterProbe(request->host_callbacks, "create-runtime",
                                 attempt);
    if (!attempt.enabled) {
      result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
      result->error_message = kHardwareTsoEnableFailed;
      return result->status;
    }
  }
  if (!EnsureProcessInitialized(request->host_features.page_size)) {
    result->status = LSX4_FEX_STATUS_INVALID_STATE;
    result->error_message =
        "FEX process page size changed after initialization";
    return result->status;
  }

  auto *runtime = new (std::nothrow) Runtime{};
  if (runtime == nullptr) {
    result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->error_message = "FEX runtime allocation failed";
    return result->status;
  }
  runtime->page_size = request->host_features.page_size;
  runtime->strong_memory_mode = request->strong_memory_mode;
  runtime->create_flags = normalized_create_flags;
  runtime->cpu_tuning = *cpu_tuning;
  runtime->runtime_features = requested_runtime_features;
  runtime->callbacks = *request->host_callbacks;
  if (persistent_rip_seed_requested) {
    runtime->runtime_root = request->runtime_root;
    runtime->cache_namespace = request->cache_namespace;
    runtime->host_build_id = request->host_build_id;
  }
  if (full_signal_owner) {
    runtime->bus_adraln_reject_fd = ::open(
        kBusAdralnRejectLogPath,
        O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    runtime->diagnostics = 0;
  }
  std::memcpy(runtime->thunk_identity.data(), request->hle_thunk_identity,
              runtime->thunk_identity.size());
  if (guest_cpu_profile != nullptr) {
    runtime->guest_cpu_profile = *guest_cpu_profile;
    std::strncpy(runtime->guest_cpu_profile_id.data(),
                 guest_cpu_profile->profile_id,
                 runtime->guest_cpu_profile_id.size() - 1);
    runtime->guest_cpu_profile.profile_id =
        runtime->guest_cpu_profile_id.data();
    runtime->exact_guest_cpu_profile = true;
  }
  runtime->guest_store_trace = lsx4::fex2608::GuestStoreTraceRequested();
  if (runtime->guest_store_trace) {
    lsx4::fex2608::ResetGuestStoreTrace();
  }
  runtime->guest_write_op_coverage =
      lsx4::fex2608::GuestWriteOpCoverageRequested();
  if (runtime->guest_write_op_coverage) {
    lsx4::fex2608::ResetGuestWriteOpCoverage();
  }
  runtime->exact_store_watch = lsx4::fex2608::ExactStoreWatchRequested();
  runtime->proof_metrics_mode =
      lsx4::fex2608::RequestedProofMetricsMode();
  runtime->proof_metrics_epoch =
      lsx4::fex2608::ResetProofMetrics(runtime->proof_metrics_mode);
  if (runtime->proof_metrics_mode !=
          LSX4_FEX2608_PROOF_METRICS_DISABLED &&
      (runtime->exact_store_watch || runtime->guest_store_trace ||
       runtime->guest_write_op_coverage)) {
    result->status = LSX4_FEX_STATUS_INVALID_ARGUMENT;
    result->error_message =
        "proof metrics cannot be combined with another IR/JIT diagnostic";
    return result->status;
  }
  runtime->range_context = request->range_context;
  runtime->snapshot_ranges = request->snapshot_executable_ranges;
  if (!AllocateSentinels(runtime)) {
    delete runtime;
    result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->error_message = "private FEX sentinel allocation failed";
    return result->status;
  }
  if (request->initial_executable_ranges != nullptr) {
    const int32_t range_status =
        ReplaceRanges(runtime, request->initial_executable_ranges);
    if (range_status != LSX4_FEX_STATUS_OK) {
      result->status = range_status;
      result->error_message = "initial executable-range snapshot is invalid";
      return result->status;
    }
  }

  auto created = FEXCore::Context::Context::CreateNewContext(
      BuildHostFeatures(request->host_features));
  if (!created) {
    result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->error_message = kContextCreationFailed;
    return result->status;
  }
  runtime->context = created.release();
  auto *context_impl =
      static_cast<FEXCore::Context::ContextImpl *>(runtime->context);
  const auto &tuning = runtime->cpu_tuning;
  const bool strict_atomics = tuning.atomics == 0;
  const bool fast_atomics = tuning.atomics == 2;
  const bool accurate_stores = tuning.accurate_cache_line_stores != 0;
  const bool safe_mapped_memory = tuning.fast_memory == 0 ||
                                  tuning.memory_manager == 2;
  static_cast<FEXCore::Config::Value<bool> &>(context_impl->Config.TSOEnabled) =
      FEXCore::Config::Value<bool>{true};
  static_cast<FEXCore::Config::Value<bool> &>(
      context_impl->Config.VectorTSOEnabled) =
      FEXCore::Config::Value<bool>{safe_mapped_memory || !fast_atomics ||
                                   accurate_stores};
  static_cast<FEXCore::Config::Value<bool> &>(
      context_impl->Config.MemcpySetTSOEnabled) =
      FEXCore::Config::Value<bool>{safe_mapped_memory || !fast_atomics ||
                                   accurate_stores};
  static_cast<FEXCore::Config::Value<bool> &>(
      context_impl->Config.StrictInProcessSplitLocks) =
      FEXCore::Config::Value<bool>{strict_atomics};
  static_cast<FEXCore::Config::Value<bool> &>(
      context_impl->Config.x87ReducedPrecision) =
      FEXCore::Config::Value<bool>{tuning.fp_accuracy != 0};
  context_impl->Config.FPAccuracyMode = tuning.fp_accuracy;
  using SmcType = typename decltype(context_impl->Config.SMCChecks)::OptionInfo::Type;
  const auto smc_mode = safe_mapped_memory || tuning.icache_accuracy != 0
                            ? FEXCore::Config::CONFIG_SMC_FULL
                        : tuning.memory_exceptions != 0
                            ? FEXCore::Config::CONFIG_SMC_MTRACK
                            : FEXCore::Config::CONFIG_SMC_NONE;
  static_cast<FEXCore::Config::Value<SmcType> &>(context_impl->Config.SMCChecks) =
      FEXCore::Config::Value<SmcType>{static_cast<SmcType>(smc_mode)};
  context_impl->Config.GuestClockPercent =
      tuning.force_real_clock_sync != 0 ? 100 : tuning.guest_clock_percent;
  context_impl->Config.CycleSkipDivisor =
      tuning.force_real_clock_sync != 0 || tuning.cycle_skip == 0
          ? 0
          : (tuning.cycle_skip == 1 ? 16 : (tuning.cycle_skip == 2 ? 8 : 4));
  context_impl->Config.SpinWaitMode = tuning.spin_loop_detection;
  context_impl->Config.PreserveTerminalThunkDynamicRegs =
      (runtime->create_flags &
       LSX4_FEX2608_CREATE_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB) != 0;
  if (!safe_mapped_memory && fast_atomics && !accurate_stores &&
      (runtime->create_flags & LSX4_FEX2608_CREATE_RELAX_VECTOR_TSO_AB) != 0) {
    static_cast<FEXCore::Config::Value<bool> &>(
        context_impl->Config.VectorTSOEnabled) =
        FEXCore::Config::Value<bool>{false};
  }
  if (!safe_mapped_memory && fast_atomics && !accurate_stores && (runtime->create_flags &
       LSX4_FEX2608_CREATE_RELAX_MEMCPY_SET_TSO_AB) != 0) {
    static_cast<FEXCore::Config::Value<bool> &>(
        context_impl->Config.MemcpySetTSOEnabled) =
        FEXCore::Config::Value<bool>{false};
  }
  const bool preserve_terminal_thunk_dynamic_regs_ab =
      (runtime->create_flags &
       LSX4_FEX2608_CREATE_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB) != 0;
  const bool preserve_typed_time_common_frame_ab =
      (runtime->create_flags &
       LSX4_FEX2608_CREATE_PRESERVE_TYPED_TIME_COMMON_FRAME_AB) != 0;
  if (preserve_terminal_thunk_dynamic_regs_ab ||
      preserve_typed_time_common_frame_ab ||
      runtime->exact_store_watch ||
      runtime->guest_store_trace ||
      runtime->guest_write_op_coverage ||
      runtime->proof_metrics_mode != LSX4_FEX2608_PROOF_METRICS_DISABLED) {
    static_cast<FEXCore::Config::Value<bool> &>(
        context_impl->CodeCache.EnableCodeCaching) =
        FEXCore::Config::Value<bool>{false};
    static_cast<FEXCore::Config::Value<bool> &>(
        context_impl->CodeCache.EnableLazyCodeCaching) =
        FEXCore::Config::Value<bool>{false};
#ifdef __ANDROID__
    if (preserve_terminal_thunk_dynamic_regs_ab) {
      __android_log_print(
          ANDROID_LOG_INFO, "LSX4Native",
          "[EXECUTOR_FEX_TERMINAL_THUNK_DYNAMIC_REGS] preserve=1 "
          "codeCache=off reason=same-binary-control");
    }
    if (preserve_typed_time_common_frame_ab) {
      __android_log_print(
          ANDROID_LOG_INFO, "LSX4Native",
          "[EXECUTOR_FEX_TYPED_TIME_COMMON_FRAME] preserve=1 outline=0 "
          "codeCache=off reason=same-binary-control");
    }
#endif
#ifdef __ANDROID__
    if (runtime->exact_store_watch) {
      __android_log_print(
          ANDROID_LOG_INFO, "LSX4Native",
          "[EXECUTOR_FEX_EXACT_STORE_WATCH_CACHE] marker=1 persistent=0 lazy=0");
    }
#endif
  }
  context_impl->Config.NeedsPendingInterruptFaultCheck = full_signal_owner;
  // Block linking and block geometry are independent controls.  Safe blocks
  // are deliberately short, but may still link their resolved exits directly
  // instead of returning through the dispatcher after every block.
  const bool exact_multiblock = full_signal_owner && tuning.block_linking != 0;
  static_cast<FEXCore::Config::Value<bool> &>(context_impl->Config.Multiblock) =
      FEXCore::Config::Value<bool>{exact_multiblock};
  // Safe is the retained Bloodborne D102 geometry.  It deliberately uses
  // half-sized exact multiblocks (256 versus Mega's 512): 128 was introduced
  // while wiring the generic CPU-tuning UI and discarded the device-proven
  // D102 profile, substantially increasing dispatch/translation overhead.
  const int32_t max_inst =
      tuning.jit_block_size == 0 ? 256 : (tuning.jit_block_size == 1 ? 512 : 5000);
  static_cast<FEXCore::Config::Value<int32_t> &>(
      context_impl->Config.MaxInstPerBlock) =
      FEXCore::Config::Value<int32_t>{max_inst};
  const bool maxinst_256 = max_inst == 256;
  const bool maxinst_512 = max_inst == 512;
  const bool cache_diagnostics_active =
      preserve_terminal_thunk_dynamic_regs_ab ||
      preserve_typed_time_common_frame_ab ||
      runtime->exact_store_watch || runtime->guest_store_trace ||
      runtime->guest_write_op_coverage ||
      runtime->proof_metrics_mode != LSX4_FEX2608_PROOF_METRICS_DISABLED;
  const bool persistent_cache = tuning.persistent_cache != 0 &&
                                !cache_diagnostics_active;
  static_cast<FEXCore::Config::Value<bool> &>(
      context_impl->CodeCache.EnableCodeCaching) =
      FEXCore::Config::Value<bool>{persistent_cache};
  static_cast<FEXCore::Config::Value<bool> &>(
      context_impl->CodeCache.EnableLazyCodeCaching) =
      FEXCore::Config::Value<bool>{persistent_cache};
#ifdef __ANDROID__
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_TYPED_TIME_COMMON_FRAME_EFFECTIVE] preserve=%d outline=%d "
      "scope=exact-typed-time",
      preserve_typed_time_common_frame_ab ? 1 : 0,
      preserve_typed_time_common_frame_ab ? 0 : 1);
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_EXACT_MULTIBLOCK_EFFECTIVE] requested=%d "
      "fullSignalOwner=%d metadataV1=1 effective=%d",
      (runtime->create_flags & LSX4_FEX2608_CREATE_EXACT_MULTIBLOCK_AB) != 0
          ? 1
          : 0,
      full_signal_owner ? 1 : 0, exact_multiblock ? 1 : 0);
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_MAXINST_512_EFFECTIVE] requested=%d exactMultiblock=%d "
      "effective=%d maxInst=%d",
      (runtime->create_flags & LSX4_FEX2608_CREATE_MAXINST_512_AB) != 0
          ? 1
          : 0,
      exact_multiblock ? 1 : 0, maxinst_512 ? 1 : 0,
      max_inst);
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_FEX_MAXINST_GEOMETRY_EFFECTIVE] requested512=%d requested256=%d "
      "exactMultiblock=%d effective512=%d effective256=%d maxInst=%d",
      (runtime->create_flags & LSX4_FEX2608_CREATE_MAXINST_512_AB) != 0 ? 1 : 0,
      (runtime->create_flags & LSX4_FEX2608_CREATE_MAXINST_256_AB) != 0 ? 1 : 0,
      exact_multiblock ? 1 : 0, maxinst_512 ? 1 : 0, maxinst_256 ? 1 : 0,
      max_inst);
  __android_log_print(
      ANDROID_LOG_INFO, "LSX4Native",
      "[EXECUTOR_CPU_TUNING_FEX] identity=0x%016" PRIx64
      " clock=%u cycleDiv=%u spin=%u fp=%u atomics=%u smc=%d cache=%d",
      tuning.identity, context_impl->Config.GuestClockPercent,
      context_impl->Config.CycleSkipDivisor, context_impl->Config.SpinWaitMode,
      tuning.fp_accuracy, tuning.atomics, static_cast<int>(smc_mode),
      persistent_cache ? 1 : 0);
#endif
  runtime->signal_delegator = new (std::nothrow)
      BridgeSignalDelegator{runtime->callback_return_sentinel};
  runtime->syscall_handler = new (std::nothrow) BridgeSyscallHandler{runtime};
  runtime->thunk_handler = new (std::nothrow) BridgeThunkHandler{runtime};
  if (runtime->signal_delegator == nullptr ||
      runtime->syscall_handler == nullptr ||
      runtime->thunk_handler == nullptr) {
    result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->error_message = "FEX runtime handler allocation failed";
    return result->status;
  }
  runtime->context->SetSignalDelegator(runtime->signal_delegator);
  runtime->context->SetSyscallHandler(runtime->syscall_handler);
  runtime->context->SetThunkHandler(runtime->thunk_handler);
  runtime->context->SetHardwareTSOSupport(
      request->strong_memory_mode == LSX4_FEX2608_STRONG_MEMORY_HARDWARE_TSO);
  if (!full_signal_owner) {
    runtime->context->EnableExitOnHLT();
  }
  if (!runtime->context->InitCore()) {
    result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->error_message = kCoreInitializationFailed;
    return result->status;
  }
  const int32_t range_status = RefreshRanges(runtime);
  if (range_status != LSX4_FEX_STATUS_OK) {
    result->status = range_status;
    result->error_message =
        "executable-range callback failed during activation";
    return result->status;
  }
  if (persistent_rip_seed_requested && tuning.persistent_cache != 0) {
    const auto *ranges =
        runtime->executable_ranges.load(std::memory_order_acquire);
    const bool initialized = runtime->persistent_rip_seed.Initialize(
        runtime->runtime_root.c_str(), runtime->cache_namespace.c_str(),
        runtime->host_build_id.c_str(), runtime->guest_cpu_profile_id.data(),
        runtime->thunk_identity.data(), runtime->thunk_identity.size(), ranges,
        tuning.persistent_cache == 2);
    if (initialized) {
      context_impl->SetCompiledBlockObserver(runtime,
                                             ObserveCompiledBlockForRipSeed);
    }
#ifdef __ANDROID__
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_FEX_PERSISTENT_RIP_SEED_EFFECTIVE] requested=1 "
        "effective=%d defaultOff=1 boundedEntries=%u prewarmLimit=%u "
        "prewarmBudgetMs=%lld nativeCodePersisted=0",
        initialized ? 1 : 0, kPersistentRipSeedCapacity,
        kPersistentRipSeedPrewarmLimit,
        static_cast<long long>(kPersistentRipSeedPrewarmBudget.count()));
#endif
  }

  result->status = LSX4_FEX_STATUS_OK;
  result->runtime = runtime;
  result->runtime_features = runtime->runtime_features;
  result->diagnostics = runtime->diagnostics;
  result->error_message = nullptr;
  if (full_signal_owner) {
    Log(runtime, 4, "bridge", kSignalOwnerActive);
  } else {
    Log(runtime, 4, "bridge", kSignalOwnerUnavailable);
  }
  if (IsRelaxedMemoryModelAb(runtime->create_flags)) {
    Log(runtime, 4, "memory-model-ab",
        MemoryModelName(runtime->strong_memory_mode, runtime->create_flags));
  }
  return LSX4_FEX_STATUS_OK;
}

const Lsx4Fex2608RuntimeBridgeApiV2 kRuntimeApi{
    .struct_size = sizeof(Lsx4Fex2608RuntimeBridgeApiV2),
    .abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2,
    .runtime_features = kRuntimeApiFeatures,
    .create_runtime = CreateRuntime,
    .run_mapped_entry = RunMappedEntry,
    .run_guest_callback = RunGuestCallback,
    .get_guest_context = GetGuestContext,
    .set_guest_context = SetGuestContext,
    .get_guest_tls = GetGuestTls,
    .set_guest_tls = SetGuestTls,
    .bind_host_thread = BindHostThread,
    .unbind_host_thread = UnbindHostThread,
    .register_guest_signal_action = RegisterGuestSignalAction,
    .queue_guest_signal = QueueGuestSignal,
    .replace_executable_ranges = ReplaceExecutableRanges,
    .invalidate_executable_ranges = InvalidateExecutableRanges,
    .query_runtime = QueryRuntime,
    .handle_host_signal = HandleHostSignal,
    .classify_signal_context_pc = ClassifySignalContextPc,
    .release_guest_thread = ReleaseGuestThread,
    .decode_signal_context_guest_rip = DecodeSignalContextGuestRip,
};

} // namespace

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_create_context_v1(const Lsx4Fex2608BridgeHostFeaturesV1 *request,
                               void **context, const char **error_message) {
  if (error_message != nullptr) {
    *error_message = nullptr;
  }
  if (!IsHostFeatureRequestValid(request) || context == nullptr ||
      !EnsureProcessInitialized(request->page_size)) {
    if (error_message != nullptr) {
      *error_message = kInvalidBridgeRequest;
    }
    return -1;
  }
  *context = nullptr;
  auto created =
      FEXCore::Context::Context::CreateNewContext(BuildHostFeatures(*request));
  if (!created) {
    if (error_message != nullptr) {
      *error_message = kContextCreationFailed;
    }
    return -2;
  }
  *context = created.release();
  return 0;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_get_runtime_bridge_api_v2(uint32_t requested_abi,
                                       uint32_t output_size,
                                       Lsx4Fex2608RuntimeBridgeApiV2 *output) {
  constexpr uint32_t kRequiredOutputSize =
      offsetof(Lsx4Fex2608RuntimeBridgeApiV2, decode_signal_context_guest_rip);
  if (requested_abi != LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2 ||
      output == nullptr || output_size < kRequiredOutputSize) {
    return LSX4_FEX_STATUS_ABI_MISMATCH;
  }
  std::memcpy(output, &kRuntimeApi,
              std::min<size_t>(output_size, sizeof(kRuntimeApi)));
  return LSX4_FEX_STATUS_OK;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_exact_store_watch_configure_v1(const uint64_t begin,
                                            const uint64_t end,
                                            const uint32_t flags) noexcept {
  return lsx4_fexcore_exact_store_watch_configure_v1(begin, end, flags);
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_exact_store_watch_take_v1(
    Lsx4Fex2608ExactStoreWatchHitV1 *const hit) noexcept {
  if (hit == nullptr || hit->struct_size < sizeof(*hit)) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  Lsx4FexExactStoreWatchHitV1 core_hit{
      .struct_size = sizeof(Lsx4FexExactStoreWatchHitV1),
  };
  const int32_t status = lsx4_fexcore_exact_store_watch_take_v1(&core_hit);
  if (status <= 0) {
    return status;
  }
  *hit = Lsx4Fex2608ExactStoreWatchHitV1{
      .struct_size = sizeof(Lsx4Fex2608ExactStoreWatchHitV1),
      .sequence = core_hit.sequence,
      .guest_rip = core_hit.guest_rip,
      .effective_address = core_hit.effective_address,
      .access_size = core_hit.access_size,
      .reserved = 0,
  };
  return 1;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_guest_store_trace_take_v1(
    Lsx4Fex2608GuestStoreTraceHitV1 *const hit) noexcept {
  return lsx4::fex2608::TakeGuestStoreTrace(hit);
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_proof_metrics_query_v1(
    void *const opaque,
    Lsx4Fex2608ProofMetricsV1 *const metrics) noexcept {
  auto *runtime = static_cast<Runtime *>(opaque);
  return runtime != nullptr
             ? lsx4::fex2608::QueryProofMetrics(
                   runtime->proof_metrics_epoch, metrics)
             : LSX4_FEX_STATUS_INVALID_ARGUMENT;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_deferred_write_epoch_configure_v1(
    void *const opaque,
    const Lsx4Fex2608DeferredWriteEpochCallbacksV1 *const callbacks) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr ||
      (runtime->runtime_features &
       LSX4_FEX2608_RUNTIME_HOST_FAULT_SIGNAL_OWNER) == 0) {
    return LSX4_FEX_STATUS_UNSUPPORTED;
  }
  if (runtime->active_threads.load(std::memory_order_acquire) != 0) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  if (callbacks == nullptr) {
    runtime->deferred_write_epoch_callbacks = {};
    return LSX4_FEX_STATUS_OK;
  }
  if (callbacks->struct_size < sizeof(*callbacks) ||
      callbacks->abi_version != LSX4_FEX2608_DEFERRED_WRITE_EPOCH_ABI_V1 ||
      callbacks->publish_ready == nullptr) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  runtime->deferred_write_epoch_callbacks = *callbacks;
  return LSX4_FEX_STATUS_OK;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_deferred_write_epoch_arm_current_v1(void *const opaque,
                                                 const uint64_t token) {
  struct ErrnoRestorer {
    int saved;
    ~ErrnoRestorer() { errno = saved; }
  } errno_restorer{errno};


  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr ||
      runtime->deferred_write_epoch_callbacks.publish_ready == nullptr) {
    if (runtime != nullptr) {
      runtime->deferred_write_epoch_unavailable_fallbacks.fetch_add(
          1, std::memory_order_relaxed);
    }
    return LSX4_FEX2608_DEFERRED_WRITE_EPOCH_FALLBACK;
  }
  const ThreadBinding binding = FindSignalBinding(CurrentTid());
  ThreadRecord *const record = binding.record;
  if (binding.runtime != runtime || record == nullptr ||
      record->thread == nullptr ||
      !record->active.load(std::memory_order_acquire)) {
    runtime->deferred_write_epoch_unavailable_fallbacks.fetch_add(
        1, std::memory_order_relaxed);
    return LSX4_FEX2608_DEFERRED_WRITE_EPOCH_FALLBACK;
  }
  const bool was_queue_empty = lsx4::fex2608::DeferredWriteEpochDepth(
                                   &record->deferred_write_epochs) == 0;
  if (!lsx4::fex2608::TryArmDeferredWriteEpoch(&record->deferred_write_epochs,
                                               token)) {
    runtime->deferred_write_epoch_overflow_fallbacks.fetch_add(
        1, std::memory_order_relaxed);
    return LSX4_FEX2608_DEFERRED_WRITE_EPOCH_FALLBACK;
  }
  /*
   * Publish the token before arming the interrupt.  The owner is stopped in
   * this fault handler, so it cannot consume the queue until the interrupt
   * page is protected.  Arming first allowed the interrupt to drain an empty
   * queue and left the subsequently published token permanently faulted.
   */
  if (!ArmDeferredWriteEpochInterrupt(record, was_queue_empty)) {
    const bool rolled_back = lsx4::fex2608::TryRollbackDeferredWriteEpoch(
        &record->deferred_write_epochs, token);
    runtime->deferred_write_epoch_interrupt_arm_failures.fetch_add(
        1, std::memory_order_relaxed);
    if (!rolled_back) {
      runtime->deferred_write_epoch_overflow_fallbacks.fetch_add(
          1, std::memory_order_relaxed);
    }
    return LSX4_FEX2608_DEFERRED_WRITE_EPOCH_FALLBACK;
  }
  runtime->deferred_write_epoch_armed.fetch_add(1, std::memory_order_relaxed);
  return LSX4_FEX2608_DEFERRED_WRITE_EPOCH_ARMED;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_deferred_write_epoch_flush_current_v1(void *const opaque) {
  auto *runtime = static_cast<Runtime *>(opaque);
  ThreadRecord *const record = CurrentRecord(runtime, nullptr, true);
  if (runtime == nullptr || record == nullptr) {
    return LSX4_FEX_STATUS_INVALID_STATE;
  }
  return static_cast<int32_t>(PublishDeferredWriteEpochs(runtime, record));
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_deferred_write_epoch_kick_owner_v1(
    void *const opaque, const int64_t host_tid) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || host_tid <= 0 ||
      host_tid > std::numeric_limits<pid_t>::max()) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const ThreadBinding binding =
      FindSignalBinding(static_cast<pid_t>(host_tid));
  ThreadRecord *const record = binding.record;
  if (binding.runtime != runtime || record == nullptr ||
      record->thread == nullptr ||
      !record->active.load(std::memory_order_acquire) ||
      record->bound_tid.load(std::memory_order_acquire) !=
          static_cast<uint64_t>(host_tid)) {
    return LSX4_FEX_STATUS_NOT_FOUND;
  }
  if (lsx4::fex2608::DeferredWriteEpochDepth(
          &record->deferred_write_epochs) == 0) {
    return LSX4_FEX_STATUS_OK;
  }
  return lsx4::fex2608::HostWakeThread(host_tid, kBridgeWakeSignal) == 0
             ? LSX4_FEX_STATUS_OK
             : LSX4_FEX_STATUS_BACKEND_FAILURE;
}

extern "C" __attribute__((visibility("default"))) int32_t
lsx4_fex2608_deferred_write_epoch_query_v1(
    void *const opaque, Lsx4Fex2608DeferredWriteEpochStatsV1 *const stats) {
  auto *runtime = static_cast<Runtime *>(opaque);
  if (runtime == nullptr || stats == nullptr ||
      stats->struct_size < sizeof(*stats) ||
      stats->abi_version != LSX4_FEX2608_DEFERRED_WRITE_EPOCH_ABI_V1) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const uint64_t armed =
      runtime->deferred_write_epoch_armed.load(std::memory_order_acquire);
  const uint64_t promoted =
      runtime->deferred_write_epoch_promoted.load(std::memory_order_acquire);
  const uint32_t capacity = stats->struct_size;
  *stats = Lsx4Fex2608DeferredWriteEpochStatsV1{
      .struct_size = capacity,
      .abi_version = LSX4_FEX2608_DEFERRED_WRITE_EPOCH_ABI_V1,
      .armed = armed,
      .promoted = promoted,
      .overflow_fallbacks =
          runtime->deferred_write_epoch_overflow_fallbacks.load(
              std::memory_order_relaxed),
      .unavailable_fallbacks =
          runtime->deferred_write_epoch_unavailable_fallbacks.load(
              std::memory_order_relaxed),
      .interrupt_arm_failures =
          runtime->deferred_write_epoch_interrupt_arm_failures.load(
              std::memory_order_relaxed),
      .pending = armed >= promoted ? armed - promoted : 0,
  };
  return LSX4_FEX_STATUS_OK;
}
