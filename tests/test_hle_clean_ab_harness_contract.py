import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PMU_PATH = ROOT / "tools" / "codex_bloodborne_pmu.sh"
HARNESS_PATH = ROOT / "tools" / "codex_bloodborne_hle_clean_ab.sh"
PMU = PMU_PATH.read_text(encoding="utf-8")
HARNESS = HARNESS_PATH.read_text(encoding="utf-8")


def inline_python(source):
    return re.findall(r"<<'PY'\n(.*?)\nPY", source, flags=re.DOTALL)


def shell_function(source, name):
    marker = f"{name}() {{"
    start = source.index(marker)
    end = source.index("\n}", start) + 2
    return source[start:end]


def bash_executable():
    if os.name == "nt":
        for program_files in (
            os.environ.get("ProgramFiles"),
            os.environ.get("ProgramFiles(x86)"),
        ):
            if not program_files:
                continue
            candidate = Path(program_files) / "Git" / "bin" / "bash.exe"
            if candidate.is_file():
                return str(candidate)
    return shutil.which("bash")


class HleCleanAbHarnessContractTests(unittest.TestCase):
    def test_save_inventory_temp_path_is_initialized_after_output(self):
        capture = shell_function(HARNESS, "capture_save_inventory")
        self.assertIn(
            'local base="$1" leaf="$2" output="$3" temporary\n'
            '    temporary="$output.tmp-$$"',
            capture,
        )
        self.assertNotIn('temporary="$output.tmp-$$"', capture.split("\n", 1)[0])

    def test_stop_game_accepts_pidof_not_found_as_success(self):
        stop_game = shell_function(PMU, "stop_game")
        self.assertIn(
            "'pidof app.lsx4.android app.lsx4.android:game || true'",
            stop_game,
        )
        self.assertIn("STOP_GAME_PID_QUERY_FAILED", stop_game)
        self.assertNotIn(
            "pidof app.lsx4.android app.lsx4.android:game' \\\n+            2>/dev/null | tr -d '\\r[:space:]') || return 1",
            stop_game,
        )

    def test_all_inline_python_is_syntactically_valid(self):
        blocks = inline_python(PMU) + inline_python(HARNESS)
        self.assertGreaterEqual(len(blocks), 4)
        for index, block in enumerate(blocks):
            compile(block, f"hle-clean-inline-{index}.py", "exec")

    def test_exact_device_and_every_adb_is_agentchat_guarded(self):
        for source, expected_direct in (
            (PMU, ['adb -s "$device" "$@"', 'adb -s "$device" "$@" &']),
            (HARNESS, ['adb -s "$device" "$@"', 'adb -s "$device" "$@" &']),
        ):
            direct = [
                line.strip()
                for line in source.splitlines()
                if re.match(r"^\s*adb(?:\s|$)", line)
            ]
            self.assertEqual(direct, expected_direct)
            self.assertIn("agentchat.py", source)
            self.assertIn("event --agent codex", source)
            self.assertIn("own --agent codex", source)
            self.assertLess(source.index(" event "), source.index(" own "))
            self.assertIn("device:vivo", source)
        self.assertIn("expected_device='10AG263F1B001HM'", HARNESS)
        for source in (PMU, HARNESS):
            self.assertIn(
                "^adb-10AG263F1B001HM-[A-Za-z0-9]+[.]_adb-tls-connect[.]_tcp$",
                source,
            )
            self.assertIn("verify_vivo_device_identity", source)
            self.assertIn("shell getprop ro.serialno", source)
            self.assertIn(
                'VIVO_DEVICE_IDENTITY_OK adbTarget=$device ro.serialno=$reported_serial',
                source,
            )
        self.assertNotIn("emulator-5554", HARNESS)
        self.assertNotIn("kill-server", PMU + HARNESS)
        self.assertNotIn("adb install", HARNESS)
        self.assertNotIn("install-vivo", HARNESS)

        pmu_main = PMU.rsplit("verify_canonical_fex_host || exit 51", 1)[1]
        self.assertLess(
            pmu_main.index("verify_vivo_device_identity || exit 69"),
            pmu_main.index("trap cleanup EXIT"),
        )
        self.assertLess(
            pmu_main.index("verify_vivo_device_identity || exit 69"),
            pmu_main.index("stop_game || exit 65"),
        )
        harness_main = HARNESS.rsplit('cd "$repo_root"', 1)[1]
        self.assertLess(
            harness_main.index("verify_vivo_device_identity"),
            harness_main.index("device_session_started=1"),
        )
        self.assertLess(
            harness_main.index("device_session_started=1"),
            harness_main.index("prepare_save_seed"),
        )

        background_wrapper = PMU.split("adb_device_background()", 1)[1].split(
            "host_path()", 1
        )[0]
        self.assertLess(
            background_wrapper.index("guard_device_action"),
            background_wrapper.index('adb -s "$device" "$@" &'),
        )

    def test_run_as_sh_c_is_one_posix_quoted_remote_argument(self):
        bash = bash_executable()
        if bash is None:
            self.skipTest("bash is unavailable")
        payload = (
            "set -eu; cd 'files/lsx4 home'; "
            "printf '%s\\n' \"$HOME\"; echo \"$(id)\""
        )
        quoted_payload = "'" + payload.replace("'", "'\"'\"'") + "'"
        expected_remote = (
            "run-as 'app.lsx4.android' sh -c " + quoted_payload
        ).encode()

        for source in (PMU, HARNESS):
            wrapper = shell_function(source, "adb_device")
            for token in (
                "posix_shell_quote()",
                '( "$1" == shell || "$1" == exec-out )',
                '"$2" == run-as',
                '"${4:-}" == sh',
                '"${5:-}" == -c',
                '[[ "$#" -ne 6 || -z "${3:-}" ]]',
                'set -- "$transport" "run-as $quoted_package sh -c $quoted_script"',
                "ADB_REMOTE_SH_C_INVALID",
            ):
                self.assertIn(token, source if token == "posix_shell_quote()" else wrapper)

            program = "\n".join(
                (
                    "set -u",
                    shell_function(source, "posix_shell_quote"),
                    wrapper,
                    "guard_device_action() { :; }",
                    "adb() { printf '%s\\000' \"$@\"; }",
                    "device=wireless-target",
                    'adb_device "$@"',
                )
            )
            for transport in ("shell", "exec-out"):
                result = subprocess.run(
                    [
                        bash,
                        "-c",
                        program,
                        "--",
                        transport,
                        "run-as",
                        "app.lsx4.android",
                        "sh",
                        "-c",
                        payload,
                    ],
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(
                    result.stdout.split(b"\0")[:-1],
                    [b"-s", b"wireless-target", transport.encode(), expected_remote],
                )

            malformed = subprocess.run(
                [
                    bash,
                    "-c",
                    program,
                    "--",
                    "shell",
                    "run-as",
                    "app.lsx4.android",
                    "sh",
                    "-c",
                    payload,
                    "unexpected-extra-argument",
                ],
                capture_output=True,
                check=False,
            )
            self.assertEqual(malformed.returncode, 64)
            self.assertEqual(malformed.stdout, b"")
            self.assertIn(b"ADB_REMOTE_SH_C_INVALID", malformed.stderr)

    def test_logcat_is_finite_and_cannot_leave_a_background_writer(self):
        for token in (
            "logcat -b all -c",
            "logcat -b all -d -v threadtime",
            '.codex-bb-$tag-startup-logcat.txt',
            '.codex-bb-$tag-measure-logcat.txt',
            '.codex-bb-$tag-logcat.txt',
        ):
            self.assertIn(token, PMU)
        self.assertNotIn("logcat_pid", PMU)
        self.assertNotIn("measurement_log_start", PMU)
        self.assertNotRegex(PMU, r"adb_device\s+logcat[^\n]*&")
        self.assertNotRegex(PMU, r"adb\s+-s[^\n]*logcat[^\n]*&")
        dumps = [
            match.start()
            for match in re.finditer("adb_device logcat -b all -d -v threadtime", PMU)
        ]
        self.assertEqual(len(dumps), 3)
        self.assertIn('.codex-bb-$tag-fex-status-logcat.txt', PMU)
        self.assertIn('> "$status_log"', PMU[dumps[0] : dumps[1]])
        self.assertIn('.codex-bb-$tag-startup-logcat.txt', PMU[dumps[1] : dumps[2]])
        self.assertIn('.codex-bb-$tag-measure-logcat.txt', PMU[dumps[2] :])

    def test_pmu_toggle_is_opt_in_and_process_immutable(self):
        for token in (
            'hle_clean_mode="${BB_HLE_CLEAN_BUFFER_FASTPATH:-}"',
            'hle_clean_stats_mode="${BB_HLE_CLEAN_DIAGNOSTIC_STATS:-0}"',
            'strict_phase_gates="${BB_STRICT_PHASE_GATES:-0}"',
            "run-enable-hle-clean-buffer-fastpath",
            "run-diagnostic-hle-clean-buffer-stats",
            "HLE_CLEAN_MARKER_STATE expected=",
            "HLE_CLEAN_STATS_MARKER_STATE expected=",
            "POST_LAUNCH_HLE_CLEAN_MARKER",
            "POST_LAUNCH_HLE_CLEAN_STATS_MARKER",
            "EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH=1",
            "HLE_CLEAN_PROCESS_ENV expected=",
            "HLE_CLEAN_MARKER_CLEAN=1",
            "HLE_CLEAN_STATS_MARKER_CLEAN=1",
            "KEEP_RUNNING_REQUIRES_BASELINE_HLE_CLEAN_MODE=0",
            "HLE_CLEAN_DIAGNOSTIC_STATS_REQUIRES=",
        ):
            self.assertIn(token, PMU)
        self.assertIn('[[ -z "$hle_clean_mode" ]] && return 0', PMU)
        cleanup = shell_function(PMU, "cleanup_profile_markers")
        self.assertIn("'$hle_clean_stats_marker'", cleanup)
        self.assertIn("test ! -e '$hle_clean_stats_marker'", cleanup)
        self.assertIn("HLE_CLEAN_STATS_MARKER_CLEANUP_FAILED", cleanup)
        setter = shell_function(PMU, "set_hle_clean_stats_marker_state")
        self.assertIn('if [[ "$hle_clean_stats_mode" == 1 ]]', setter)
        self.assertIn("touch '$hle_clean_stats_marker'", setter)
        self.assertIn("test -f '$hle_clean_stats_marker'", setter)
        self.assertIn("test ! -e '$hle_clean_stats_marker'", setter)
        pmu_main = PMU.rsplit("verify_canonical_fex_host || exit 51", 1)[1]
        self.assertLess(
            pmu_main.index("set_hle_clean_stats_marker_state"),
            pmu_main.index("am start -n app.lsx4.android/.MainActivity"),
        )

    def test_fresh_exact_aba_sequence_has_one_axis_and_fresh_production_handoff(self):
        ordered = [
            "run_phase baseline-a1 0 0",
            "run_phase clean-b 1 0",
            "run_phase baseline-a2 0 1",
        ]
        positions = [HARNESS.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        for token in (
            "CUSA03173/eboot.bin",
            "reason=process-identity-not-fresh",
            "reason=aba-process-identities-not-unique",
            "baseline_a1_pid",
            "clean_b_pid",
            "baseline_a2_pid",
            "sequence=baseline-a1:0,clean-b:1,baseline-a2:0",
            "BB_HLE_CLEAN_BUFFER_FASTPATH=\"$mode\"",
            "BB_HLE_CLEAN_DIAGNOSTIC_STATS=1",
            "BB_DISABLE_ASYNC_SHADER_RETRANSLATION=1",
            "run-diagnostic-hle-clean-buffer-stats",
            "run-disable-async-shader-retranslation",
            "experimentalAxis\": False",
            "run_production_handoff",
            "reason=handoff-process-identity-not-fresh",
            "BB_HLE_CLEAN_BUFFER_FASTPATH=0",
            "BB_HLE_CLEAN_DIAGNOSTIC_STATS=0",
            "BB_DISABLE_ASYNC_SHADER_RETRANSLATION=0",
            "asyncShaderRetranslation=default-on",
        ):
            self.assertIn(token, HARNESS)

        causal = HARNESS.split("run_phase()", 1)[1].split(
            "run_production_handoff()", 1
        )[0]
        production = HARNESS.split("run_production_handoff()", 1)[1].split(
            "scene_metric()", 1
        )[0]
        self.assertEqual(causal.count("BB_DISABLE_ASYNC_SHADER_RETRANSLATION=1"), 1)
        self.assertEqual(causal.count("BB_HLE_CLEAN_DIAGNOSTIC_STATS=1"), 1)
        self.assertNotIn('BB_DISABLE_ASYNC_SHADER_RETRANSLATION="$mode"', causal)
        self.assertIn(
            "[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=1 "
            "fastTierAllowed=0 hleGuardRequested=$mode hleGuardEnabled=$mode",
            causal,
        )
        self.assertIn("BB_DISABLE_ASYNC_SHADER_RETRANSLATION=0", production)
        self.assertIn("BB_HLE_CLEAN_DIAGNOSTIC_STATS=0", production)
        self.assertIn(
            "[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=0 "
            "fastTierAllowed=1 hleGuardRequested=0 hleGuardEnabled=0",
            production,
        )
        self.assertLess(
            HARNESS.index("evaluate_mechanism_evidence"),
            HARNESS.rindex("run_production_handoff"),
        )

    def test_route_camera_warmup_window_and_work_are_comparable(self):
        for token in (
            "route=iosefka-door-forward",
            "camera=unchanged",
            "BB_HLE_CLEAN_AB_WARMUP_WAIT",
            "BB_PRE_MEASURE_WARMUP",
            "BB_MEASUREMENT_INPUT=forward",
            "CROSS_OPEN_DOOR",
            "WARMUP_DONE",
            "write_builder_scene_metrics",
            "BB_MIN_BUILDER_UNITS_PER_FRAME",
            "compare_scene_metric builderUnitsPerFrame",
            "compare_scene_metric builderDrawPerFrame",
            "compare_scene_metric builderShaderPerFrame",
        ):
            self.assertIn(token, PMU + HARNESS)

    def test_all_six_are_off_and_platform_policy_is_exact(self):
        for marker in (
            "run-disable-dynamic-shadows",
            "run-disable-ssao",
            "run-disable-motion-blur",
            "run-disable-dof",
            "run-disable-anti-aliasing",
            "run-disable-chromatic-aberration",
        ):
            self.assertIn(marker, PMU)
            self.assertIn(marker, HARNESS)
        for extra in (
            "profile_disable_dynamic_shadows true",
            "profile_disable_ssao true",
            "profile_disable_motion_blur true",
            "profile_disable_depth_of_field true",
            "profile_disable_anti_aliasing true",
            "profile_disable_chromatic_aberration true",
            "ALL_SIX=off",
            "POST_LAUNCH_GRAPHICS_BASELINE",
            "GRAPHICS_BASELINE_OK",
            "aggressive40Hz=1.*targetNs=25000000.*sessionActive=1.*updateResult=0",
            "vivo mode=performance-hint-4241",
            "rootBoost .*applied=true",
        ):
            self.assertIn(extra, PMU + HARNESS)

    def test_zero_valued_scene_channel_is_accepted_only_when_all_phases_are_zero(self):
        self.assertIn(
            "if (mean == 0) exit !(a == 0 && b == 0 && c == 0)",
            HARNESS,
        )

    def test_forbidden_fex_and_profiler_markers_are_absent(self):
        for marker in (
            "run-enable-fex-buffer-capture-ack",
            "run-enable-fex-deferred-write-epoch",
            "run-enable-bounded-guest-invalidation-drain",
            "run-force-fex-software-tso",
            "run-enable-fex-multiblock",
            "run-enable-fex-relaxed-vector-tso-ab",
            "run-enable-fex-relaxed-memcpyset-tso-ab",
            "run-enable-fex-low-memory-cache",
            "run-profile-guest-invalidation-sources",
            "run-profile-guest-invalidation-overlap",
            "run-disable-render-effects-bundle",
            "run-force-internal-resolution-640x360",
            "run-force-interpret-all",
            "run-enable-legacy-patches",
        ):
            self.assertIn(marker, PMU)
            self.assertIn(marker, HARNESS)
        self.assertIn("STRICT_CONFOUNDERS=none", PMU)
        self.assertIn("assert_experiment_markers_absent", HARNESS)

    def test_canonical_fex_hashes_and_status_are_fail_closed(self):
        for role, library in (
            ("executor", "liblsx4_executor_android.so"),
            ("adapter", "liblsx4_fex_backend_v3.so"),
            ("bridge", "liblsx4_fex2608_context_bridge.so"),
            ("fexcore", "libFEXCore.so"),
        ):
            self.assertIn(role, PMU)
            self.assertIn(library, PMU)
        for token in (
            "verify_android_fex_artifacts.py",
            "CANONICAL_FEX_RUNTIME_HASH_MISMATCH",
            'status.get("effective") != "fex-experimental"',
            'status["configureFlags"] != 0',
            "strong-memory capability bit2 absent",
            "A/B capability bit12 absent",
            'status.get("faultSignalOwner") != "unselected"',
            'status.get("selectionLocked") is not True',
            "[EXECUTOR_FEX_SIGNAL_MODE] owner=split shadps4=SIGSEGV,SIGBUS fex=SIGILL jit=0",
            "[EXECUTOR_FEX_bridge] full FEX runtime: LSX4 signal owner, guest delivery, and thread wake active",
            "CANONICAL_FEX_STATUS_OK configureFlags=0 faultSignalOwner=unselected signalMode=split selectionLocked=true",
            "-lookup-throughput-sm-swtso",
            "ABA_PROVENANCE_OK",
        ):
            self.assertIn(token, PMU + HARNESS)

    def test_log_and_counter_contract_is_active_only_in_b(self):
        for token in (
            "[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH]",
            "source=sentinel",
            "enabled=1",
            "scope=memset-only",
            "maxPages=16",
            '"attempts", "eligible", "committed", "avoidedSync"',
            "eligible_delta != avoided_sync_delta",
            "first_active != 0 or end_active != 0",
            "aborted_or_unaccounted != 0",
            "boundary_committed = committed_delta - first_active + end_active",
            '"fallbackShader"',
            "end_active > last[\"activeMax\"]",
            "HLE_CLEAN_COUNTERS_OK",
            "HLE_CLEAN_WINDOW_COVERAGE",
            "GUEST_INVALIDATION_WINDOW_OK",
            "BB_MIN_HLE_SYNC_COVERAGE",
            "HLE_CLEAN_LOG_BASELINE_OK enabled=0 counters=absent",
        ):
            self.assertIn(token, PMU)

        counter_validator = next(
            block
            for block in inline_python(PMU)
            if "HLE clean measurement snapshots insufficient" in block
        )
        first = (
            "attempts=12 eligible=9 committed=8 avoidedSync=9 fallbackSize=1 "
            "fallbackAlias=1 fallbackMapping=0 fallbackPage=0 fallbackTexture=0 "
            "fallbackShader=0 fallbackRegionMissing=0 fallbackRegionBusy=0 "
            "fallbackRegionGpuDirty=1 fallbackPostvalidate=0 fallbackNested=0 "
            "pages=16 bytes=65536 active=0 activeMax=2"
        )
        second = (
            "attempts=112 eligible=69 committed=68 avoidedSync=69 fallbackSize=21 "
            "fallbackAlias=11 fallbackMapping=0 fallbackPage=0 fallbackTexture=0 "
            "fallbackShader=0 fallbackRegionMissing=0 fallbackRegionBusy=0 "
            "fallbackRegionGpuDirty=11 fallbackPostvalidate=0 fallbackNested=0 "
            "pages=132 bytes=540672 active=0 activeMax=3"
        )
        log_text = "\n".join(
            (
                "[EXECUTOR_GUEST_INVALIDATION] completed=100 requests=100 batches=100 "
                "retries=0 nested=0 rounds=100 catchup=0 queueMax=1 "
                "waitTotalMs=10.000 waitMaxMs=2.000",
                f"[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH] {first}",
                "[EXECUTOR_GUEST_INVALIDATION] completed=110 requests=110 batches=110 "
                "retries=0 nested=0 rounds=110 catchup=0 queueMax=1 "
                "waitTotalMs=15.000 waitMaxMs=3.000",
                f"[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH] {second}",
            )
        ) + "\n"
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "log.txt"
            metrics = Path(temporary) / "metrics.json"
            log.write_text(log_text, encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    "-",
                    str(log),
                    "300",
                    "1",
                    "1",
                    str(metrics),
                    "0.50",
                ],
                input=counter_validator,
                text=True,
                capture_output=True,
                check=False,
            )
            parsed = json.loads(metrics.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("HLE_CLEAN_COUNTERS_OK", result.stdout)
        self.assertIn("HLE_CLEAN_WINDOW_COVERAGE", result.stdout)
        self.assertEqual(parsed["hleClean"]["firstActive"], 0)
        self.assertEqual(parsed["hleClean"]["endActive"], 0)
        self.assertEqual(parsed["hleClean"]["eligibleDelta"], 60)
        self.assertEqual(parsed["hleClean"]["avoidedSyncDelta"], 60)
        self.assertEqual(parsed["hleClean"]["committedDelta"], 60)
        self.assertEqual(parsed["hleClean"]["abortedOrUnaccounted"], 0)
        self.assertEqual(parsed["hleClean"]["boundaryCommitted"], 60)
        self.assertAlmostEqual(parsed["hleClean"]["committedSyncShare"], 60 / 70)
        self.assertFalse(parsed["fpsVerdictAllowed"])

    def test_pmu_sf_workload_image_and_failure_gates_are_mandatory(self):
        for token in (
            "simpleperf stat -p",
            "instructions:u,cpu-cycles:u,task-clock,context-switches,cpu-migrations,page-faults",
            "Total test time:",
            "SurfaceFlinger --timestats -clear",
            "statsStart = ",
            "statsEnd = ",
            "STRICT_ZERO_FPS_FREEZE",
            "STRICT_TAIL_FRAME_RATE_LOW",
            "STRICT_PRESENT_GAP_TOO_HIGH",
            "BB_MAX_PRESENT_GAP_MS",
            "aggressive40Hz=1.*targetNs=25000000.*sessionActive=1.*updateResult=0",
            "STOP_GAME_VERIFIED pids=absent",
            "STOP_GAME_PID_STILL_RUNNING",
            "STRICT_SF_DROPPED_FRAMES",
            "FATAL EXCEPTION",
            "am_anr",
            "VK_ERROR_DEVICE_LOST",
            "gameState loading=false gameplay=true",
            "serial=CUSA03173",
            "target=960x544",
            "blank/corrupt graphics gate",
            '.codex-bb-$tag-start.png',
            '.codex-bb-$tag-start-graphics.json',
            "white/missing-geometry gate failed",
            "GRAPHICS_ABA_OK",
            "STRICT_PHASE_OK",
        ):
            self.assertIn(token, PMU + HARNESS)

    def test_counter_validator_rejects_uncommitted_and_active_boundaries(self):
        validator = next(
            block
            for block in inline_python(PMU)
            if "HLE clean measurement snapshots insufficient" in block
        )
        guest_first = (
            "[EXECUTOR_GUEST_INVALIDATION] requests=100 waitTotalMs=10.000 waitMaxMs=2.000"
        )
        guest_last = (
            "[EXECUTOR_GUEST_INVALIDATION] requests=110 waitTotalMs=15.000 waitMaxMs=3.000"
        )

        def hle_line(*, committed, active):
            return (
                "[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH] "
                f"attempts={12 if committed == 8 else 112} "
                f"eligible={9 if committed == 8 else 69} committed={committed} "
                f"avoidedSync={9 if committed == 8 else 69} "
                f"fallbackSize={1 if committed == 8 else 21} "
                f"fallbackAlias={1 if committed == 8 else 11} "
                "fallbackMapping=0 fallbackPage=0 fallbackTexture=0 fallbackShader=0 "
                "fallbackRegionMissing=0 fallbackRegionBusy=0 "
                f"fallbackRegionGpuDirty={1 if committed == 8 else 11} "
                "fallbackPostvalidate=0 fallbackNested=0 "
                f"pages={16 if committed == 8 else 132} "
                f"bytes={65536 if committed == 8 else 540672} "
                f"active={active} activeMax=3"
            )

        cases = (
            ("uncommitted", hle_line(committed=8, active=0), hle_line(committed=66, active=0),
             "aborted/unaccounted"),
            ("first-active", hle_line(committed=8, active=1), hle_line(committed=68, active=0),
             "firstActive=1 endActive=0"),
            ("end-active", hle_line(committed=8, active=0), hle_line(committed=68, active=1),
             "firstActive=0 endActive=1"),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, first, last, expected in cases:
                log = root / f"{name}.txt"
                metrics = root / f"{name}.json"
                log.write_text(
                    f"{guest_first}\n{first}\n{guest_last}\n{last}\n",
                    encoding="utf-8",
                )
                result = subprocess.run(
                    [
                        sys.executable,
                        "-",
                        str(log),
                        "300",
                        "1",
                        "1",
                        str(metrics),
                        "0.50",
                    ],
                    input=validator,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0, name)
                self.assertIn(expected, result.stderr, name)

    def test_stats_off_hle_window_has_no_counter_or_diagnostic_fps_tax(self):
        validator = next(
            block
            for block in inline_python(PMU)
            if "HLE clean measurement snapshots insufficient" in block
        )
        log_text = (
            "[EXECUTOR_GUEST_INVALIDATION] requests=100 waitTotalMs=10.000 waitMaxMs=2.000\n"
            "[EXECUTOR_GUEST_INVALIDATION] requests=110 waitTotalMs=15.000 waitMaxMs=3.000\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "performance.txt"
            metrics = Path(temporary) / "performance.json"
            log.write_text(log_text, encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    "-",
                    str(log),
                    "300",
                    "1",
                    "0",
                    str(metrics),
                    "0.50",
                ],
                input=validator,
                text=True,
                capture_output=True,
                check=False,
            )
            parsed = json.loads(metrics.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("HLE_CLEAN_LOG_PERFORMANCE_OK", result.stdout)
        self.assertFalse(parsed["diagnosticOnly"])
        self.assertTrue(parsed["fpsVerdictAllowed"])
        self.assertFalse(parsed["hleClean"]["diagnosticStats"])
        self.assertEqual(parsed["hleClean"]["snapshots"], 0)

    def test_window_counter_validator_accepts_baseline_and_rejects_low_coverage(self):
        validator = next(
            block
            for block in inline_python(PMU)
            if "HLE clean measurement snapshots insufficient" in block
        )
        guest_first = (
            "[EXECUTOR_GUEST_INVALIDATION] requests=100 waitTotalMs=10.000 waitMaxMs=2.000"
        )
        guest_last = (
            "[EXECUTOR_GUEST_INVALIDATION] requests=1100 waitTotalMs=15.000 waitMaxMs=3.000"
        )
        hle_first = (
            "[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH] attempts=12 eligible=9 committed=8 "
            "avoidedSync=9 fallbackSize=1 fallbackAlias=1 fallbackMapping=0 fallbackPage=0 "
            "fallbackTexture=0 fallbackShader=0 fallbackRegionMissing=0 fallbackRegionBusy=0 "
            "fallbackRegionGpuDirty=1 fallbackPostvalidate=0 fallbackNested=0 pages=16 "
            "bytes=65536 active=0 activeMax=2"
        )
        hle_last = (
            "[EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH] attempts=112 eligible=69 committed=68 "
            "avoidedSync=69 fallbackSize=21 fallbackAlias=11 fallbackMapping=0 fallbackPage=0 "
            "fallbackTexture=0 fallbackShader=0 fallbackRegionMissing=0 fallbackRegionBusy=0 "
            "fallbackRegionGpuDirty=11 fallbackPostvalidate=0 fallbackNested=0 pages=132 "
            "bytes=540672 active=0 activeMax=3"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline_log = root / "baseline.txt"
            baseline_json = root / "baseline.json"
            baseline_log.write_text(f"{guest_first}\n{guest_last}\n", encoding="utf-8")
            baseline = subprocess.run(
                [
                    sys.executable,
                    "-",
                    str(baseline_log),
                    "300",
                    "0",
                    "1",
                    str(baseline_json),
                    "0.50",
                ],
                input=validator,
                text=True,
                capture_output=True,
                check=False,
            )
            low_log = root / "low.txt"
            low_json = root / "low.json"
            low_log.write_text(
                f"{guest_first}\n{hle_first}\n{guest_last}\n{hle_last}\n",
                encoding="utf-8",
            )
            low = subprocess.run(
                [
                    sys.executable,
                    "-",
                    str(low_log),
                    "300",
                    "1",
                    "1",
                    str(low_json),
                    "0.50",
                ],
                input=validator,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        self.assertIn("HLE_CLEAN_LOG_BASELINE_OK", baseline.stdout)
        self.assertNotEqual(low.returncode, 0)
        self.assertIn("committed coverage below threshold", low.stderr)

    def test_cleanup_always_removes_marker_and_keeps_exact_baseline(self):
        cleanup = HARNESS.split("restore_baseline_running_best_effort()", 1)[1].split(
            "on_exit()", 1
        )[0]
        for token in (
            "remove_experiment_markers",
            "assert_experiment_markers_absent",
            "assert_graphics_baseline",
            'if [[ "$active_mode" == production && -n "$current_pid" ]]',
            "launch_exact_baseline",
            "CLEANUP_BASELINE_RUNNING",
            "CLEANUP_KEEP_RUNNING_PRODUCTION_BASELINE",
            "run-enable-hle-clean-buffer-fastpath",
            "run-diagnostic-hle-clean-buffer-stats",
        ):
            self.assertIn(token, cleanup + HARNESS)
        self.assertIn("finalMode=production-baseline finalGameRunning=true", HARNESS)
        self.assertIn("PRODUCTION_HANDOFF_OK", HARNESS)
        self.assertIn("mechanismEvidence=REJECTED optimization=NOT_EVALUATED", HARNESS)
        rejected = HARNESS.rindex("mechanismEvidence=REJECTED")
        self.assertLess(HARNESS.rindex("run_production_handoff"), rejected)
        self.assertIn("device_session_started == 1", HARNESS)
        self.assertIn("KEEP_RUNNING_BASELINE", PMU)
        self.assertIn("HLE_CLEAN_MARKER_CLEANUP_FAILED", PMU)
        self.assertIn("HLE_CLEAN_STATS_MARKER_CLEANUP_FAILED", PMU)
        self.assertNotIn('if [[ "$active_mode" == baseline', cleanup)
        production = HARNESS.split("run_production_handoff()", 1)[1].split(
            "scene_metric()", 1
        )[0]
        self.assertIn("active_mode=production", production)

    def test_run_namespace_window_ledger_and_hash_index_are_fail_closed(self):
        for token in (
            ".codex-bb-runs/$run_id",
            "windows.jsonl",
            "save-restores.jsonl",
            "manifest.json",
            "RUN_NAMESPACE_CREATED",
            "started",
            "accepted",
            "rejected",
            'os.O_WRONLY | os.O_APPEND',
            "short append to windows.jsonl",
            "measurementArtifactSha256",
            "processIdentity",
            "/proc/<pid>/stat field 22",
            "SHA256SUMS",
            "RUN_NAMESPACE_SEALED",
        ):
            self.assertIn(token, HARNESS)
        self.assertIn("os.O_CREAT | os.O_EXCL | os.O_WRONLY", HARNESS)
        self.assertIn('with result.open("x"', HARNESS)
        self.assertIn('with index_path.open("x"', HARNESS)

    def test_save_seed_is_quiescent_verified_atomic_and_never_broadly_deleted(self):
        for token in (
            "1000/CUSA00207",
            "SPRJ0005",
            "SAVE_SEED_PROCESS_QUIESCENT",
            "save-seed-symlink-rejected",
            "save-seed-special-file-rejected",
            "save-seed-runtime-contract-unavailable",
            "du -sk",
            "df -k",
            "save-seed-free-space-insufficient",
            "save_space_multiplier=8",
            "save_space_margin_kib=65536",
            "original.tar",
            "device-original-tar.sha256",
            "save-seed-host-anchored-archive-hash-mismatch",
            "staging-extract-failed",
            "staged-inventory-mismatch",
            "live-inventory-mismatch",
            "atomic-swap-failed",
            "displaced-$save_restore_sequence-$phase",
            "SAVE_SEED_RESTORED",
        ):
            self.assertIn(token, HARNESS)
        self.assertNotRegex(HARNESS, r"\brm\s+-rf\b")
        self.assertIn("mv '$save_path' '$displaced'", HARNESS)
        self.assertIn("mv '$stage/$save_leaf' '$save_path'", HARNESS)
        self.assertIn("mv '$displaced' '$save_path'", HARNESS)

    def test_diagnostic_decision_never_uses_fps_as_a_verdict(self):
        for token in (
            "minimum_sync_coverage='0.50'",
            "BB_HLE_MIN_WAIT_REDUCTION_PCT",
            "baseline_wait = (wait[0] + wait[2]) / 2.0",
            'baseline_fps = (surface[0]["fps"] + surface[2]["fps"]) / 2.0',
            "wait-reduction-below-threshold",
            "coverage-below-floor",
            "VALID_ABA",
            "MECHANISM_EVIDENCE_ACCEPTED",
            "MECHANISM_EVIDENCE_REJECTED",
            '"diagnosticOnly": True',
            '"fpsVerdictAllowed": False',
            '"mechanismEvidenceAccepted": accepted',
            '"optimizationAccepted": False',
            '"surfaceFpsObservationOnly": True',
            "surfaceFpsObservationPct=",
            '"productionEligible": False',
            '"productionTransferProven": False',
            '"productionValidationRequired": True',
            '"measuredConfiguration": "async-shader-retranslation-disabled"',
            '"productionConfiguration": "async-shader-retranslation-default-on"',
            "productionEligible=false productionTransfer=unproven",
            "diagnostic-mechanism-thresholds-not-met",
            "run_production_handoff",
        ):
            self.assertIn(token, HARNESS)
        for forbidden in (
            "BB_HLE_MIN_FPS_IMPROVEMENT_PCT",
            "minimumSurfaceFpsImprovementPct",
            "sf-fps-improvement-below-threshold",
            "OPTIMIZATION_ACCEPTED",
        ):
            self.assertNotIn(forbidden, HARNESS)
        main = HARNESS.split("prepare_save_seed", 1)[-1]
        self.assertLess(main.rindex("evaluate_mechanism_evidence"), main.rindex("run_production_handoff"))
        self.assertLess(main.rindex("run_production_handoff"), main.rindex("mechanismEvidence=REJECTED"))

    def test_cleanup_mode_is_published_only_after_phase_proof(self):
        run_phase = HARNESS.split("run_phase()", 1)[1].split(
            "scene_metric()", 1
        )[0]
        mode_publish = (
            'active_mode="$([[ "$mode" == 0 ]] && echo baseline || echo clean)"'
        )
        self.assertEqual(run_phase.count(mode_publish), 1)
        self.assertGreater(
            run_phase.index(mode_publish),
            run_phase.index("HLE_CLEAN_LOG_BASELINE_OK enabled=0 counters=absent"),
        )
        self.assertGreater(
            run_phase.index(mode_publish),
            run_phase.index("fastpath-counters-not-proven"),
        )


if __name__ == "__main__":
    unittest.main()
