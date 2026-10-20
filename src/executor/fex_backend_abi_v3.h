// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef LSX4_EXECUTOR_FEX_BACKEND_ABI_V3_H
#define LSX4_EXECUTOR_FEX_BACKEND_ABI_V3_H

#include <stddef.h>
#include <stdint.h>

#define LSX4_FEX_BACKEND_ABI_V3 UINT32_C(3)
#define LSX4_FEX_MAPPED_ENTRY_ABI_V5 UINT32_C(5)
#define LSX4_FEX_EXECUTABLE_RANGE_ABI_V5 UINT32_C(5)
#define LSX4_FEX_BACKEND_GET_API_V3_SYMBOL "lsx4_fex_backend_get_api_v3"

#define LSX4_FEX_HLE_GPR_ARGUMENT_COUNT UINT32_C(6)
#define LSX4_FEX_HLE_XMM_ARGUMENT_COUNT UINT32_C(8)
#define LSX4_FEX_HLE_GPR_RESULT_COUNT UINT32_C(2)
#define LSX4_FEX_HLE_XMM_RESULT_COUNT UINT32_C(2)
#define LSX4_FEX_GUEST_GPR_COUNT UINT32_C(16)
#define LSX4_FEX_GUEST_XMM_COUNT UINT32_C(16)














#ifdef __cplusplus
extern "C" {
#endif

typedef enum Lsx4FexStatusV3 {
    LSX4_FEX_STATUS_OK = 0,
    LSX4_FEX_STATUS_INVALID_ARGUMENT = -1,
    LSX4_FEX_STATUS_ABI_MISMATCH = -2,
    LSX4_FEX_STATUS_MISSING_CAPABILITY = -3,
    LSX4_FEX_STATUS_INVALID_STATE = -4,
    LSX4_FEX_STATUS_NOT_FOUND = -5,
    LSX4_FEX_STATUS_BUFFER_TOO_SMALL = -6,
    LSX4_FEX_STATUS_UNSUPPORTED = -7,
    LSX4_FEX_STATUS_BACKEND_FAILURE = -8,
} Lsx4FexStatusV3;

typedef enum Lsx4FexLifecycleStateV3 {
    LSX4_FEX_LIFECYCLE_UNCONFIGURED = 0,
    LSX4_FEX_LIFECYCLE_CONFIGURED = 1,
    LSX4_FEX_LIFECYCLE_ACTIVE = 2,
    LSX4_FEX_LIFECYCLE_FAILED = 3,
} Lsx4FexLifecycleStateV3;

typedef enum Lsx4FexSysVClassV3 {
    LSX4_FEX_SYSV_NO_CLASS = 0,
    LSX4_FEX_SYSV_INTEGER = 1,
    LSX4_FEX_SYSV_SSE = 2,
    LSX4_FEX_SYSV_SSEUP = 3,
    LSX4_FEX_SYSV_X87 = 4,
    LSX4_FEX_SYSV_X87UP = 5,
    LSX4_FEX_SYSV_COMPLEX_X87 = 6,
    LSX4_FEX_SYSV_MEMORY = 7,
} Lsx4FexSysVClassV3;

enum {
    LSX4_FEX_PLATFORM_PS4 = UINT32_C(1) << 0,
    LSX4_FEX_PLATFORM_PS5 = UINT32_C(1) << 1,
};

#define LSX4_FEX_CPU_TUNING_ABI_V1 UINT32_C(1)

typedef struct Lsx4FexCpuTuningV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t preset;
    uint32_t execution_mode;
    uint32_t fast_memory;
    uint32_t memory_manager;
    uint32_t block_linking;
    uint32_t persistent_cache;
    uint32_t function_replacements;
    uint32_t guest_clock_percent;
    uint32_t cycle_skip;
    uint32_t idle_loop_skip;
    uint32_t wait_loop_detection;
    uint32_t spin_loop_detection;
    uint32_t max_guest_workers;
    uint32_t fast_event_timing;
    uint32_t jit_block_size;
    uint32_t fp_accuracy;
    uint32_t atomics;
    uint32_t accurate_cache_line_stores;
    uint32_t accurate_dma_timing;
    uint32_t memory_exceptions;
    uint32_t icache_accuracy;
    uint32_t ignore_bad_accesses;
    uint32_t jit_feature_mask;
    uint32_t jit_disable_mask;
    uint32_t cpu_gpu_dual_thread;
    uint32_t scheduler;
    uint32_t preferred_heavy_threads;
    uint32_t subsystem_multithreading;
    uint32_t separate_io_thread;
    uint32_t separate_audio_hle_thread;
    uint32_t guest_thread_delay_mode;
    uint32_t guest_thread_delay_us;
    uint32_t sync_on_idle_skip;
    uint32_t cpu_gpu_sync;
    uint32_t force_real_clock_sync;
    uint32_t hle_mode;
    uint32_t hle_fast_timers;
    uint32_t per_game_profile;
    uint32_t auto_game_db;

    uint32_t reserved0;
    uint64_t identity;
} Lsx4FexCpuTuningV1;

