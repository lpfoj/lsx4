// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

    .text
    .p2align 2

    .hidden Lsx4MaterializeDeferredFlagsStaticGprJitAbi
    .global Lsx4MaterializeDeferredFlagsStaticGprJitAbi
    .type Lsx4MaterializeDeferredFlagsStaticGprJitAbi, %function
Lsx4MaterializeDeferredFlagsStaticGprJitAbi:
    bti c
    sub sp, sp, #80
    stp x1, x2, [sp, #0]
    stp x3, x4, [sp, #16]
    stp x5, x6, [sp, #32]
    stp x7, x8, [sp, #48]
    str x30, [sp, #64]
    bl Lsx4MaterializeDeferredFlagsBody
    ldr x30, [sp, #64]
    ldp x7, x8, [sp, #48]
    ldp x5, x6, [sp, #32]
    ldp x3, x4, [sp, #16]
    ldp x1, x2, [sp, #0]
    add sp, sp, #80
    ret
    .size Lsx4MaterializeDeferredFlagsStaticGprJitAbi, .-Lsx4MaterializeDeferredFlagsStaticGprJitAbi

    .p2align 2
    .hidden Lsx4MaterializeDeferredFlagsJitAbi
    .global Lsx4MaterializeDeferredFlagsJitAbi
    .type Lsx4MaterializeDeferredFlagsJitAbi, %function
Lsx4MaterializeDeferredFlagsJitAbi:
    bti c
    sub sp, sp, #448
    stp x1, x2, [sp, #0]
    stp x3, x4, [sp, #16]
    stp x5, x6, [sp, #32]
    stp x7, x8, [sp, #48]
    str x30, [sp, #64]
    stp q9, q10, [sp, #96]
    stp q11, q12, [sp, #128]
    stp q13, q14, [sp, #160]
    stp q15, q16, [sp, #192]
    stp q17, q18, [sp, #224]
    stp q19, q20, [sp, #256]
    stp q21, q22, [sp, #288]
    stp q23, q24, [sp, #320]
    stp q25, q26, [sp, #352]
    stp q27, q28, [sp, #384]
    stp q29, q30, [sp, #416]
    bl Lsx4MaterializeDeferredFlagsBody
    ldp q29, q30, [sp, #416]
    ldp q27, q28, [sp, #384]
    ldp q25, q26, [sp, #352]
    ldp q23, q24, [sp, #320]
    ldp q21, q22, [sp, #288]
    ldp q19, q20, [sp, #256]
    ldp q17, q18, [sp, #224]
    ldp q15, q16, [sp, #192]
    ldp q13, q14, [sp, #160]
    ldp q11, q12, [sp, #128]
    ldp q9, q10, [sp, #96]
    ldr x30, [sp, #64]
    ldp x7, x8, [sp, #48]
    ldp x5, x6, [sp, #32]
    ldp x3, x4, [sp, #16]
    ldp x1, x2, [sp, #0]
    add sp, sp, #448
    ret
    .size Lsx4MaterializeDeferredFlagsJitAbi, .-Lsx4MaterializeDeferredFlagsJitAbi

    .p2align 2
    .hidden CaptureJitFaultResume
    .global CaptureJitFaultResume
    .type CaptureJitFaultResume, %function
CaptureJitFaultResume:
    bti c
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x1, sp
    str x1, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mrs x1, fpcr
    str w1, [x0, #168]
    mrs x1, fpsr
    str w1, [x0, #172]
    mov w0, wzr
    ret
    .size CaptureJitFaultResume, .-CaptureJitFaultResume

    .p2align 2
    .hidden RestoreJitFaultResume
    .global RestoreJitFaultResume
    .type RestoreJitFaultResume, %function
RestoreJitFaultResume:
    bti c
    mov x16, x0
    ldp d8, d9, [x16, #104]
    ldp d10, d11, [x16, #120]
    ldp d12, d13, [x16, #136]
    ldp d14, d15, [x16, #152]
    ldr w17, [x16, #168]
    msr fpcr, x17
    ldr w17, [x16, #172]
    msr fpsr, x17
    ldr x17, [x16, #96]
    mov sp, x17
    ldp x19, x20, [x16, #0]
    ldp x21, x22, [x16, #16]
    ldp x23, x24, [x16, #32]
    ldp x25, x26, [x16, #48]
    ldp x27, x28, [x16, #64]
    ldp x29, x30, [x16, #80]
    mov w0, #1
    ret
    .size RestoreJitFaultResume, .-RestoreJitFaultResume

    .p2align 2
    .hidden ReturnFromJitSignalContextFault
    .global ReturnFromJitSignalContextFault
    .type ReturnFromJitSignalContextFault, %function
ReturnFromJitSignalContextFault:
    bti c
    ldp d9, d10, [sp, #96]
    ldp d11, d12, [sp, #112]
    ldp d13, d14, [sp, #128]
    ldr d15, [sp, #144]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #160
    mov x0, xzr
    ret
    .size ReturnFromJitSignalContextFault, .-ReturnFromJitSignalContextFault

    .arch_extension lse
    .p2align 2
    .hidden Lsx4AtomicExchangeByteLse
    .global Lsx4AtomicExchangeByteLse
    .type Lsx4AtomicExchangeByteLse, %function
Lsx4AtomicExchangeByteLse:
    bti c
    swpalb w1, w0, [x0]
    ret
    .size Lsx4AtomicExchangeByteLse, .-Lsx4AtomicExchangeByteLse

    .p2align 2
    .hidden Lsx4SampleTieredGuardExit
    .global Lsx4SampleTieredGuardExit
    .type Lsx4SampleTieredGuardExit, %function
Lsx4SampleTieredGuardExit:
    bti c
1:
    ldxr x16, [x0]
    add x16, x16, #1
    stxr w17, x16, [x0]
    cbnz w17, 1b
    tst x16, #0xff
    csel x0, x16, xzr, eq
    ret
    .size Lsx4SampleTieredGuardExit, .-Lsx4SampleTieredGuardExit

    .macro lsx4_store_fixed_xmm_if_dirty bit, reg, offset
    tbz w9, #\bit, 99f
    str \reg, [x19, #\offset]
99:
    .endm

    // Private branch ABI:
    //   x19 = LsxMachineImage*, w9 = effective fixed-XMM dirty mask,
    //   x16 = continuation. All pinned guest registers are preserved.
    .p2align 2
    .hidden Lsx4JitPublishFixedXmmDirty
    .global Lsx4JitPublishFixedXmmDirty
    .type Lsx4JitPublishFixedXmmDirty, %function
Lsx4JitPublishFixedXmmDirty:
    bti jc
    lsx4_store_fixed_xmm_if_dirty 0, q16, 0x140
    lsx4_store_fixed_xmm_if_dirty 1, q17, 0x160
    lsx4_store_fixed_xmm_if_dirty 2, q18, 0x180
    lsx4_store_fixed_xmm_if_dirty 3, q19, 0x1a0
    lsx4_store_fixed_xmm_if_dirty 4, q20, 0x1c0
    lsx4_store_fixed_xmm_if_dirty 5, q21, 0x1e0
    lsx4_store_fixed_xmm_if_dirty 6, q22, 0x200
    lsx4_store_fixed_xmm_if_dirty 7, q23, 0x220
    lsx4_store_fixed_xmm_if_dirty 8, q24, 0x240
    lsx4_store_fixed_xmm_if_dirty 9, q25, 0x260
    lsx4_store_fixed_xmm_if_dirty 10, q26, 0x280
    lsx4_store_fixed_xmm_if_dirty 11, q27, 0x2a0
    lsx4_store_fixed_xmm_if_dirty 12, q28, 0x2c0
    lsx4_store_fixed_xmm_if_dirty 13, q29, 0x2e0
    lsx4_store_fixed_xmm_if_dirty 14, q9, 0x300
    lsx4_store_fixed_xmm_if_dirty 15, q10, 0x320
    strh wzr, [x19, #0x380]
    ret x16
    .size Lsx4JitPublishFixedXmmDirty, .-Lsx4JitPublishFixedXmmDirty

    .macro lsx4_validate_fixed_xmm_clean bit, reg, offset
    tbnz w9, #\bit, 99f
    ldr q31, [x19, #\offset]
    eor v31.16b, v31.16b, \reg\().16b
    umaxv b31, v31.16b
    umov w15, v31.b[0]
    cbz w15, 99f
    mov w14, #\bit
    b .Llsx4_fixed_xmm_shadow_mismatch
99:
    .endm

    // Shadow proof ABI:
    //   w9 effective mask, x10 source RIP, w11 local mask,
    //   w12 inherited mask, w13 edge type, x16 continuation, x19 state.
    // It only reads architectural values. A mismatch records telemetry and
    // returns to the exact same generated control-flow point without repair.
    .p2align 2
    .hidden Lsx4JitValidateFixedXmmDirtyShadow
    .global Lsx4JitValidateFixedXmmDirtyShadow
    .type Lsx4JitValidateFixedXmmDirtyShadow, %function
Lsx4JitValidateFixedXmmDirtyShadow:
    bti jc
    lsx4_validate_fixed_xmm_clean 0, v16, 0x140
    lsx4_validate_fixed_xmm_clean 1, v17, 0x160
    lsx4_validate_fixed_xmm_clean 2, v18, 0x180
    lsx4_validate_fixed_xmm_clean 3, v19, 0x1a0
    lsx4_validate_fixed_xmm_clean 4, v20, 0x1c0
    lsx4_validate_fixed_xmm_clean 5, v21, 0x1e0
    lsx4_validate_fixed_xmm_clean 6, v22, 0x200
    lsx4_validate_fixed_xmm_clean 7, v23, 0x220
    lsx4_validate_fixed_xmm_clean 8, v24, 0x240
    lsx4_validate_fixed_xmm_clean 9, v25, 0x260
    lsx4_validate_fixed_xmm_clean 10, v26, 0x280
    lsx4_validate_fixed_xmm_clean 11, v27, 0x2a0
    lsx4_validate_fixed_xmm_clean 12, v28, 0x2c0
    lsx4_validate_fixed_xmm_clean 13, v29, 0x2e0
    lsx4_validate_fixed_xmm_clean 14, v9, 0x300
    lsx4_validate_fixed_xmm_clean 15, v10, 0x320
    ret x16

.Llsx4_fixed_xmm_shadow_mismatch:
    adrp x17, g_lsx4_jit_fixed_xmm_shadow_status
    add x17, x17, :lo12:g_lsx4_jit_fixed_xmm_shadow_status
    ldar w15, [x17]
    cbnz w15, .Llsx4_fixed_xmm_shadow_done
    str w14, [x17, #4]
    str x10, [x17, #8]
    str w11, [x17, #16]
    str w12, [x17, #20]
    str w9, [x17, #24]
    str w13, [x17, #28]
    mov w15, #2
    stlr w15, [x17]
.Llsx4_fixed_xmm_shadow_done:
    ret x16
    .size Lsx4JitValidateFixedXmmDirtyShadow, .-Lsx4JitValidateFixedXmmDirtyShadow

    // Private static-GPR chain ABI adapter.  Entry contract:
    //   x17 = OutboundEdgeState*
    //   x19 = LsxMachineImage*
    //   x30 = native edge scheduling phase
    //   x23,x24,x25,x26,x4,x6,x7,x8,x1,x2,x3,x5,x20,x27,x29
    //       = guest GPR[0..14]
    //   x28 = guest RFLAGS
    //   q16..q29,q9,q10 = guest XMM[0..15]
    // The target at edge+0x68 was published before the adapter patch cell.
    // The null branch is a defensive shared-chain epilogue; publication order
    // makes it unreachable in normal execution, but it keeps withdrawal races
    // fail-safe without adding a continuation instruction to every hot edge.
    .p2align 2
    .hidden Lsx4JitStaticEdgeToResident
    .global Lsx4JitStaticEdgeToResident
    .type Lsx4JitStaticEdgeToResident, %function
Lsx4JitStaticEdgeToResident:
    bti jc
    stp x23, x24, [x19, #0]
    stp x25, x26, [x19, #16]
    stp x4, x6, [x19, #32]
    stp x7, x8, [x19, #48]
    stp x1, x2, [x19, #64]
    stp x3, x5, [x19, #80]
    stp x20, x27, [x19, #96]
    str x29, [x19, #112]
    str x28, [x19, #0x98]
    str q16, [x19, #0x140]
    str q17, [x19, #0x160]
    str q18, [x19, #0x180]
    str q19, [x19, #0x1a0]
    str q20, [x19, #0x1c0]
    str q21, [x19, #0x1e0]
    str q22, [x19, #0x200]
    str q23, [x19, #0x220]
    str q24, [x19, #0x240]
    str q25, [x19, #0x260]
    str q26, [x19, #0x280]
    str q27, [x19, #0x2a0]
    str q28, [x19, #0x2c0]
    str q29, [x19, #0x2e0]
    str q9, [x19, #0x300]
    str q10, [x19, #0x320]
    strh wzr, [x19, #0x380]
    add x16, x17, #0x68
    ldar x16, [x16]
    cbz x16, .Llsx4_static_edge_fallback
    br x16

.Llsx4_static_edge_fallback:
    str w30, [x19, #0xa8]
    ldr x0, [x19, #0x80]
    ldp d9, d10, [sp, #96]
    ldp d11, d12, [sp, #112]
    ldp d13, d14, [sp, #128]
    ldr d15, [sp, #144]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #160
    ret
    .size Lsx4JitStaticEdgeToResident, .-Lsx4JitStaticEdgeToResident

    .macro load_repeated_half register, half
    mov \register, #\half
    movk \register, #\half, lsl #16
    movk \register, #\half, lsl #32
    movk \register, #\half, lsl #48
    .endm

    .macro compare_repeated_half register, half
    load_repeated_half x10, \half
    cmp \register, x10
    cset w10, eq
    and w9, w9, w10
    .endm

    .macro load_repeated_half_double register, half
    load_repeated_half x10, \half
    fmov \register, x10
    .endm

    .macro compare_repeated_half_double register, half
    fmov x11, \register
    load_repeated_half x10, \half
    cmp x11, x10
    cset w10, eq
    and w9, w9, w10
    .endm

    .p2align 2
    .hidden Lsx4InvokeGeneratedCodeWithAbiCanaries
    .global Lsx4InvokeGeneratedCodeWithAbiCanaries
    .type Lsx4InvokeGeneratedCodeWithAbiCanaries, %function
Lsx4InvokeGeneratedCodeWithAbiCanaries:
    bti c
    stp x29, x30, [sp, #-176]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    str x2, [sp, #96]
    stp d9, d10, [sp, #112]
    stp d11, d12, [sp, #128]
    stp d13, d14, [sp, #144]
    str d15, [sp, #160]
    mov x16, x0
    mov x0, x1
    load_repeated_half x19, 0x1919
    load_repeated_half x20, 0x2020
    load_repeated_half x21, 0x2121
    load_repeated_half x22, 0x2222
    load_repeated_half x23, 0x2323
    load_repeated_half x24, 0x2424
    load_repeated_half x25, 0x2525
    load_repeated_half x26, 0x2626
    load_repeated_half x27, 0x2727
    load_repeated_half x28, 0x2828
    load_repeated_half_double d9, 0x0909
    load_repeated_half_double d10, 0x1010
    load_repeated_half_double d11, 0x1111
    load_repeated_half_double d12, 0x1212
    load_repeated_half_double d13, 0x1313
    load_repeated_half_double d14, 0x1414
    load_repeated_half_double d15, 0x1515
    blr x16
    ldr x2, [sp, #96]
    str x0, [x2]
    mov x10, sp
    cmp x29, x10
    cset w9, eq
    compare_repeated_half x19, 0x1919
    compare_repeated_half x20, 0x2020
    compare_repeated_half x21, 0x2121
    compare_repeated_half x22, 0x2222
    compare_repeated_half x23, 0x2323
    compare_repeated_half x24, 0x2424
    compare_repeated_half x25, 0x2525
    compare_repeated_half x26, 0x2626
    compare_repeated_half x27, 0x2727
    compare_repeated_half x28, 0x2828
    compare_repeated_half_double d9, 0x0909
    compare_repeated_half_double d10, 0x1010
    compare_repeated_half_double d11, 0x1111
    compare_repeated_half_double d12, 0x1212
    compare_repeated_half_double d13, 0x1313
    compare_repeated_half_double d14, 0x1414
    compare_repeated_half_double d15, 0x1515
    mov w0, w9
    ldp d9, d10, [sp, #112]
    ldp d11, d12, [sp, #128]
    ldp d13, d14, [sp, #144]
    ldr d15, [sp, #160]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #176
    ret
    .size Lsx4InvokeGeneratedCodeWithAbiCanaries, .-Lsx4InvokeGeneratedCodeWithAbiCanaries

    .section .note.GNU-stack,"",%progbits
