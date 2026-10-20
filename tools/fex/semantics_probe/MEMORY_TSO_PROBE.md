# FEX memory-component A/B semantics probe

This probe is a pre-promotion correctness gate for the two diagnostic
software-TSO component overrides. It does not change emulator production code.

## Coverage

`memory_tso_program.h` emits real x86-64 guest instructions for:

- disjoint `REP MOVSB` and `REP MOVSQ`;
- `REP STOSB` and `REP STOSQ`;
- zero-count MOVS/STOS;
- both overlap-safe `memmove` directions (CLD forward and STD backward);
- bounded two-guest-thread `REP MOVS -> scalar flag`, `REP STOS -> scalar
  flag`, and `MOVDQU vector store -> scalar flag` publication tests.

Every publication participant has an instruction-count timeout. The staged
mapped-entry request also carries a 30-second timeout. A runner should still
apply an external process timeout because no in-guest bound can recover a JIT
or bridge bug that stops executing guest instructions entirely.

The staged Android executable creates a real FEX-2608 context-bridge runtime
in software-TSO mode. Its accepted modes are:

```text
baseline
memcpy-relaxed
vector-relaxed
```

It rejects a runtime whose reported feature bits or memory-model name do not
exactly match the selected mode. The host feature mask is deliberately zero so
the test covers the architectural software-TSO fallback without silently
depending on LSE/LRCPC. Device-specific LSE/LRCPC lowering remains a separate
test obligation.

## Build and host oracle

Configure the directory normally on Windows, then run CTest. The
`memory_tso_x86_oracle` target validates the byte emitter and x86 architectural
results. It runs eight rounds of 20,000 iterations for every publication case.

The x86 oracle cannot expose Arm weak-order outcomes and therefore cannot
approve either relaxed option. Promotion still requires running all three
staged modes against the exact Android bridge/FEXCore artifacts, followed by
fresh-process game A/B/A and the existing graphics, audio, freeze, lifecycle,
and frame-time gates.

## Scope limits

A passing bounded litmus demonstrates only the exercised instruction shapes,
addresses, alignments, and scheduling run. It is not a proof of the global
Android memory model, all REP sizes/alignments, every vector width, signal or
fault restart behavior, HLE synchronization, MMIO, GPU visibility, or
self-modifying code. No relaxed memory component should become a default from
this result alone.