enum {
    LSX4_FEX_HOST_ISA_LSE = UINT64_C(1) << 0,
    LSX4_FEX_HOST_ISA_FLAGM = UINT64_C(1) << 1,
    LSX4_FEX_HOST_ISA_FLAGM2 = UINT64_C(1) << 2,
    LSX4_FEX_HOST_ISA_LRCPC = UINT64_C(1) << 3,
    LSX4_FEX_HOST_ISA_ILRCPC = UINT64_C(1) << 4,
    LSX4_FEX_HOST_ISA_LRCPC3 = UINT64_C(1) << 5,
    LSX4_FEX_HOST_ISA_SVE = UINT64_C(1) << 6,
    LSX4_FEX_HOST_ISA_SVE2 = UINT64_C(1) << 7,
    LSX4_FEX_HOST_ISA_AES = UINT64_C(1) << 8,
    LSX4_FEX_HOST_ISA_PMULL = UINT64_C(1) << 9,
    LSX4_FEX_HOST_ISA_SHA1 = UINT64_C(1) << 10,
    LSX4_FEX_HOST_ISA_SHA2 = UINT64_C(1) << 11,
    LSX4_FEX_HOST_ISA_CRC32 = UINT64_C(1) << 12,
    LSX4_FEX_HOST_ISA_AFP = UINT64_C(1) << 13,
    LSX4_FEX_HOST_ISA_FCMA = UINT64_C(1) << 14,
    LSX4_FEX_HOST_ISA_FRINTTS = UINT64_C(1) << 15,
    LSX4_FEX_HOST_ISA_ECV = UINT64_C(1) << 16,
    LSX4_FEX_HOST_ISA_RPRES = UINT64_C(1) << 17,
};

enum {
    LSX4_FEX_HOST_OS_SVE_CONTEXT = UINT64_C(1) << 0,
    LSX4_FEX_HOST_OS_HARDWARE_TSO_ENABLED = UINT64_C(1) << 1,
};

enum {
    LSX4_FEX_CAP_MAPPED_ENTRY_V5 = UINT64_C(1) << 0,
    LSX4_FEX_CAP_HLE_FRAME_V3 = UINT64_C(1) << 1,
    LSX4_FEX_CAP_STRONG_MEMORY = UINT64_C(1) << 2,
    LSX4_FEX_CAP_GUEST_SIGNALS = UINT64_C(1) << 3,
    LSX4_FEX_CAP_GUEST_TLS = UINT64_C(1) << 4,
    LSX4_FEX_CAP_GUEST_CALLBACKS = UINT64_C(1) << 5,
    LSX4_FEX_CAP_GUEST_FUTEX_SCHEDULING = UINT64_C(1) << 6,
    LSX4_FEX_CAP_SELF_MODIFYING_CODE = UINT64_C(1) << 7,
    LSX4_FEX_CAP_EXECUTABLE_RANGE_V5 = UINT64_C(1) << 8,
    LSX4_FEX_CAP_PROCESS_LIFETIME = UINT64_C(1) << 9,
    LSX4_FEX_CAP_SINGLE_SIGNAL_OWNER = UINT64_C(1) << 10,





    LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6 = UINT64_C(1) << 11,






    LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES = UINT64_C(1) << 12,
};

enum {
    LSX4_FEX_LIFETIME_PROCESS_RESIDENT = UINT32_C(1) << 0,
    LSX4_FEX_LIFETIME_ACTIVATE_ONCE = UINT32_C(1) << 1,
    LSX4_FEX_LIFETIME_NO_DEACTIVATE = UINT32_C(1) << 2,
    LSX4_FEX_LIFETIME_SINGLE_SIGNAL_OWNER = UINT32_C(1) << 3,
};

enum {
    LSX4_FEX_SRET_PRESENT = UINT8_C(1) << 0,
    LSX4_FEX_SRET_RAX_RETURNS_ADDRESS = UINT8_C(1) << 1,
    LSX4_FEX_SRET_HOST_WROTE_MEMORY = UINT8_C(1) << 2,
};

enum {
    LSX4_FEX_CALL_VARIADIC = UINT32_C(1) << 0,
    LSX4_FEX_CALL_NORETURN = UINT32_C(1) << 1,
};

enum {





    LSX4_FEX_HLE_RESULT_GUEST_EXIT = UINT32_C(1) << 0,
};

enum {
    LSX4_FEX_CONTEXT_GPR_VALID = UINT32_C(1) << 0,
    LSX4_FEX_CONTEXT_XMM_VALID = UINT32_C(1) << 1,
    LSX4_FEX_CONTEXT_FLAGS_VALID = UINT32_C(1) << 2,
    LSX4_FEX_CONTEXT_TLS_VALID = UINT32_C(1) << 3,
};

