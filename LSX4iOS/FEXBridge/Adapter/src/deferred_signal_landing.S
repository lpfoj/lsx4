#if defined(__APPLE__)
// SPDX-License-Identifier: MIT

#if defined(__aarch64__)
  .text
  .p2align 2
  .global _lsx4_fex2608_deferred_signal_landing


_lsx4_fex2608_deferred_signal_landing:
  // The first signal return supplies ThreadRecord* in x0 and a dedicated,
  // guarded host stack.  No interrupted JIT register is authoritative here;
  // the original kernel mcontext lives in fixed ThreadRecord storage.
  bl _lsx4_fex2608_process_deferred_signal
  // Exactly one means that a complete restoration context is ready.  Failure
  // values are stable diagnostics for the otherwise async-only landing path.
  cmp w0, #1
  b.eq 1f
  cbnz w0, 2f
  mov w0, #127
2:
  bl _lsx4_fex2608_report_deferred_exit
  brk #0

  // The reserved trap is the only restoration gateway.  Its handler replaces
  // this trap's ucontext with the complete saved/updated interrupted context,
  // so kernel sigreturn restores x0-x30, SP, PC, PSTATE and all context records.
1:
  .global _lsx4_fex2608_deferred_signal_resume_trap


_lsx4_fex2608_deferred_signal_resume_trap:
  brk #0x4f45
  // A missing/misconfigured signal owner must fail closed, never return into
  // a C++ call frame or execute arbitrary bytes.
  mov x0, #127
  bl _lsx4_fex2608_report_deferred_exit
  brk #0

#endif

#else
// SPDX-License-Identifier: MIT

#if defined(__aarch64__)
  .text
  .p2align 2
  .global lsx4_fex2608_deferred_signal_landing
  .hidden lsx4_fex2608_deferred_signal_landing
  .type lsx4_fex2608_deferred_signal_landing, %function
lsx4_fex2608_deferred_signal_landing:
  // The first signal return supplies ThreadRecord* in x0 and a dedicated,
  // guarded host stack.  No interrupted JIT register is authoritative here;
  // the original kernel mcontext lives in fixed ThreadRecord storage.
  bl lsx4_fex2608_process_deferred_signal
  // Exactly one means that a complete restoration context is ready.  Failure
  // values are stable diagnostics for the otherwise async-only landing path.
  cmp w0, #1
  b.eq 1f
  cbnz w0, 2f
  mov w0, #127
2:
  mov x8, #94
  svc #0
  brk #0
  .size lsx4_fex2608_deferred_signal_landing, .-lsx4_fex2608_deferred_signal_landing
  // The reserved trap is the only restoration gateway.  Its handler replaces
  // this trap's ucontext with the complete saved/updated interrupted context,
  // so kernel sigreturn restores x0-x30, SP, PC, PSTATE and all context records.
1:
  .global lsx4_fex2608_deferred_signal_resume_trap
  .hidden lsx4_fex2608_deferred_signal_resume_trap
  .type lsx4_fex2608_deferred_signal_resume_trap, %function
lsx4_fex2608_deferred_signal_resume_trap:
  brk #0x4f45
  // A missing/misconfigured signal owner must fail closed, never return into
  // a C++ call frame or execute arbitrary bytes.
  mov x0, #127
  mov x8, #94
  svc #0
  brk #0
  .size lsx4_fex2608_deferred_signal_resume_trap, .-lsx4_fex2608_deferred_signal_resume_trap
#endif

#endif
