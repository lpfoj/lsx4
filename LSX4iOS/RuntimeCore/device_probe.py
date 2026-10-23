#!/usr/bin/env python3
"""Run the same bounded shared-core fixture on a paired iPhone; leave its UI awake."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
import plistlib
from pathlib import Path
import subprocess
import time
import fcntl
import uuid
from android_cpu_profiles import NAME as CPU_PROFILE_NAME, stage_profile, sha as profile_sha, resolve_backend
from device_startup_policy import debugger_ready_timeout
from device_transport_recovery import restart_user_coredevice
from apple_text_trace import validate_text_trace
from device_build_identity import verify_installed_build
from device_run_evidence import inspect_stderr

PROJECT = Path(__file__).resolve().parent.parent
BUNDLE = "com.llayerlinux.LSX4RuntimeProbe"

def render_progress(report):
    samples=report.get('samples',[])
    if len(samples)<3:
        return {'renderingContinued':False,'reason':'Not enough timing samples'}
    first,last=samples[-3],samples[-1]
    elapsed=last['seconds']-first['seconds']
    flips=last['flips']-first['flips']
    return {'renderingContinued':elapsed>0 and flips>0,
        'tailSeconds':elapsed,'tailGuestFlips':flips,
        'tailGuestFps':flips/elapsed if elapsed>0 else 0,
        'gameplayVerified':False}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device',required=True)
    parser.add_argument('--app',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--interpreter',action='store_true')
    parser.add_argument('--unlinked-fixture',action='store_true',help='Run shared state/fault/SIMD contracts without block linking; fixture only')
    parser.add_argument('--heap-fixture',action='store_true',help='Fresh-process actual Aerolib fallback allocator contract')
    parser.add_argument('--fex-adapter-fixture',action='store_true',help='Synthetic FEX adapter app only: detach after its owned executable pool is prepared')
    parser.add_argument('--skip-install',action='store_true')
    parser.add_argument('--game',help='Game executable relative to probe Documents')
    parser.add_argument('--cpu-profile',type=Path,help='Resolved Android INI for this game, copied and read back before launch')
    parser.add_argument('--managed-launch',choices=('off','options','full'),
        help='Isolated Android launch bridge A/B: switches only, or switches plus physical scene scale')
    parser.add_argument('--physical-drs-policy',choices=('mapped','legacy'),help='MoltenVK mapping of Android physical DRS lifetime quarantine')
    parser.add_argument('--render-diagnostics',choices=('quiet','legacy','verbose'),help='Isolated Apple mapping of Android production diagnostic gates')
    parser.add_argument('--vulkan-driver-policy',choices=('off','submit','on'),help='Isolated Android Vulkan default mapping; submit changes only queue coalescing')
    parser.add_argument('--render-launch',choices=('off','on'),help='Isolated original Android source/output/upscaler and surface geometry bridge')
    parser.add_argument('--managed-readback-mode',choices=('off','on'),help='Isolated same-binary Android effective-readback-mode bridge control')
    parser.add_argument('--managed-disable-options',nargs='+',type=int,help='Diagnostic launch-only managed option IDs1–21 to disable after Android resolution; excludes FPS acceptance')
    parser.add_argument('--mono-selfcheck-objects',choices=('off','on'),help='Isolated native Mono selfcheck fixture A/B; original guest readers remain guarded')
    parser.add_argument('--live-input',action='store_true',help='Finite launch-scoped interactive pad/screenshot diagnostic; excludes FPS acceptance')
    parser.add_argument('--cpu-backend',choices=('current','fex','auto'),default='current',
        help='Explicit engine, or auto: prefer FEX while preserving Android Current/IR memory and execution policies')
    parser.add_argument('--game-stage',choices=['load','relocate','run'],default='load')
    parser.add_argument('--seconds',type=int,default=90,help='Bounded device game observation, 5–600 seconds')
    parser.add_argument('--input-trace',type=Path,help='Timed pad inputs copied into probe Documents')
    parser.add_argument('--text-input-trace',type=Path,help='Opt-in JSON system IME events, bound to the first active session for each label')
    parser.add_argument('--capture-window',help='Explicit GPU captures: start end interval seconds; excluded from FPS claims')
    parser.add_argument('--video-window',help='Native viewport MP4: start end fps, max180s/15fps; no audio/overlay, excludes FPS acceptance')
    parser.add_argument('--video-fragments',action='store_true',help='Experimental recoverable MP4 fragments; physical encoder may fail (-16341)')
    parser.add_argument('--detach-after',type=int,help='Diagnostic A/B: detach the debugger after this many seconds')
    parser.add_argument('--prepared-pool-mib',type=int,help='Prepare 4–512 MiB before detaching and starting the game')
    parser.add_argument('--allocation-failure-trace',action='store_true',help='Log a bounded native stack only on C++ allocation failure')
    parser.add_argument('--metal-shader-validation',action='store_true',help='Diagnostic-only Metal shader instrumentation; report and stop on first invalid GPU access, never accept FPS from this run')
    parser.add_argument('--gpu-command-errors',action='store_true',help='Diagnostic MoltenVK encoder error details and shader conversion logs; excludes FPS acceptance')
    parser.add_argument('--metal-api-validation',action='store_true',help='Diagnostic Metal resource/encoder API checks; asserts on invalid calls and excludes FPS acceptance')
    parser.add_argument('--abort-stack',action='store_true',help='Terminal SIGABRT native stack; requires the isolated Apple abort-stack build; excludes FPS acceptance')
    parser.add_argument('--metal-command-prefill',choices=('submit','end'),help='Diagnostic MoltenVK command/resource ownership A/B: encode at queue submit or command-buffer end; excludes FPS acceptance')
    parser.add_argument('--metal-live-resource-check',choices=('off','on'),help='Diagnostic MoltenVK live resource tracking; excludes FPS acceptance')
    parser.add_argument('--metal-argument-buffers',choices=('off','on'),help='Diagnostic MoltenVK argument-buffer/residency route; excludes FPS acceptance')
    parser.add_argument('--shader-dumps',nargs='+',type=lambda value:int(value,0),help='Capture up to64 shader binaries/SPIR-V files for explicit program hashes; diagnostic only')
    parser.add_argument('--metal-safe-math',action='store_true',help='Diagnostic MoltenVK shader-compiler A/B without fast math; excludes FPS acceptance')
    parser.add_argument('--gpu-content-probe',action='store_true',help='Diagnostic-only guest/GPU buffer comparison for three draws; excludes FPS acceptance')
    parser.add_argument('--gpu-content-dumps',action='store_true',help='Save bounded sampled/attachment bytes; requires --gpu-content-probe')
    parser.add_argument('--gpu-content-extent',type=int,nargs=2,metavar=('WIDTH','HEIGHT'),help='Bounded diagnostic dumps of a sampled texture of this extent; requires --gpu-content-probe')
    parser.add_argument('--gpu-content-probe-first-draw',type=int,default=1,help='First of three diagnostic GPU draw calls to inspect')
    parser.add_argument('--gpu-content-probe-fragment-hash',type=lambda value:int(value,0),help='Filter the bounded GPU-content probe to a fragment shader hash; first-draw then counts matching occurrences')
    parser.add_argument('--input-route-trace',action='store_true',help='Diagnostic-only host/system-UI/guest button routing; excludes FPS acceptance')
    parser.add_argument('--capture-runtime-stacks',action='store_true',help='Diagnostic-only native stack snapshot after observation, including a black screen with advancing counters; excludes FPS acceptance')
    parser.add_argument('--capture-stall-stacks',action='store_true',help='Attach once after a stopped frame counter to collect bounded native stacks; excludes FPS acceptance')
    parser.add_argument('--readback-trace',action='store_true',help='Diagnostic-only page ownership logs; do not use this run for FPS acceptance')
    parser.add_argument('--progress-trace',action='store_true',help='Preserve existing five-second memory/code samples in stderr; diagnostic-only')
    parser.add_argument('--module-stage-trace',action='store_true',help='Diagnostic-only common module paths and load addresses; do not accept FPS from this run')
    parser.add_argument('--jit-rip-probe',help='Diagnostic-only pairs of guest RIP and return RIP (0 matches any return); maximum 32 pairs; excludes FPS acceptance')
    parser.add_argument('--jit-memory-edges',action='store_true',help='Diagnostic-only linked JIT with memory-published register edges; preserves the Android profile, excludes FPS acceptance')
    parser.add_argument('--jit-basic-edges',action='store_true',help='Diagnostic-only ordinary block exits with unchanged profile/lowering; excludes FPS acceptance')
    parser.add_argument('--jit-local-xmm',action='store_true',help='Diagnostic-only linked JIT with per-block SIMD caches; preserves the Android profile, excludes FPS acceptance')
    parser.add_argument('--jit-perf-map',action='store_true',help='Diagnostic-only generated-code address map for physical CPU samples; excludes FPS acceptance')
    parser.add_argument('--guest-hot-sampler',action='store_true',help='Diagnostic-only sparse guest RIP samples grouped by thread; excludes FPS acceptance')
    parser.add_argument('--memory-owner-trace',action='store_true',help='Diagnostic-only malloc/Metal counters and their query duration every five seconds')
    parser.add_argument('--quiet-final-seconds',type=int,default=0,help='Suspend device polling for the final 0–18 seconds of a prepared-pool run')
    parser.add_argument('--live-query-timeout','--process-query-timeout',dest='live_query_timeout',type=int,default=8,help='Bound live process/report reads to 8–30 seconds; video export over Wi-Fi can occupy CoreDevice longer than eight seconds')
    parser.add_argument('--audible',action='store_true',help='Enable game sound for an attended run')
    parser.add_argument('--leave-running',action='store_true',help='After a successful observation, leave the game in the foreground for manual play')
    args=parser.parse_args()
    if args.abort_stack:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-abort-stack' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or not enabled:
            parser.error('--abort-stack requires a timed game and its isolated experiment build')
    if args.metal_command_prefill and (not args.game or args.game_stage!='run'):
        parser.error('--metal-command-prefill requires a timed game')
    if (args.metal_live_resource_check or args.metal_argument_buffers) and (not args.game or args.game_stage!='run'):
        parser.error('Metal resource lifetime controls require a timed game')
    if args.physical_drs_policy:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-physical-drs-policy' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or not enabled:
            parser.error('--physical-drs-policy requires its experiment build and a timed game')
    if args.render_diagnostics:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-render-diagnostics' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or not enabled:
            parser.error('--render-diagnostics requires its experiment build and a timed game')
    if args.vulkan_driver_policy:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-vulkan-driver-policy' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or not enabled:
            parser.error('--vulkan-driver-policy requires its experiment build and a timed game')
    if args.render_launch:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-android-presentation' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if (not args.game or args.game_stage!='run' or not enabled or
            (args.render_launch=='on' and args.managed_launch not in ('options','full'))):
            parser.error('--render-launch requires its presentation experiment build and timed game')
    if args.managed_readback_mode:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-managed-readback-mode' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or not enabled:
            parser.error('--managed-readback-mode requires its experiment build and a timed game')
    if args.managed_disable_options:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-managed-option-control' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if (args.managed_launch not in ('options','full') or not enabled or
            any(not 1<=v<=21 for v in args.managed_disable_options) or
            len(set(args.managed_disable_options))!=len(args.managed_disable_options)):
            parser.error('--managed-disable-options requires its experiment build, options/full and unique IDs1–21')
    if args.mono_selfcheck_objects:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-mono-selfcheck-objects' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not enabled: parser.error('--mono-selfcheck-objects requires its experiment build')
    if args.live_input:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='apple-live-controls' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or args.input_trace or args.capture_window or not enabled:
            parser.error('--live-input requires a timed game with its experiment build, without input trace/capture window')
    if args.managed_launch:
        try:
            evidence=json.loads((args.app.parent/'build-evidence.json').read_text())
            enabled=any(layer.get('name')=='android-managed-launch' for layer in evidence['experiments'])
        except (OSError,ValueError,KeyError,TypeError): enabled=False
        if not args.game or args.game_stage!='run' or not enabled:
            parser.error('--managed-launch requires a timed game and a build with the managed-launch experiment')
    if args.cpu_backend == 'auto' and (not args.game or not args.cpu_profile):
        parser.error('--cpu-backend auto requires --game and a resolved --cpu-profile')
    if args.fex_adapter_fixture and (args.game or args.game_stage!='load' or args.interpreter or
                                    args.heap_fixture or args.unlinked_fixture or args.prepared_pool_mib or args.detach_after):
        parser.error('--fex-adapter-fixture requires its dedicated native fixture app without a game or other fixture mode')
    if args.heap_fixture and (args.game or args.game_stage!='load'):
        parser.error('--heap-fixture requires a fresh fixture process without a game')
    if args.unlinked_fixture and (args.game or args.game_stage!='load'):
        parser.error('--unlinked-fixture requires a fresh fixture process without a game')
    subprocess.Popen(['/usr/bin/caffeinate','-dimsu','-w',str(os.getpid())],
                     stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    if not 5<=args.seconds<=600: parser.error('--seconds must be 5–600')
    if args.detach_after is not None and not 5<=args.detach_after<args.seconds:
        parser.error('--detach-after must be at least 5 and less than --seconds')
    if args.prepared_pool_mib is not None and (args.prepared_pool_mib<4 or args.prepared_pool_mib>512 or args.prepared_pool_mib%4):
        parser.error('--prepared-pool-mib must be 4–512 in multiples of 4')
    if args.prepared_pool_mib and args.detach_after: parser.error('Choose one detach mode')
    if not 0<=args.quiet_final_seconds<=18 or args.quiet_final_seconds>=args.seconds:
        parser.error('--quiet-final-seconds must be 0–18 and shorter than the run')
    if not 8<=args.live_query_timeout<=30:
        parser.error('--live-query-timeout must be 8–30')
    if args.quiet_final_seconds and (not args.prepared_pool_mib or args.game_stage!='run'):
        parser.error('A quiet final window requires a prepared-pool timed game run')
    if args.game_stage=='run' and (not args.game or args.interpreter):
        parser.error('Timed game observations require a game and the attached native debugger')
    if args.metal_shader_validation and args.game_stage!='run':
        parser.error('--metal-shader-validation requires a timed game observation')
    if args.guest_hot_sampler and args.game_stage!='run':
        parser.error('--guest-hot-sampler requires a timed game observation')
    if args.jit_rip_probe is not None:
        values=args.jit_rip_probe.split()
        try:
            numbers=[int(v,0) for v in values]
        except ValueError:
            parser.error('--jit-rip-probe requires integer address pairs')
        if not numbers or len(numbers)%2 or len(numbers)>64 or any(n<0 or n>0xffffffffffffffff for n in numbers) or any(not n for n in numbers[::2]):
            parser.error('--jit-rip-probe requires 1–32 valid nonzero RIP / return RIP pairs')
    if args.gpu_content_dumps and not args.gpu_content_probe:
        parser.error('--gpu-content-dumps requires --gpu-content-probe')
    if args.gpu_content_extent and (not args.gpu_content_probe or any(not 1<=n<=16384 for n in args.gpu_content_extent)):
        parser.error('--gpu-content-extent requires --gpu-content-probe and dimensions1–16384')
    if not 1<=args.gpu_content_probe_first_draw<=0xffffffff:
        parser.error('--gpu-content-probe-first-draw must be1–4294967295')
    if args.gpu_content_probe_first_draw!=1 and not args.gpu_content_probe:
        parser.error('--gpu-content-probe-first-draw requires --gpu-content-probe')
    # Reject incompatible timing before device traffic or costly JIT preparation.
    if args.input_trace:
        try:
            previous=0.0
            count=0
            for number,line in enumerate(args.input_trace.read_text().splitlines(),1):
                line=line.strip()
                if not line or line.startswith('#'): continue
                fields=line.split()
                when=float(fields[0])
                count+=1
                if not math.isfinite(when) or not previous<=when<args.seconds or count>1000:
                    raise ValueError(f'line {number}: invalid time/order/event limit')
                if len(fields)==4 and fields[1]=='axis':
                    if not 0<=int(fields[2])<=5 or not 0<=int(fields[3])<=255:
                        raise ValueError(f'line {number}: axis/value outside pad range')
                elif len(fields)==3:
                    mask=int(fields[1],16)
                    if (mask<=0 or (mask & ~0x11ffff)) or int(fields[2]) not in (0,1):
                        raise ValueError(f'line {number}: invalid button mask/state')
                else:
                    raise ValueError(f'line {number}: expected button or axis event')
                previous=when
        except (OSError,ValueError,IndexError) as error:
            parser.error('Invalid input trace: '+str(error))
    text_trace_events=None
    if args.text_input_trace:
        if args.game_stage!='run': parser.error('--text-input-trace requires a timed game run')
        try:
            text_trace_events=validate_text_trace(args.text_input_trace,args.seconds)
        except (OSError,ValueError,OverflowError) as error:
            parser.error('Invalid text input trace: '+str(error))
    if args.capture_window:
        try:
            begin,finish,interval=map(float,args.capture_window.split())
            if not all(map(math.isfinite,(begin,finish,interval))) or not 0<=begin<finish<=args.seconds or interval<1:
                raise ValueError('window is outside the observation duration')
        except ValueError as error:
            parser.error('Invalid capture window: '+str(error))
    if args.gpu_content_probe_fragment_hash is not None:
        if not args.gpu_content_probe or not 0 < args.gpu_content_probe_fragment_hash < 2**64:
            parser.error('--gpu-content-probe-fragment-hash requires --gpu-content-probe and a nonzero64-bit hash')
    if args.video_fragments and not args.video_window:
        parser.error('--video-fragments requires --video-window')
    if args.video_window:
        if args.game_stage!='run' or args.capture_window:
            parser.error('--video-window requires a timed game run without --capture-window')
        try:
            begin,finish,fps=map(float,args.video_window.split())
            if not all(map(math.isfinite,(begin,finish,fps))) or not 0<=begin<finish<=args.seconds-5 or finish-begin>180 or not 1<=fps<=15:
                raise ValueError('expected bounded window <=180s, fps1–15, ending5s before observation')
        except ValueError as error:
            parser.error('Invalid video window: '+str(error))
    # Coordinate recovery with the awake watchdog through install/launch/LLDB.
    lease_path=PROJECT/'.build/device-probe.lease'
    lease_path.parent.mkdir(parents=True,exist_ok=True)
    lease=lease_path.open('a')
    fcntl.flock(lease,fcntl.LOCK_EX)
    args.output.mkdir(parents=True,exist_ok=True)
    # Replace any prior identity before setup can fail in a reused output
    # folder. Neither old stderr nor old awake recovery can prove this attempt.
    run_id=str(uuid.uuid4())
    (args.output/'probe-launch.json').write_text(json.dumps({
        'runId':run_id,'bundleIdentifier':BUNDLE,'game':args.game},indent=2)+'\n')
    diagnostic_flags=[name for name in ('abort_stack','module_stage_trace','readback_trace','progress_trace','memory_owner_trace','jit_rip_probe','jit_memory_edges','jit_basic_edges','jit_local_xmm','jit_perf_map','shader_dumps','guest_hot_sampler','gpu_content_probe','allocation_failure_trace','metal_shader_validation','gpu_command_errors','metal_api_validation','metal_safe_math','input_route_trace','capture_stall_stacks','capture_runtime_stacks','video_window','capture_window','live_input','managed_disable_options') if getattr(args,name)]
    if args.render_diagnostics in ('legacy','verbose'):diagnostic_flags.append('render_diagnostics')
    if args.physical_drs_policy=='legacy':diagnostic_flags.append('physical_drs_legacy')
    if args.metal_command_prefill:diagnostic_flags.append('metal_command_prefill')
    if args.metal_live_resource_check:diagnostic_flags.append('metal_live_resource_check')
    if args.metal_argument_buffers:diagnostic_flags.append('metal_argument_buffers')
    (args.output/'diagnostics.json').write_text(json.dumps({'flags':diagnostic_flags,'fpsAcceptanceEligible':not diagnostic_flags,
        'managedLaunchRequested':args.managed_launch,'monoSelfcheckObjectsRequested':args.mono_selfcheck_objects,
        'physicalDrsPolicyRequested':args.physical_drs_policy,
        'metalCommandPrefillRequested':args.metal_command_prefill,
        'metalLiveResourceCheckRequested':args.metal_live_resource_check,
        'metalArgumentBuffersRequested':args.metal_argument_buffers,
        'renderDiagnosticsRequested':args.render_diagnostics,
        'vulkanDriverPolicyRequested':args.vulkan_driver_policy,
        'managedReadbackModeRequested':args.managed_readback_mode,'renderLaunchRequested':args.render_launch,
        'managedDisabledOptionsRequested':args.managed_disable_options},indent=2)+'\n')
    # Preserve the exact symbol-bearing image before another local build can
    # replace it. APFS cloning keeps repeated device observations inexpensive.
    binary=args.app.resolve()/'LSX4RuntimeProbe'
    archived_binary=args.output/'LSX4RuntimeProbe'
    subprocess.run(['cp','-c',str(binary),str(archived_binary)],check=True)
    (args.output/'build.json').write_text(json.dumps({
        'binarySha256':hashlib.sha256(archived_binary.read_bytes()).hexdigest(),
        'binaryBytes':archived_binary.stat().st_size,
        'sourceApp':str(args.app.resolve()),
        'archivedBinary':str(archived_binary.resolve())},indent=2)+'\n')
    observation_active=False
    file_transport_recovered=False
    def device(arguments,name):
        nonlocal file_transport_recovered
        output=args.output/(name+'.json')
        checking_processes=arguments[:2]==['info','processes']
        file_copy=arguments[:2] in (['copy','to'],['copy','from'])
        recover_file_copy=file_copy and not observation_active and not file_transport_recovered
        attempts=2 if (checking_processes and not observation_active) or recover_file_copy else 1
        for attempt in range(attempts):
            log_name=name if not attempt else name+'-retry'
            with (args.output/(log_name+'.log')).open('w') as log:
                live_read=observation_active and (checking_processes or arguments[:2]==['copy','from'])
                timeout=args.live_query_timeout if live_read else (8 if observation_active else (90 if arguments[:2]==['install','app'] else 25))
                # The default live-call budget keeps ordinary recovery short.
                # Explicit video runs can extend read-only queries while Xcode
                # exports a recording over Wi-Fi. The caller must exclude any
                # overlapping transport/recording work from its FPS window.
                tool_timeout=max(5,timeout)
                try:
                    subprocess.run(['xcrun','devicectl','device',*arguments[:2],'--device',args.device,
                        *arguments[2:],'--timeout',str(tool_timeout),'--json-output',str(output)],
                        stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout+1)
                    break
                except (subprocess.CalledProcessError,subprocess.TimeoutExpired) as error:
                    timed_out=isinstance(error,subprocess.TimeoutExpired) or 'Command timeout' in (args.output/(log_name+'.log')).read_text(errors='replace')
                    if recover_file_copy and attempt==0 and timed_out:
                        # Only idempotent file transfers, before attach or after
                        # observation/recovery, under this probe's device lease.
                        # Never restart transport during gameplay or replay a
                        # process launch/install after an uncertain response.
                        file_transport_recovered=True
                        restarted=restart_user_coredevice()
                        (args.output/'file-transport-recovery.json').write_text(json.dumps({
                            'operation':arguments[:2],'name':name,'restartedPids':restarted,
                            'duringObservation':False,'launchReplayed':False})+'\n')
                        print('Recovered CoreDevice file transport; retrying '+name,flush=True)
                        continue
                    if not checking_processes: raise
                    if attempt+1==attempts:
                        raise RuntimeError('Apple device transport failed during process liveness check') from error
                    # A CoreDevice service timeout is not evidence of a guest
                    # crash. Retry this read once, then recover the awake UI.
                    print('Retrying transient CoreDevice process check',flush=True)
        return json.loads(output.read_text())['result']
    def restore_awake():
        attempts=[(['process','launch','--terminate-existing','--environment-variables',
            '{"LSX4_CORE_KEEP_AWAKE":"1"}',BUNDLE],'keep-awake-final'),
            (['process','launch','com.llayerlinux.LSX4iOS'],'keep-awake-fallback')]
        for arguments,name in attempts:
            try:
                device(arguments,name)
                return True
            except Exception as error:
                print('Awake recovery attempt failed: '+str(error),flush=True)
        return False
    if not args.skip_install:
        device(['install','app',str(args.app.resolve())],'install')
    # The lease serializes traffic, but a queued probe can lose a race to an
    # install before acquiring it. Never pair stale local symbols with that app.
    try:
        local_info=plistlib.loads((args.app.resolve()/'Info.plist').read_bytes())
        identity=verify_installed_build(device(['info','apps','--bundle-id',BUNDLE],
            'installed-app'),local_info)
    except Exception:
        restore_awake()
        raise
    (args.output/'installed-build.json').write_text(json.dumps(identity,indent=2)+'\n')
    started=time.time()
    # A previous device report can be copied while the new run is still active.
    # Publish report.json only after its launch-time check, so live reviewers
    # cannot mistake a stale candidate for this run's completed measurement.
    for name in ('report.json', 'report-evidence.json'):
        (args.output/name).unlink(missing_ok=True)
    environment={'LSX4_CORE_INTERPRETER':'1' if args.interpreter else '0',
        'LSX4_PROBE_RUN_ID':run_id}
    if args.heap_fixture: environment['LSX4_CORE_AEROLIB_HEAP_FIXTURE']='1'
    if args.live_input:
        environment['LSX4_PROBE_LIVE_INPUT']='1'
        (args.output/'live-control.json').write_text(json.dumps({'runId':run_id,
            'device':args.device,'directory':'Documents/live-control-'+run_id,
            'fpsAcceptanceEligible':False},indent=2)+'\n')
    if args.mono_selfcheck_objects: environment['LSX4_APPLE_MONO_SELFCHECK_OBJECTS']='1' if args.mono_selfcheck_objects=='on' else '0'
    if args.managed_launch: environment['LSX4_APPLE_MANAGED_LAUNCH']=args.managed_launch
    if args.physical_drs_policy: environment['LSX4_APPLE_PHYSICAL_DRS_POLICY']=args.physical_drs_policy
    if args.render_diagnostics: environment['LSX4_APPLE_RENDER_DIAGNOSTICS']=args.render_diagnostics
    if args.render_diagnostics=='verbose': environment['LSX4_RUNTIME_VERBOSE']='1'
    if args.vulkan_driver_policy: environment['LSX4_APPLE_VULKAN_POLICY']=args.vulkan_driver_policy
    if args.render_launch: environment['LSX4_APPLE_RENDER_LAUNCH']=args.render_launch
    if args.managed_readback_mode: environment['LSX4_APPLE_MANAGED_READBACK_MODE']='1' if args.managed_readback_mode=='on' else '0'
    if args.managed_disable_options: environment['LSX4_APPLE_MANAGED_DISABLE_MASK']=str(sum(1<<v for v in args.managed_disable_options))
    if args.unlinked_fixture: environment['LSX4_TEST_UNLINKED_NATIVE']='1'
    if args.allocation_failure_trace: environment['LSX4_PROBE_ALLOCATION_FAILURE_TRACE']='1'
    if args.gpu_command_errors:
        environment.update(MVK_CONFIG_DEBUG='1',LSX4_PROBE_GPU_MARKERS='1')
    if args.metal_safe_math: environment['MVK_CONFIG_FAST_MATH_ENABLED']='0'
    if args.abort_stack: environment['LSX4_PROBE_ABORT_STACK']='1'
    if args.metal_command_prefill:
        environment['MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS']={'submit':'0','end':'1'}[args.metal_command_prefill]
    if args.metal_live_resource_check:
        environment['MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES']='1' if args.metal_live_resource_check=='on' else '0'
    if args.metal_argument_buffers:
        environment['MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS']='1' if args.metal_argument_buffers=='on' else '0'
    if args.metal_api_validation:
        environment.update(MTL_DEBUG_LAYER='1',MTL_DEBUG_LAYER_ERROR_MODE='assert',
            MTL_DEBUG_LAYER_VALIDATE_UNRETAINED_RESOURCES='5')
    if args.metal_shader_validation:
        # Apple's shader validator changes shaders and their memory behavior.
        # Stop at the first fault and keep it out of all performance acceptance.
        environment.update(MTL_SHADER_VALIDATION='1',
            MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING='1',
            MTL_SHADER_VALIDATION_REPORT_TO_STDERR='1',
            MTL_SHADER_VALIDATION_ABORT_ON_FAULT='1',
            MTL_SHADER_VALIDATION_DUMP_PIPELINES='1')
    if args.shader_dumps:
        environment.update(LSX4_PROBE_SHADER_DUMP='1',EXECUTOR_SHADER_DUMP_HASHES=' '.join(hex(value) for value in args.shader_dumps))
    if args.gpu_content_probe:
        environment.update(LSX4_RUNTIME_VERBOSE='1',EXECUTOR_GPU_CONTENT_PROBE='1',
            EXECUTOR_GPU_CONTENT_PROBE_DRAW_FIRST=str(args.gpu_content_probe_first_draw),EXECUTOR_GPU_CONTENT_PROBE_DRAW_COUNT='3',
            EXECUTOR_GPU_CONTENT_PROBE_DRAW_STRIDE='1',EXECUTOR_GPU_CONTENT_PROBE_BUFFERS='1',
            EXECUTOR_GPU_CONTENT_PROBE_BUDGET='6',EXECUTOR_GPU_CONTENT_PROBE_MAX_BYTES='8388608',
            EXECUTOR_GPU_CONTENT_PROBE_DISPATCH_COUNT='0')
    if args.gpu_content_extent:
        environment.update(EXECUTOR_GPU_CONTENT_SAMPLE_WIDTH=str(args.gpu_content_extent[0]),
            EXECUTOR_GPU_CONTENT_SAMPLE_HEIGHT=str(args.gpu_content_extent[1]),LSX4_PROBE_GPU_TEXTURE_DUMP='1')
    if args.gpu_content_dumps:
        environment['LSX4_PROBE_GPU_TEXTURE_DUMP']='1'
    if args.gpu_content_probe_fragment_hash is not None:
        environment['EXECUTOR_GPU_CONTENT_PROBE_DRAW_HASH']=hex(args.gpu_content_probe_fragment_hash)
    if args.input_route_trace: environment['LSX4_PROBE_INPUT_ROUTE']='1'
    if args.readback_trace:
        environment.update(LSX4_RUNTIME_VERBOSE='1',EXECUTOR_READBACK_CONTRACT_TRACE='1')
    if args.progress_trace: environment['LSX4_PROBE_PROGRESS_TRACE']='1'
    if args.module_stage_trace:
        environment.update(EXECUTOR_VERBOSE_MODULE_STAGE='1',LSX4_RUNTIME_VERBOSE='1')
    if args.jit_rip_probe is not None:
        # Apple uses the common JIT's environment parser. Android's sentinel
        # loader is inside __ANDROID__; no persistent file is needed here.
        environment.update(EXECUTOR_DIAG_JIT_RIP_PROBE=args.jit_rip_probe,LSX4_RUNTIME_VERBOSE='1')
    if args.guest_hot_sampler:
        environment['EXECUTOR_JIT_GUEST_HOT_SAMPLER']='1'
    if args.jit_memory_edges: environment['EXECUTOR_DIAG_JIT_MEMORY_EDGES']='1'
    if args.jit_basic_edges: environment['EXECUTOR_DIAG_JIT_BASIC_EDGES']='1'
    if args.jit_local_xmm: environment['EXECUTOR_DIAG_JIT_LOCAL_XMM']='1'
    if args.jit_perf_map: environment['LSX4_PROBE_JIT_PERF_MAP']='1'
    if args.memory_owner_trace: environment['LSX4_PROBE_MEMORY_OWNER_TRACE']='1'
    capture_path=None
    video_path=None
    if args.game:
        environment.update(LSX4_CORE_GAME=args.game,LSX4_CORE_GAME_STAGE=args.game_stage)
        if args.game_stage!='run': environment['LSX4_RUNTIME_VERBOSE']='1'
        profile=stage_profile(args.cpu_profile,args.output,Path(args.game).parent.name)
        backend=resolve_backend(args.cpu_backend,profile)
        environment['LSX4_APPLE_CPU_BACKEND']=backend['effective']
        (args.output/'cpu-backend.json').write_text(json.dumps(backend,indent=2)+'\n')
        device(['copy','to','--source',str(profile.parent.resolve()),
            '--destination','Documents/shared-runtime/runtime-config',
            '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-cpu-profile')
        readback=args.output/'cpu-profile-readback.ini'
        device(['copy','from','--source','Documents/shared-runtime/runtime-config/'+CPU_PROFILE_NAME,
            '--destination',str(readback),'--domain-type','appDataContainer',
            '--domain-identifier',BUNDLE],'verify-cpu-profile')
        if profile_sha(profile)!=profile_sha(readback):raise RuntimeError('CPU profile transfer mismatch')
    if args.game_stage=='run':
        environment.update(LSX4_PROBE_SECONDS=str(args.seconds))
        if args.leave_running: environment['LSX4_PROBE_LEAVE_RUNNING']='1'
        if args.audible:
            environment.update(LSX4_PROBE_AUDIO_VOLUME='100',SDL_AUDIO_CATEGORY='playback')
        else:
            environment['LSX4_PROBE_AUDIO_MUTE']='1'
        if args.detach_after: environment['LSX4_PROBE_DETACH_SECONDS']=str(args.detach_after)
        if args.prepared_pool_mib: environment['LSX4_PROBE_PREPARED_POOL_MIB']=str(args.prepared_pool_mib)
        if args.capture_window:
            capture_path='Documents/device-captures/'+str(uuid.uuid4())
            environment.update(LSX4_PROBE_CAPTURE_DIR=capture_path,LSX4_PROBE_CAPTURE_WINDOW=args.capture_window)
        if args.video_window:
            video_path='Documents/device-video/'+str(uuid.uuid4())
            environment.update(LSX4_PROBE_VIDEO_DIR=video_path,LSX4_PROBE_VIDEO_WINDOW=args.video_window)
            if args.video_fragments: environment['LSX4_PROBE_VIDEO_FRAGMENTS']='1'
        if args.input_trace:
            device(['copy','to','--source',str(args.input_trace.resolve()),'--destination','Documents/device-input.txt',
                '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-input')
            environment['LSX4_PROBE_INPUT_TRACE']='Documents/device-input.txt'
        if args.text_input_trace:
            # Stage the validated snapshot, not a source file that could change
            # after validation. The trace is explicit test input, never a default name.
            staged=args.output/'text-input-trace.json'
            staged.write_text(json.dumps(text_trace_events,ensure_ascii=False)+'\n',encoding='utf-8')
            device(['copy','to','--source',str(staged.resolve()),'--destination','Documents/device-text-input.json',
                '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-text-input')
            environment['LSX4_PROBE_TEXT_INPUT_TRACE']='Documents/device-text-input.json'
            (args.output/'text-input-metadata.json').write_text(json.dumps({
                'sha256':hashlib.sha256(staged.read_bytes()).hexdigest(),'events':len(text_trace_events),
                'sessionPolicy':'first-observed-token-no-rebinding'},indent=2)+'\n')
    failed=False
    try:
        launch=device(['process','launch','--terminate-existing','--environment-variables',
            json.dumps(environment),BUNDLE],'launch')
        # LLDB's device select takes a CoreDevice UUID. Reuse the identity from
        # this launch instead of repeating a serial-number discovery after XCTest.
        debug_device_id=launch.get('deviceIdentifier',args.device)
        observation_active=args.game_stage=='run'
        if not args.interpreter:
            spec=importlib.util.spec_from_file_location('lsx4_device_cycle',PROJECT/'device_cycle.py')
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            quote=lambda p:'"'+str(p).replace('\\','\\\\').replace('"','\\"')+'"'
            ready_symbol='LSX4RuntimeProbeTimedRunReady' if args.game_stage=='run' else 'LSX4RuntimeProbeReportReady'
            if args.fex_adapter_fixture: ready_symbol='LSX4FexAdapterPoolReady'
            if args.detach_after: ready_symbol='LSX4RuntimeProbeObservationReady'
            if args.prepared_pool_mib: ready_symbol='LSX4RuntimeProbePreparedPoolReady'
            # The common page tracker handles protected guest writes through
            # SIGBUS/SIGSEGV. Darwin's Mach exception must reach that handler,
            # rather than causing a debugger round trip on every dirty page.
            # This setting belongs only to this short-lived LLDB process.
            commands=['script lldb.debugger.SetAsync(False)',
                'settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS',
                'device select '+debug_device_id,
                'target create '+quote(args.app.resolve()/'LSX4RuntimeProbe'),
                'command script import '+quote(PROJECT/'Debug/lsx4_jit.py'),
                *(['script lsx4_jit.configure_progress('+str(args.prepared_pool_mib*1024*1024)+')'] if args.prepared_pool_mib else []),
                'breakpoint set -n '+ready_symbol,
                *(['breakpoint set -n LSX4RuntimeProbeReportReady'] if args.prepared_pool_mib else []),
                'device process attach -p '+str(launch['process']['processIdentifier']),
                'process handle SIGBUS SIGSEGV -n false -s false -p true',
                'continue','script import json; print("LSX4_JIT_STATS " + json.dumps(lsx4_jit.statistics()))',
                'process detach','quit']
            startup_timeout=debugger_ready_timeout(args.seconds,args.prepared_pool_mib)
            print('JIT_STARTUP_BUDGET '+json.dumps({'seconds':startup_timeout,
                'preparedPoolMiB':args.prepared_pool_mib}),flush=True)
            def recover_pre_attach():
                # debug_device invokes this only after two discovery failures,
                # with its LLDB process closed and before any attach/continue.
                restarted=restart_user_coredevice()
                (args.output/'debugger-transport-recovery.json').write_text(json.dumps({
                    'restartedPids':restarted,'beforeAttach':True,
                    'launchReplayed':False,'attemptsRemaining':1})+'\n')
            module.debug_device(commands,args.output/'lldb.log',startup_timeout,
                ready_pattern=ready_symbol,
                recover_pre_attach=recover_pre_attach,
                failure_pattern=r'stop reason = (?:EXC_|signal)|Process \d+ exited'+('|LSX4RuntimeProbeReportReady' if args.prepared_pool_mib else ''))
        report_path=args.output/'report.json'
        candidate_path=args.output/'report-candidate.json'
        detached=args.detach_after or args.prepared_pool_mib
        observation_start=time.monotonic()
        quiet_start=observation_start+args.seconds-args.quiet_final_seconds
        quiet_end=observation_start+args.seconds+0.5
        deadline=time.monotonic()+(args.seconds+30 if detached else 30)
        while time.monotonic()<deadline:
            if args.quiet_final_seconds and quiet_start<=time.monotonic()<quiet_end:
                # Keep an uninterrupted tail for FPS. The bounded quiet period
                # leaves time for bounded liveness and awake recovery before
                # the phone's 30-second idle timeout if it crashes;
                # early startup failures still receive prompt awake recovery.
                (args.output/'quiet-observation.json').write_text(json.dumps({
                    'startedUnix':time.time(),'seconds':quiet_end-time.monotonic(),
                    'devicePollingSuspended':True})+'\n')
                time.sleep(max(0,quiet_end-time.monotonic()))
            # Check liveness before copying reports. After a guest crash, long
            # sequential CoreDevice timeouts used to consume the idle window.
            if args.game_stage=='run':
                processes=device(['info','processes'],'process-liveness').get('runningProcesses',[])
                if not any('/LSX4RuntimeProbe.app/LSX4RuntimeProbe' in p.get('executable','') for p in processes):
                    raise RuntimeError('Probe exited before publishing a report')
            source='device-timed-run.json' if args.game_stage=='run' else 'shared-core-probe.json'
            device(['copy','from','--source','Documents/'+source,
                '--destination',str(candidate_path),'--domain-type','appDataContainer',
                '--domain-identifier',BUNDLE],'copy-report')
            report=json.loads(candidate_path.read_text())
            if report.get('timestamp',0)>=started:
                candidate_path.replace(report_path)
                (args.output/'report-evidence.json').write_text(json.dumps({
                    'runId':run_id,'launchStartedUnix':started,
                    'reportTimestamp':report['timestamp'],'freshForLaunch':True},indent=2)+'\n')
                break
            if args.game_stage=='run':
                returned=args.output/'runtime-return.json'
                device(['copy','from','--source','Documents/shared-core-probe.json',
                    '--destination',str(returned),'--domain-type','appDataContainer',
                    '--domain-identifier',BUNDLE],'copy-runtime-return')
                ended=json.loads(returned.read_text())
                if ended.get('timestamp',0)>=started:
                    raise RuntimeError('Guest runtime returned before the observation ended; see runtime-return.json')
            time.sleep(3 if detached else 0.5)
        else: raise RuntimeError('No report from this launch')
        print(json.dumps(report,indent=2))
        key='timedRunCompleted' if args.game_stage=='run' else 'passed'
        if not report.get(key): raise RuntimeError('Shared core fixture failed')
        if args.game_stage=='run':
            progress=render_progress(report)
            (args.output/'observation.json').write_text(json.dumps(progress,indent=2)+'\n')
            # A game can keep animating its loader without reaching gameplay.
            # Allow the reviewer to request one snapshot at the observation
            # boundary, using this probe's lease instead of a competing attach.
            # UUID binding prevents an old request from affecting a later run.
            stack_request_path=args.output/'runtime-stacks-request.json'
            stack_request=json.loads(stack_request_path.read_text()) if stack_request_path.exists() else {}
            capture_runtime_stacks=args.capture_runtime_stacks or stack_request.get('runId')==run_id
            if capture_runtime_stacks or (args.capture_stall_stacks and not progress['renderingContinued']):
                # Observation and recording have ended. Read stacks only:
                # evaluating guest/host functions while other threads are
                # suspended can deadlock the very process under diagnosis.
                process=next(p for p in processes if '/LSX4RuntimeProbe.app/LSX4RuntimeProbe' in p.get('executable',''))
                try:
                    if args.interpreter:
                        spec=importlib.util.spec_from_file_location('lsx4_device_cycle',PROJECT/'device_cycle.py')
                        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.debug_device([
                        'script lldb.debugger.SetAsync(False)',
                        'settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS',
                        'device select '+debug_device_id,
                        'target create '+json.dumps(str(args.output/'LSX4RuntimeProbe')),
                        'device process attach -p '+str(process['processIdentifier']),
                        'process handle SIGBUS SIGSEGV -n false -s false -p true',
                        'thread list','thread backtrace all -c 64',
                        'command script import '+json.dumps(str(Path(__file__).with_name('lldb_native_state.py'))),
                        'lsx4-native-state','image list -o -f',
                        'process detach','quit'],args.output/('runtime-stacks.log' if capture_runtime_stacks else 'stall-stacks.log'),60)
                except Exception as error:
                    (args.output/('runtime-stacks-error.txt' if capture_runtime_stacks else 'stall-stacks-error.txt')).write_text(str(error)+'\n')
            if not progress['renderingContinued']:
                raise RuntimeError('Game frame counter stopped; elapsed observation time is not a successful run')
            # A failure while the final sample is being published must not be
            # reported as a healthy handoff merely because its counters grew.
            time.sleep(1)
            returned=args.output/'runtime-return-after-observation.json'
            device(['copy','from','--source','Documents/shared-core-probe.json',
                '--destination',str(returned),'--domain-type','appDataContainer',
                '--domain-identifier',BUNDLE],'copy-runtime-return-after-observation')
            ended=json.loads(returned.read_text())
            progress['runtimeStillRunningAfterObservation']=ended.get('timestamp',0)<started
            (args.output/'observation.json').write_text(json.dumps(progress,indent=2)+'\n')
            if not progress['runtimeStillRunningAfterObservation']:
                raise RuntimeError('Guest runtime returned at the observation boundary; see runtime-return-after-observation.json')
    except BaseException:
        failed=True
        raise
    finally:
        recovery_attempted=failed or (args.game_stage=='run' and not args.leave_running)
        restored=restore_awake() if recovery_attempted else True
        (args.output/'probe-recovery.json').write_text(json.dumps({
            'runId':run_id,'attempted':recovery_attempted,'awakeRecovered':restored},indent=2)+'\n')
        observation_active=False
        if failed:
            try:
                device(['copy','from','--source','Documents/shared-core-probe.json',
                    '--destination',str(args.output/'startup-or-runtime-return.json'),
                    '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-failure-report')
            except Exception as error: print('Could not collect failure report: '+str(error),flush=True)
        # Persist native fixture failures as well as game failures.
        stderr_source='probe-stderr.log' if args.game else 'fixture-stderr.log'
        for attempt in range(2):
            try:
                device(['copy','from','--source','Documents/shared-runtime/'+stderr_source,
                    '--destination',str(args.output/'stderr.log'),'--domain-type','appDataContainer',
                    '--domain-identifier',BUNDLE],'copy-stderr' if not attempt else 'copy-stderr-retry')
                break
            except Exception as error:
                print('Could not collect stderr: '+str(error),flush=True)
                if not attempt: time.sleep(1)
        if args.jit_perf_map:
            try:
                device(['copy','from','--source',f'Documents/shared-runtime/jit-perf-{run_id}.map',
                    '--destination',str(args.output/'jit-perf.map'),'--domain-type','appDataContainer',
                    '--domain-identifier',BUNDLE],'copy-jit-perf-map')
            except Exception as error: print('Could not collect JIT perf map: '+str(error),flush=True)
        if args.shader_dumps:
            (args.output/'shader-dumps').mkdir(exist_ok=True)
            try:
                device(['copy','from','--source',f'Documents/shared-runtime/shader-dumps-{run_id}',
                    '--destination',str(args.output/'shader-dumps'),'--domain-type','appDataContainer',
                    '--domain-identifier',BUNDLE],'copy-shader-dumps')
            except Exception as error: print('Could not collect shader dumps: '+str(error),flush=True)
        if args.gpu_content_extent or args.gpu_content_dumps:
            (args.output/'sampled-textures').mkdir(exist_ok=True)
            try:
                device(['copy','from','--source',f'Documents/shared-runtime/gpu-textures-{run_id}',
                    '--destination',str(args.output/'sampled-textures'),'--domain-type','appDataContainer',
                    '--domain-identifier',BUNDLE],'copy-sampled-textures')
            except Exception as error: print('Could not collect sampled textures: '+str(error),flush=True)
        if capture_path:
            try:
                device(['copy','from','--source',capture_path,'--destination',str(args.output/'frames'),
                    '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-frames')
            except Exception as error: print('Could not collect frames: '+str(error),flush=True)
        if video_path:
            try:
                device(['copy','from','--source',video_path,'--destination',str(args.output/'viewport-video'),
                    '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-video')
            except Exception as error: print('Could not collect viewport video: '+str(error),flush=True)
        if args.live_input:
            try:
                device(['copy','from','--source','Documents/live-control-'+run_id,
                    '--destination',str(args.output/'live-control'),
                    '--domain-type','appDataContainer','--domain-identifier',BUNDLE],'copy-live-control')
            except Exception as error: print('Could not collect live input evidence: '+str(error),flush=True)
        if not restored and not failed:
            raise RuntimeError('Device observation finished, but foreground awake recovery was not confirmed')
        # A dead guest worker does not necessarily stop the renderer or main
        # runtime. Check the exported current-launch log only after preserving
        # the recording; advancing empty-frame counters cannot hide that fault.
        stderr_path=args.output/'stderr.log'
        if stderr_path.exists():
            evidence=inspect_stderr(stderr_path.read_text(errors='replace'),run_id)
            (args.output/'stderr-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
            if evidence['fatalDiagnostic'] and not failed:
                if not recovery_attempted:
                    restored=restore_awake()
                    (args.output/'probe-recovery.json').write_text(json.dumps({
                        'runId':run_id,'attempted':True,'awakeRecovered':restored},indent=2)+'\n')
                raise RuntimeError('Terminal guest/runtime diagnostic despite frame progress; see stderr-evidence.json')

if __name__=='__main__': main()