enum {
    LSX4_FEX_RANGE_EXECUTABLE = UINT32_C(1) << 0,
    LSX4_FEX_RANGE_WRITABLE = UINT32_C(1) << 1,
    LSX4_FEX_RANGE_HLE_TRAMPOLINE = UINT32_C(1) << 2,
    LSX4_FEX_RANGE_SELF_MODIFYING = UINT32_C(1) << 3,
    LSX4_FEX_RANGE_HLE_FRAME_POOL = UINT32_C(1) << 4,

    LSX4_FEX_RANGE_PERSISTENT_MODULE = UINT32_C(1) << 5,
};

enum {
    LSX4_FEX_RANGE_INVALIDATE_TRANSLATIONS = UINT32_C(1) << 0,
    LSX4_FEX_RANGE_REMOVE_REGISTRATIONS = UINT32_C(1) << 1,
};

enum {
    LSX4_FEX_SIGNAL_USE_EXPLICIT_CONTEXT = UINT32_C(1) << 0,
    LSX4_FEX_SIGNAL_SYNCHRONOUS = UINT32_C(1) << 1,





    LSX4_FEX_SIGNAL_CONTEXT_ARG1 = UINT32_C(1) << 2,
};

typedef enum Lsx4FexHostSignalDispositionV3 {
    LSX4_FEX_HOST_SIGNAL_NOT_HANDLED = 0,
    LSX4_FEX_HOST_SIGNAL_HANDLED = 1,
} Lsx4FexHostSignalDispositionV3;

typedef struct Lsx4FexXmm128 {
    uint64_t low;
    uint64_t high;
} Lsx4FexXmm128;










typedef struct Lsx4FexHleCallFrameV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t call_id;
    uint64_t native_function;
    uint64_t guest_rip;
    uint64_t guest_rsp;
    uint64_t gpr[LSX4_FEX_HLE_GPR_ARGUMENT_COUNT];
    Lsx4FexXmm128 xmm[LSX4_FEX_HLE_XMM_ARGUMENT_COUNT];
    uint64_t sret_address;
    uint32_t sret_size;
    uint8_t return_class[2];
    uint8_t return_eightbyte_count;
    uint8_t sret_flags;
    uint32_t call_flags;
    uint32_t sysv_sse_arg_count;
    uint64_t abi_descriptor_id;
    uint64_t input_rax;
} Lsx4FexHleCallFrameV3;

typedef struct Lsx4FexHleResultFrameV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t call_id;
    uint64_t rax;
    uint64_t rdx;
    Lsx4FexXmm128 xmm0;
    Lsx4FexXmm128 xmm1;
    uint64_t sret_address;
    uint32_t sret_size;
    uint8_t return_class[2];
    uint8_t return_eightbyte_count;
    uint8_t sret_flags;
    uint32_t result_flags;
    int32_t status;
    uint64_t abi_descriptor_id;
} Lsx4FexHleResultFrameV3;

typedef struct Lsx4FexThreadIdentityV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t guest_thread;
    uint64_t pthread_handle;
    int64_t host_tid;
    uint64_t backend_thread;
} Lsx4FexThreadIdentityV3;

typedef struct Lsx4FexGuestContextV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t gpr[LSX4_FEX_GUEST_GPR_COUNT];
    uint64_t rip;
    uint64_t rflags;
    uint64_t fs_base;
    uint64_t gs_base;
    Lsx4FexXmm128 xmm[LSX4_FEX_GUEST_XMM_COUNT];
    uint32_t mxcsr;
    uint32_t context_flags;
    uint64_t guest_thread;
} Lsx4FexGuestContextV3;

typedef struct Lsx4FexGuestSignalActionV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t signum;
    uint32_t action_flags;
    uint64_t handler;





    uint64_t restorer;

    uint64_t mask_low;
    uint64_t mask_high;
} Lsx4FexGuestSignalActionV3;

typedef struct Lsx4FexGuestSignalDeliveryV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    Lsx4FexThreadIdentityV3 target;
    int32_t signum;
    uint32_t delivery_flags;
    uint64_t explicit_rip;
    uint64_t explicit_rsp;
    const Lsx4FexGuestContextV3* explicit_context;
} Lsx4FexGuestSignalDeliveryV3;

typedef struct Lsx4FexGuestTlsStateV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    Lsx4FexThreadIdentityV3 target;
    uint64_t fs_base;
    uint64_t gs_base;
    uint64_t tcb_base;
    uint64_t tls_image_address;
    uint64_t tls_image_size;
    uint64_t tls_module_id;
    uint32_t tls_flags;
    uint32_t reserved;
} Lsx4FexGuestTlsStateV3;

typedef struct Lsx4FexGuestCallbackRequestV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t callback_id;
    uint64_t guest_function;
    uint64_t guest_rsp;
    uint64_t gpr[LSX4_FEX_HLE_GPR_ARGUMENT_COUNT];
    Lsx4FexXmm128 xmm[LSX4_FEX_HLE_XMM_ARGUMENT_COUNT];
    uint64_t sret_address;
    uint32_t sret_size;
    uint8_t return_class[2];
    uint8_t return_eightbyte_count;
    uint8_t sret_flags;
    uint32_t call_flags;
    uint32_t sysv_sse_arg_count;
    uint64_t abi_descriptor_id;
    uint64_t input_rax;
} Lsx4FexGuestCallbackRequestV3;



