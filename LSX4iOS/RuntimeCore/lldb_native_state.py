"""Read-only snapshot of suspended generated-code threads; no target evaluation."""
import json
import struct
import lldb


def capture(debugger, command, result, internal_dict):
    process = debugger.GetSelectedTarget().GetProcess()
    captured = 0
    for thread in process:
        # A stopped guest often waits inside a host HLE call. Unwind to its
        # generated caller: callee-saved x19 (Current) or x28 (FEX) is still
        # a state candidate there.
        # Reading only frame zero silently missed every such waiting thread.
        frames = [thread.GetFrameAtIndex(i) for i in range(min(thread.GetNumFrames(), 64))]
        # FEX thunks can terminate the unwind before ExecuteThread. The HLE
        # bridge is sufficient evidence that their generated caller is guest
        # code; otherwise blocked FEX threads silently disappear here.
        guest_entries = ('RunGuest(', 'RunGuestCallback(', 'ExecuteThread(',
                         'ExecuteThreadWithGuestFpcr(', 'BridgeThunkCommon(',
                         'ExecutorFexDispatchHle')
        guest_stack = any(any(entry in (f.GetFunctionName() or '')
                              for entry in guest_entries) for f in frames)
        candidates_at = [i for i, f in enumerate(frames)
                         if f.IsValid() and f.GetPC() and not f.GetModule().IsValid()
                         and (i == 0 or guest_stack)]
        if not candidates_at:
            continue
        frame_index = candidates_at[0]
        frame = frames[frame_index]
        pc = frame.GetPC()
        registers = {name: frame.FindRegister(name).GetValue()
                     for name in [*(f'x{i}' for i in range(29)), 'fp', 'lr', 'sp', 'pc', 'cpsr']}
        error = lldb.SBError()
        start = max(0, pc - 32)
        code = process.ReadMemory(start, 128, error)
        memory_error = error.GetCString() if error.Fail() else None
        candidates = {}
        # Current's generated body uses x19 for LsxMachineImage and x21 for
        # its inline fault descriptor. A snapshot can catch an entry/exit stub,
        # FEX's Arm64Emitter uses x28 for CpuStateFrame (State is its first
        # member). Retain bytes as candidates: generated code may have live
        # registers that have not yet been spilled into the memory image.
        # None of these reads evaluates code in the suspended target.
        for register, size in (('x19', 0x3a0), ('x21', 128), ('x28', 0x400)):
            address = frame.FindRegister(register).GetValueAsUnsigned(0)
            if register == 'x21':
                address &= ~1  # NativeFaultRecoveryTag; the raw register is retained above.
            if address:
                data = process.ReadMemory(address, size, error)
                candidates[register] = {'address': hex(address),
                    'bytes': data.hex() if data else '',
                    'error': error.GetCString() if error.Fail() else None}
                if register == 'x28' and data and len(data) >= 160:
                    # Pinned FEX CPUState: RIP at24, sixteen GPRs at32.
                    # Keep this explicitly a candidate, including the raw
                    # bytes above. A generated entry stub may not use x28 yet.
                    rip = struct.unpack_from('<Q', data, 24)[0]
                    gpr = struct.unpack_from('<16Q', data, 32)
                    candidates[register]['fexStateCandidate'] = {
                        'rip': hex(rip), 'gpr': [hex(v) for v in gpr]}
                    # Bound extra network reads to the first eight guest
                    # threads. RDI may name the live HLE words; RSP may point
                    # into the exact leased helper stack instead of the game's
                    # stack. Neither is presumed to be the original context.
                    if captured < 8:
                        regions = {}
                        for label, pointer in (('rip', rip), ('rsp', gpr[4]),
                                               ('rdi', gpr[7])):
                            if not pointer:
                                continue
                            payload = process.ReadMemory(pointer, 160, error)
                            regions[label] = {'address': hex(pointer),
                                'bytes': payload.hex() if payload else '',
                                'error': error.GetCString() if error.Fail() else None}
                        candidates[register]['boundedMemoryCandidates'] = regions
        result.PutCString('[APPLE_NATIVE_THREAD_STATE] ' + json.dumps({
            'threadIndex': thread.GetIndexID(), 'threadId': thread.GetThreadID(),
            'name': thread.GetName(), 'frameIndex': frame_index,
            'topFunction': frames[0].GetFunctionName(),
            'unwoundRegisters': frame_index != 0, 'registers': registers,
            'nativeStart': hex(start), 'nativeBytes': code.hex() if code else '',
            'memoryError': memory_error, 'contextCandidates': candidates,
        }))
        captured += 1
        if captured == 64:
            break
    result.PutCString('[APPLE_NATIVE_THREAD_STATE_SUMMARY] ' + json.dumps({'captured': captured}))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f lldb_native_state.capture lsx4-native-state')