typedef Lsx4FexHleResultFrameV3 Lsx4FexGuestCallbackResultV3;

typedef struct Lsx4FexMappedSegmentV5 {
    uint64_t address;
    uint64_t size;
    uint64_t range_cookie;
    uint32_t protection;
    uint32_t range_flags;
} Lsx4FexMappedSegmentV5;

typedef struct Lsx4FexExecutableRangeV5 {
    uint64_t address;
    uint64_t size;
    uint64_t range_cookie;
    uint32_t protection;
    uint32_t range_flags;
} Lsx4FexExecutableRangeV5;

typedef struct Lsx4FexExecutableRangeSnapshotV5 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    Lsx4FexExecutableRangeV5* ranges;
    uint32_t capacity;
    uint32_t range_count;
    uint32_t snapshot_flags;
    uint32_t reserved;
} Lsx4FexExecutableRangeSnapshotV5;









typedef struct Lsx4FexMappedEntryRequestV5 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t request_flags;
    uint32_t timeout_ms;
    uint64_t entry;
    uint64_t proc_param;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t tls_image_address;
    uint64_t tls_image_size;
    uint64_t tls_module_id;
    uint64_t tcb_base;
    uint64_t entry_params;
    uint64_t exit_function;
    uint64_t hle_trampoline_base;
    uint64_t hle_trampoline_size;
    const Lsx4FexMappedSegmentV5* segments;
    uint32_t segment_count;
    uint32_t reserved;
    const Lsx4FexExecutableRangeSnapshotV5* executable_ranges;
    uint64_t request_cookie;
} Lsx4FexMappedEntryRequestV5;

typedef struct Lsx4FexMappedEntryResultV5 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t status;
    uint32_t result_flags;
    uint64_t rax;
    uint64_t final_rip;
    uint64_t guest_exit_code;
    uint64_t request_cookie;
} Lsx4FexMappedEntryResultV5;

typedef void (*Lsx4FexLogCallbackV3)(void* callback_context, int32_t priority,
                                     const char* component, const char* message);
typedef int32_t (*Lsx4FexHleDispatchCallbackV3)(
    void* callback_context, const Lsx4FexHleCallFrameV3* call,
    Lsx4FexHleResultFrameV3* result);






typedef int32_t (*Lsx4FexTryLeafHleDispatchCallbackV3)(
    void* callback_context, uint64_t native_function, uint64_t guest_rip,
    uint64_t guest_rsp, const uint64_t gpr[LSX4_FEX_HLE_GPR_ARGUMENT_COUNT],
    uint64_t* result_rax);







typedef int32_t (*Lsx4FexTryTypedTimeHleDispatchCallbackV3)(
    void* callback_context, uint64_t native_function, uint64_t guest_rip,
    uint64_t guest_rsp, const uint64_t gpr[LSX4_FEX_HLE_GPR_ARGUMENT_COUNT],
    uint64_t* result_rax);







typedef int32_t (*Lsx4FexTryScalarFpHleDispatchCallbackV3)(
    void* callback_context, uint64_t native_function, uint64_t guest_rip,
    uint64_t guest_rsp, const uint64_t gpr[LSX4_FEX_HLE_GPR_ARGUMENT_COUNT],
    const Lsx4FexXmm128 xmm[2], Lsx4FexXmm128* result_xmm0);
typedef void (*Lsx4FexThreadEventCallbackV3)(
    void* callback_context, uint32_t event, const Lsx4FexThreadIdentityV3* thread);
typedef void (*Lsx4FexFatalErrorCallbackV3)(void* callback_context, int32_t status,
                                            const char* message);

enum {
    LSX4_FEX_GUEST_MEMORY_ACCESS_READ = UINT32_C(1) << 0,
    LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE = UINT32_C(1) << 1,
    LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY = UINT32_C(1) << 2,
};










typedef int32_t (*Lsx4FexAcquireGuestMemoryLeaseV3)(
    void* callback_context, uint64_t guest_address, uint64_t size,
    uint32_t access_flags, uint64_t* lease_token);
typedef void (*Lsx4FexReleaseGuestMemoryLeaseV3)(
    void* callback_context, uint64_t lease_token);

typedef struct Lsx4FexHostCallbacksV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* callback_context;
    Lsx4FexLogCallbackV3 log;
    Lsx4FexHleDispatchCallbackV3 dispatch_hle;
    Lsx4FexThreadEventCallbackV3 thread_event;
    Lsx4FexFatalErrorCallbackV3 fatal_error;
    Lsx4FexAcquireGuestMemoryLeaseV3 acquire_guest_memory_lease;
    Lsx4FexReleaseGuestMemoryLeaseV3 release_guest_memory_lease;
    Lsx4FexTryLeafHleDispatchCallbackV3 try_dispatch_leaf_hle;
    Lsx4FexTryTypedTimeHleDispatchCallbackV3 try_dispatch_typed_time_hle;
    Lsx4FexTryScalarFpHleDispatchCallbackV3 try_dispatch_scalar_fp_hle;
} Lsx4FexHostCallbacksV3;

#define LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6 UINT32_C(6)

typedef struct Lsx4FexCpuidResultV6 {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} Lsx4FexCpuidResultV6;

typedef Lsx4FexCpuidResultV6 (*Lsx4FexQueryCpuidFnV6)(
    const void* profile_context, uint32_t leaf, uint32_t subleaf);
typedef uint64_t (*Lsx4FexQueryXcrFnV6)(const void* profile_context,
                                        uint32_t index);







typedef struct Lsx4FexGuestCpuProfileV6 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t identity_low;
    uint64_t identity_high;
    const char* profile_id;
    const void* profile_context;
    Lsx4FexQueryCpuidFnV6 query_cpuid;
    Lsx4FexQueryXcrFnV6 query_xcr;
    uint64_t xcr0_writable_mask;
    uint32_t mxcsr_reset_value;
    uint32_t mxcsr_mask;
    uint32_t legacy_save_area_bytes;
    uint32_t profile_flags;
} Lsx4FexGuestCpuProfileV6;

enum {
    LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB = UINT32_C(1) << 0,
    LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB = UINT32_C(1) << 1,
    LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB = UINT32_C(1) << 2,
    LSX4_FEX_CONFIG_MAXINST_512_AB = UINT32_C(1) << 3,
    LSX4_FEX_CONFIG_MAXINST_256_AB = UINT32_C(1) << 4,
    LSX4_FEX_CONFIG_PERSISTENT_RIP_SEED_AB = UINT32_C(1) << 5,

    LSX4_FEX_CONFIG_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB = UINT32_C(1) << 6,

    LSX4_FEX_CONFIG_PRESERVE_TYPED_TIME_COMMON_FRAME_AB = UINT32_C(1) << 7,

    LSX4_FEX_CONFIG_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB = UINT32_C(1) << 8,
};

typedef struct Lsx4FexConfigureRequestV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t platform_mask;
    uint32_t configure_flags;
    const char* runtime_root;
    const char* cache_namespace;
    const uint8_t* hle_thunk_identity;
    uint32_t hle_thunk_identity_size;
    uint32_t reserved;
    uint64_t required_capabilities;
    const char* host_build_id;
    uint64_t request_cookie;
    uint64_t host_hwcap;
    uint64_t host_hwcap2;
    uint64_t host_isa_features;
    uint64_t host_os_features;
    uint32_t sve_vector_bytes;
    uint32_t reserved2;
    const Lsx4FexGuestCpuProfileV6* guest_cpu_profile_v6;
    const Lsx4FexCpuTuningV1* cpu_tuning_v1;
} Lsx4FexConfigureRequestV3;

typedef struct Lsx4FexConfigureResultV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t status;
    uint32_t lifecycle_state;
    uint64_t provided_capabilities;
    uint32_t mapped_entry_abi;
    uint32_t executable_range_abi;
    const char* adapter_name;
    const char* adapter_version;
    const char* strong_memory_model;
    const char* error_message;
    uint64_t request_cookie;
} Lsx4FexConfigureResultV3;

typedef struct Lsx4FexQueryRequestV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t query_flags;
    uint32_t reserved;
    uint64_t request_cookie;
} Lsx4FexQueryRequestV3;

typedef struct Lsx4FexQueryResultV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t status;
    uint32_t lifecycle_state;
    uint64_t provided_capabilities;
    uint64_t activation_epoch;
    uint64_t executable_range_generation;
    uint64_t active_guest_threads;
    uint32_t mapped_entry_abi;
    uint32_t executable_range_abi;
    const char* adapter_name;
    const char* adapter_version;
    const char* strong_memory_model;
    const char* status_message;
    uint64_t request_cookie;

    uint64_t published_executable_blocks;

    uint64_t code_cache_generation;
    uint64_t code_cache_retained_rotations;
    uint64_t code_cache_generation_resets;
    uint64_t code_cache_current_segments;
    uint64_t code_cache_used_bytes;
    uint64_t code_cache_capacity_bytes;
    uint64_t jit_compilations;
    uint64_t jit_compile_time_ns;
} Lsx4FexQueryResultV3;

typedef struct Lsx4FexActivateRequestV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t activate_flags;
    uint32_t reserved;
    const Lsx4FexHostCallbacksV3* host_callbacks;
    const Lsx4FexExecutableRangeSnapshotV5* initial_executable_ranges;
    const uint8_t* hle_thunk_identity;
    uint32_t hle_thunk_identity_size;
    uint32_t reserved2;
    uint64_t request_cookie;
} Lsx4FexActivateRequestV3;

typedef struct Lsx4FexActivateResultV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t status;
    uint32_t lifecycle_state;
    uint64_t activation_epoch;
    uint64_t effective_capabilities;
    const char* error_message;
    uint64_t request_cookie;
} Lsx4FexActivateResultV3;

typedef int32_t (*Lsx4FexConfigureFnV3)(void* backend_context,
                                        const Lsx4FexConfigureRequestV3* request,
                                        Lsx4FexConfigureResultV3* result);
typedef int32_t (*Lsx4FexQueryFnV3)(void* backend_context,
                                    const Lsx4FexQueryRequestV3* request,
                                    Lsx4FexQueryResultV3* result);
typedef int32_t (*Lsx4FexActivateFnV3)(void* backend_context,
                                       const Lsx4FexActivateRequestV3* request,
                                       Lsx4FexActivateResultV3* result);
typedef int32_t (*Lsx4FexRunMappedEntryFnV5)(
    void* backend_context, const Lsx4FexMappedEntryRequestV5* request,
    Lsx4FexMappedEntryResultV5* result);
typedef int32_t (*Lsx4FexRunGuestCallbackFnV3)(
    void* backend_context, const Lsx4FexGuestCallbackRequestV3* request,
    Lsx4FexGuestCallbackResultV3* result);
typedef int32_t (*Lsx4FexRegisterGuestSignalActionFnV3)(
    void* backend_context, const Lsx4FexGuestSignalActionV3* action);
typedef int32_t (*Lsx4FexQueueGuestSignalFnV3)(
    void* backend_context, const Lsx4FexGuestSignalDeliveryV3* delivery);
typedef int32_t (*Lsx4FexGetGuestContextFnV3)(
    void* backend_context, const Lsx4FexThreadIdentityV3* thread,
    Lsx4FexGuestContextV3* context);
typedef int32_t (*Lsx4FexSetGuestContextFnV3)(
    void* backend_context, const Lsx4FexThreadIdentityV3* thread,
    const Lsx4FexGuestContextV3* context);
typedef int32_t (*Lsx4FexGetGuestTlsFnV3)(void* backend_context,
                                         const Lsx4FexThreadIdentityV3* thread,
                                         Lsx4FexGuestTlsStateV3* tls);
typedef int32_t (*Lsx4FexSetGuestTlsFnV3)(void* backend_context,
                                         const Lsx4FexGuestTlsStateV3* tls);
typedef int32_t (*Lsx4FexBindHostThreadFnV3)(
    void* backend_context, const Lsx4FexThreadIdentityV3* thread);
typedef int32_t (*Lsx4FexUnbindHostThreadFnV3)(void* backend_context);






typedef int32_t (*Lsx4FexReleaseGuestThreadFnV3)(
    void* backend_context, const Lsx4FexThreadIdentityV3* thread);
typedef int32_t (*Lsx4FexSnapshotExecutableRangesFnV5)(
    void* backend_context, Lsx4FexExecutableRangeSnapshotV5* snapshot);
typedef int32_t (*Lsx4FexBulkRegisterExecutableRangesFnV5)(
    void* backend_context, const Lsx4FexExecutableRangeV5* ranges,
    uint32_t range_count, uint64_t expected_generation, uint64_t* new_generation);
typedef int32_t (*Lsx4FexInvalidateExecutableRangesFnV5)(
    void* backend_context, const Lsx4FexExecutableRangeV5* ranges,
    uint32_t range_count, uint32_t invalidate_flags, uint64_t expected_generation,
    uint64_t* new_generation);






typedef int32_t (*Lsx4FexHandleHostSignalFnV3)(
    void* backend_context, int32_t signum, const void* signal_info,
    void* raw_context);





typedef int32_t (*Lsx4FexClassifySignalContextPcFnV3)(
    void* backend_context, const void* raw_context);

typedef struct Lsx4FexBackendApiV3 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t provided_capabilities;
    uint32_t lifetime_flags;
    uint32_t reserved;
    void* backend_context;
    const char* adapter_name;
    const char* adapter_version;
    Lsx4FexConfigureFnV3 configure;
    Lsx4FexQueryFnV3 query;
    Lsx4FexActivateFnV3 activate;
    Lsx4FexRunMappedEntryFnV5 run_mapped_entry_v5;
    Lsx4FexRunGuestCallbackFnV3 run_guest_callback;
    Lsx4FexRegisterGuestSignalActionFnV3 register_guest_signal_action;
    Lsx4FexQueueGuestSignalFnV3 queue_guest_signal;
    Lsx4FexGetGuestContextFnV3 get_guest_context;
    Lsx4FexSetGuestContextFnV3 set_guest_context;
    Lsx4FexGetGuestTlsFnV3 get_guest_tls;
    Lsx4FexSetGuestTlsFnV3 set_guest_tls;
    Lsx4FexBindHostThreadFnV3 bind_host_thread;
    Lsx4FexUnbindHostThreadFnV3 unbind_host_thread;
    Lsx4FexSnapshotExecutableRangesFnV5 snapshot_executable_ranges_v5;
    Lsx4FexBulkRegisterExecutableRangesFnV5 bulk_register_executable_ranges_v5;
    Lsx4FexInvalidateExecutableRangesFnV5 invalidate_executable_ranges_v5;
    Lsx4FexHandleHostSignalFnV3 handle_host_signal;
    Lsx4FexClassifySignalContextPcFnV3 classify_signal_context_pc;
    Lsx4FexReleaseGuestThreadFnV3 release_guest_thread;
} Lsx4FexBackendApiV3;

typedef int32_t (*Lsx4FexGetBackendApiV3Fn)(uint32_t requested_abi,
                                            uint32_t output_size,
                                            Lsx4FexBackendApiV3* output);

int32_t lsx4_fex_backend_get_api_v3(uint32_t requested_abi, uint32_t output_size,
                                    Lsx4FexBackendApiV3* output);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <type_traits>

static_assert(sizeof(void*) == 8, "FEX backend ABI v3 requires a 64-bit process");

#define LSX4_FEX_ABI_ASSERT_POD(type_, size_)                                             \
    static_assert(sizeof(type_) == (size_));                                              \
    static_assert(alignof(type_) == 8);                                                   \
    static_assert(std::is_standard_layout_v<type_>);                                      \
    static_assert(std::is_trivially_copyable_v<type_>)
#define LSX4_FEX_ABI_ASSERT_OFFSET(type_, field_, offset_) \
    static_assert(offsetof(type_, field_) == (offset_))

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexXmm128, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexXmm128, low, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexXmm128, high, 8);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexHleCallFrameV3, 256);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, call_id, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, native_function, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, guest_rip, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, guest_rsp, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, gpr, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, xmm, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, sret_address, 216);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, sret_size, 224);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, return_class, 228);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, return_eightbyte_count, 230);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, sret_flags, 231);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, call_flags, 232);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, sysv_sse_arg_count, 236);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, abi_descriptor_id, 240);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleCallFrameV3, input_rax, 248);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexHleResultFrameV3, 96);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, call_id, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, rax, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, rdx, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, xmm0, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, xmm1, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, sret_address, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, sret_size, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, return_class, 76);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, return_eightbyte_count, 78);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, sret_flags, 79);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, result_flags, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, status, 84);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHleResultFrameV3, abi_descriptor_id, 88);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexThreadIdentityV3, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexThreadIdentityV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexThreadIdentityV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexThreadIdentityV3, guest_thread, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexThreadIdentityV3, pthread_handle, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexThreadIdentityV3, host_tid, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexThreadIdentityV3, backend_thread, 32);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexGuestContextV3, 440);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, gpr, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, rip, 136);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, rflags, 144);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, fs_base, 152);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, gs_base, 160);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, xmm, 168);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, mxcsr, 424);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, context_flags, 428);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestContextV3, guest_thread, 432);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexGuestSignalActionV3, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, signum, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, action_flags, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, handler, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, restorer, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, mask_low, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalActionV3, mask_high, 40);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexGuestSignalDeliveryV3, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, target, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, signum, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, delivery_flags, 52);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, explicit_rip, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, explicit_rsp, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestSignalDeliveryV3, explicit_context, 72);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexGuestTlsStateV3, 104);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, target, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, fs_base, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, gs_base, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, tcb_base, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, tls_image_address, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, tls_image_size, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, tls_module_id, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, tls_flags, 96);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestTlsStateV3, reserved, 100);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexGuestCallbackRequestV3, 248);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, callback_id, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, guest_function, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, guest_rsp, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, gpr, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, xmm, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, sret_address, 208);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, sret_size, 216);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, return_class, 220);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, return_eightbyte_count, 222);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, sret_flags, 223);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, call_flags, 224);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, sysv_sse_arg_count, 228);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, abi_descriptor_id, 232);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCallbackRequestV3, input_rax, 240);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexMappedSegmentV5, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedSegmentV5, address, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedSegmentV5, size, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedSegmentV5, range_cookie, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedSegmentV5, protection, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedSegmentV5, range_flags, 28);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexExecutableRangeV5, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeV5, address, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeV5, size, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeV5, range_cookie, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeV5, protection, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeV5, range_flags, 28);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexExecutableRangeSnapshotV5, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, generation, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, ranges, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, capacity, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, range_count, 28);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, snapshot_flags, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexExecutableRangeSnapshotV5, reserved, 36);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexMappedEntryRequestV5, 144);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, request_flags, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, timeout_ms, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, entry, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, proc_param, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, stack_base, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, stack_size, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, tls_image_address, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, tls_image_size, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, tls_module_id, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, tcb_base, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, entry_params, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, exit_function, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, hle_trampoline_base, 96);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, hle_trampoline_size, 104);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, segments, 112);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, segment_count, 120);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, reserved, 124);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, executable_ranges, 128);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryRequestV5, request_cookie, 136);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexMappedEntryResultV5, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, status, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, result_flags, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, rax, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, final_rip, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, guest_exit_code, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexMappedEntryResultV5, request_cookie, 40);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexHostCallbacksV3, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, callback_context, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, log, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, dispatch_hle, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, thread_event, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, fatal_error, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, acquire_guest_memory_lease, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, release_guest_memory_lease, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, try_dispatch_leaf_hle, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, try_dispatch_typed_time_hle, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexHostCallbacksV3, try_dispatch_scalar_fp_hle, 80);

static_assert(sizeof(Lsx4FexCpuidResultV6) == 16);
static_assert(alignof(Lsx4FexCpuidResultV6) == 4);
static_assert(std::is_standard_layout_v<Lsx4FexCpuidResultV6>);
static_assert(std::is_trivially_copyable_v<Lsx4FexCpuidResultV6>);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuidResultV6, eax, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuidResultV6, ebx, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuidResultV6, ecx, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuidResultV6, edx, 12);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexGuestCpuProfileV6, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, identity_low, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, identity_high, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, profile_id, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, profile_context, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, query_cpuid, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, query_xcr, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, xcr0_writable_mask, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, mxcsr_reset_value, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, mxcsr_mask, 68);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, legacy_save_area_bytes, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexGuestCpuProfileV6, profile_flags, 76);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexCpuTuningV1, 176);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuTuningV1, reserved0, 164);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuTuningV1, identity, 168);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexConfigureRequestV3, 128);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, platform_mask, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, configure_flags, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, runtime_root, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, cache_namespace, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, hle_thunk_identity, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, hle_thunk_identity_size, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, reserved, 44);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, required_capabilities, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, host_build_id, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, request_cookie, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, host_hwcap, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, host_hwcap2, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, host_isa_features, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, host_os_features, 96);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, sve_vector_bytes, 104);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, reserved2, 108);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, guest_cpu_profile_v6, 112);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureRequestV3, cpu_tuning_v1, 120);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexConfigureResultV3, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, status, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, lifecycle_state, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, provided_capabilities, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, mapped_entry_abi, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, executable_range_abi, 28);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, adapter_name, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, adapter_version, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, strong_memory_model, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, error_message, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexConfigureResultV3, request_cookie, 64);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexQueryRequestV3, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryRequestV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryRequestV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryRequestV3, query_flags, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryRequestV3, reserved, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryRequestV3, request_cookie, 16);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexQueryResultV3, 168);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, status, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, lifecycle_state, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, provided_capabilities, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, activation_epoch, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, executable_range_generation, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, active_guest_threads, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, mapped_entry_abi, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, executable_range_abi, 52);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, adapter_name, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, adapter_version, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, strong_memory_model, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, status_message, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, request_cookie, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, published_executable_blocks, 96);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, code_cache_generation, 104);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, code_cache_retained_rotations, 112);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, code_cache_generation_resets, 120);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, code_cache_current_segments, 128);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, code_cache_used_bytes, 136);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, code_cache_capacity_bytes, 144);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, jit_compilations, 152);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexQueryResultV3, jit_compile_time_ns, 160);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexActivateRequestV3, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, activate_flags, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, reserved, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, host_callbacks, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, initial_executable_ranges, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, hle_thunk_identity, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, hle_thunk_identity_size, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, reserved2, 44);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateRequestV3, request_cookie, 48);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexActivateResultV3, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, status, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, lifecycle_state, 12);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, activation_epoch, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, effective_capabilities, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, error_message, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexActivateResultV3, request_cookie, 40);

LSX4_FEX_ABI_ASSERT_POD(Lsx4FexBackendApiV3, 200);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, struct_size, 0);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, abi_version, 4);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, provided_capabilities, 8);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, lifetime_flags, 16);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, reserved, 20);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, backend_context, 24);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, adapter_name, 32);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, adapter_version, 40);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, configure, 48);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, query, 56);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, activate, 64);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, run_mapped_entry_v5, 72);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, run_guest_callback, 80);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, register_guest_signal_action, 88);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, queue_guest_signal, 96);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, get_guest_context, 104);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, set_guest_context, 112);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, get_guest_tls, 120);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, set_guest_tls, 128);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, bind_host_thread, 136);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, unbind_host_thread, 144);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, snapshot_executable_ranges_v5, 152);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, bulk_register_executable_ranges_v5, 160);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, invalidate_executable_ranges_v5, 168);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, handle_host_signal, 176);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, classify_signal_context_pc, 184);
LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexBackendApiV3, release_guest_thread, 192);

static_assert(sizeof(Lsx4FexGuestCallbackResultV3) == 96);
static_assert(alignof(Lsx4FexGuestCallbackResultV3) == 8);
static_assert(std::is_standard_layout_v<Lsx4FexGuestCallbackResultV3>);
static_assert(std::is_trivially_copyable_v<Lsx4FexGuestCallbackResultV3>);

#undef LSX4_FEX_ABI_ASSERT_OFFSET
#undef LSX4_FEX_ABI_ASSERT_POD

#endif

#endif
