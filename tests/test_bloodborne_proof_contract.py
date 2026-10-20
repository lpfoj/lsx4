import copy
import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "bloodborne-proof" / "proof_contract.py"
SPEC = importlib.util.spec_from_file_location("bloodborne_proof_contract", MODULE_PATH)
proof = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(proof)


HASH = "1" * 64


def pmu_csv(**overrides):
    values = {
        "instructions:u": 1_000_000,
        "cpu-cycles:u": 2_000_000,
        "raw-l1i-cache-refill:u": 3_000,
        "raw-br-mis-pred-retired:u": 4_000,
        "raw-dmb-spec:u": 5_000,
        "page-faults:u": 60,
    }
    values.update(overrides)
    rows = ["Performance counter statistics,"]
    rows.extend(f"{value},{name},1.0,/sec," for name, value in values.items())
    rows.append("Total test time,10.000000,seconds,")
    return "\n".join(rows)


def fex_snapshot(sequence, *, scale=1, mode="dynamic"):
    unavailable = sum(
        proof.FEX_PROOF_METRIC_BITS[name]
        for name in proof.FEX_PROOF_UNAVAILABLE_TOTALS
    )
    if mode != "dynamic":
        unavailable |= proof.FEX_PROOF_METRIC_BITS["executedIrBlockEntries"]
    valid = proof.FEX_PROOF_REQUESTED_MASK & ~unavailable
    dynamic = (
        proof.FEX_PROOF_METRIC_BITS["executedIrBlockEntries"]
        if mode == "dynamic"
        else 0
    )
    if mode == "off":
        valid = 0
        unavailable = proof.FEX_PROOF_REQUESTED_MASK
        dynamic = 0
    static = valid & ~dynamic
    perturbed = dynamic
    values = {
        name: ((index + 1) * 100 * scale if valid & bit else 0)
        for index, (name, bit) in enumerate(proof.FEX_PROOF_METRIC_BITS.items())
    }
    return {
        "schema": "lsx4-fex-proof-metrics/v1",
        "source": "fex-runtime-diagnostic-v1",
        "effectiveBackend": "fex-experimental",
        "queryStatus": 0,
        "status": 0,
        "structSize": 256,
        "abiVersion": 1,
        "instrumentationState": mode,
        "pid": 42,
        "bootBeforeNs": 800_000_000 + sequence,
        "bootAfterNs": 800_000_100 + sequence,
        "captureFlags": 3 if mode != "off" else 0,
        "requestedMask": proof.FEX_PROOF_REQUESTED_MASK,
        "validMask": valid,
        "unavailableMask": unavailable,
        "exactMask": valid,
        "sampledMask": 0,
        "staticMask": static,
        "dynamicMask": dynamic,
        "perturbedMask": perturbed,
        "attemptedNotRetiredMask": 0,
        "epoch": 7,
        "sequence": sequence,
        "values": values,
    }


def runtime_counters(sequence, *, scale=1, mode="off", fault_valid=True):
    valid = 0
    if mode == "dynamic-diagnostic":
        valid = proof.RUNTIME_PROOF_REQUIRED_DYNAMIC_MASK
        if fault_valid:
            valid |= proof.RUNTIME_PROOF_METRIC_BITS[
                "ownedGeneratedCodeWriteFaults"
            ]
    unavailable = proof.RUNTIME_PROOF_REQUESTED_MASK & ~valid
    values = {
        name: ((index + 1) * 100 * scale if valid & bit else 0)
        for index, (name, bit) in enumerate(
            proof.RUNTIME_PROOF_METRIC_BITS.items()
        )
    }
    capture_flags = 0
    if mode == "dynamic-diagnostic":
        capture_flags = 0x3 | (0x4 if fault_valid else 0)
    return {
        "schema": "lsx4-runtime-proof-counters/v1",
        "source": "funnel-runtime-diagnostic-v1",
        "effectiveBackend": "fex-experimental",
        "queryStatus": 0,
        "status": 0,
        "structSize": 256,
        "abiVersion": 1,
        "instrumentationState": mode,
        "pid": 42,
        "bootBeforeNs": 700_000_000 + sequence,
        "bootAfterNs": 700_000_100 + sequence,
        "captureFlags": capture_flags,
        "saturated": 0,
        "requestedMask": proof.RUNTIME_PROOF_REQUESTED_MASK,
        "validMask": valid,
        "unavailableMask": unavailable,
        "exactMask": valid,
        "sampledMask": 0,
        "staticMask": 0,
        "dynamicMask": valid,
        "perturbedMask": valid,
        "attemptedNotRetiredMask": (
            proof.RUNTIME_PROOF_ATTEMPTED_MASK & valid
        ),
        "epoch": 3 if mode == "dynamic-diagnostic" else 0,
        "sequence": sequence if mode == "dynamic-diagnostic" else 0,
        "values": values,
    }


def combined_snapshot(token, sequence, boot_before, boot_after, *, scale=1,
                      fex_mode="off", runtime_mode="off"):
    fex = fex_snapshot(sequence, scale=scale, mode=fex_mode)
    runtime = runtime_counters(sequence, scale=scale, mode=runtime_mode)
    fex["bootBeforeNs"] = boot_before + 1
    fex["bootAfterNs"] = boot_before + 2
    runtime["bootBeforeNs"] = boot_before + 3
    runtime["bootAfterNs"] = boot_before + 4
    return {
        "schema": proof.PROOF_SNAPSHOT_SCHEMA,
        "requestToken": token,
        "pid": 42,
        "processStarttime": 9001,
        "bootBeforeNs": boot_before,
        "bootAfterNs": boot_after,
        "fex": fex,
        "runtime": runtime,
        "hud": {
            "schema": proof.HUD_COUNTER_SCHEMA,
            "source": "runtimeHudStats-exact-cumulative",
            "pid": 42,
            "bootBeforeNs": boot_before + 5,
            "bootAfterNs": boot_before + 6,
            "valueCount": 11,
            "indices": {
                "generatedBlocks": 0,
                "drawCalls": 6,
                "submitCalls": 8,
                "presentCalls": 10,
            },
            "counters": {
                "generatedBlocks": 100 * scale,
                "drawCalls": 700 * scale,
                "submitCalls": 900 * scale,
                "presentCalls": 1100 * scale,
            },
            "rawValues": [
                100 * scale, 0, 0, 0, 0, 0, 700 * scale, 0,
                900 * scale, 0, 1100 * scale,
            ],
        },
    }


def provenance():
    return {
        "device": {"serial": "10AG263F1B001HM", "soc": "SM8850", "board": "canoe"},
        "build": {
            "gitCommit": "9f093f4be",
            "apkSha256": HASH,
            "apkVersionCode": 1,
            "signerSha256": HASH,
        },
        "libraries": {
            "executorSha256": HASH,
            "fexAdapterSha256": HASH,
            "fexBridgeSha256": HASH,
            "fexCoreSha256": HASH,
        },
        "backend": {"requested": "FEX", "effective": "FEX", "fallbackReason": "none"},
        "vulkan": {
            "requested": "turnip",
            "effective": "turnip",
            "vendorId": "0x5143",
            "deviceId": "0x0001",
            "driverVersion": "26.2.1",
            "driverBuild": "mesa-26.2.1",
            "icdSha256": HASH,
            "pipelineCacheUuid": "00112233445566778899aabbccddeeff",
        },
        "settings": {"sha256": HASH},
        "patches": {"sha256": HASH},
        "route": {"sha256": HASH},
        "input": {"sha256": HASH},
        "save": {"sha256": HASH},
        "cache": {
            "seedSha256": HASH,
            "pipelineInventorySha256": HASH,
            "fexInventorySha256": HASH,
        },
        "display": {"mode": "3168x1440", "refreshHz": 60},
        "pmuCapabilities": {
            "sha256": HASH,
            "l1iValidated": True,
            "branchValidated": True,
            "dmbPmuValidated": False,
            "dmbMetricSource": "unavailable",
        },
        "markers": {
            "run-enable-fex-relaxed-memcpyset-tso-ab": False,
            "run-enable-fex-relaxed-vector-tso-ab": False,
            "run-enable-hle-clean-buffer-fastpath": False,
            "run-enable-fex-multiblock": False,
        },
    }


def latency_text(count=301, interval_ns=33_333_333, start=1_000_000_000):
    rows = ["16666666"]
    for index in range(count):
        timestamp = start + index * interval_ns
        rows.append(f"{timestamp} {timestamp} {timestamp}")
    return "\n".join(rows)


def bundle():
    start = combined_snapshot(
        "window-1-start", 10, 950_000_000, 960_000_000, scale=1
    )
    end = combined_snapshot(
        "window-1-end", 11, 10_800_000_000, 10_810_000_000, scale=2
    )
    return {
        "window": {
            "schema": proof.SCHEMA,
            "runId": "run-1",
            "windowId": "window-1",
            "phase": "baseline",
            "variant": "A",
            "pid": 42,
            "processStarttime": 9001,
            "surfaceGeneration": 7,
            "boundary": {
                "deviceBootStartNs": 900_000_000,
                "deviceBootEndNs": 10_900_000_000,
                "hostMonotonicStartNs": 100_000_000,
                "hostMonotonicEndNs": 10_100_000_000,
            },
        },
        "provenance": provenance(),
        "surface": {
            "layerName": "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST)",
            "layerId": 123,
            "generation": 7,
            "uid": 10358,
            "displayId": 0,
        },
        "health": {
            "fatalSignals": 0,
            "deviceLost": 0,
            "blackFrames": 0,
            "corruptFrames": 0,
            "freezeGapsOver100ms": 0,
            "audioContractFailures": 0,
            "foreground": True,
            "tailProgress": True,
        },
        "pmu": {"stdout": pmu_csv(), "stderr": ""},
        "proofStart": start,
        "proofEnd": end,
        "surfaceLatency": latency_text(),
    }


class SimpleperfContractTests(unittest.TestCase):
    def test_accepts_complete_csv(self):
        result = proof.parse_simpleperf_csv(pmu_csv(), "")
        self.assertEqual(result["events"]["cpu-cycles:u"], 2_000_000)

    def test_rejects_historic_child_command_error(self):
        with self.assertRaisesRegex(proof.ProofRejected, "simpleperf-stderr-error"):
            proof.parse_simpleperf_csv(
                pmu_csv(),
                "simpleperf E workload.cpp:36] Running child command in app uid isn't allowed",
            )

    def test_rejects_missing_event(self):
        text = pmu_csv().replace("60,page-faults:u,1.0,/sec,\n", "")
        with self.assertRaisesRegex(proof.ProofRejected, "simpleperf-missing-events"):
            proof.parse_simpleperf_csv(text, "")

    def test_rejects_not_counted(self):
        text = pmu_csv(**{"raw-l1i-cache-refill:u": "<not counted>"})
        with self.assertRaisesRegex(proof.ProofRejected, "simpleperf-event-unavailable"):
            proof.parse_simpleperf_csv(text, "")


class FexProofCounterContractTests(unittest.TestCase):
    def test_accepts_only_ir_block_dynamic_denominator(self):
        delta = proof.fex_proof_delta(
            fex_snapshot(1), fex_snapshot(2, scale=2)
        )
        self.assertGreater(delta["values"]["executedIrBlockEntries"], 0)
        self.assertIn("executedGuestInstructionEntries", delta["unavailableFields"])
        self.assertIn("executedIrBlockEntries", delta["exactFields"])
        self.assertIn("executedIrBlockEntries", delta["dynamicFields"])
        self.assertIn("executedIrBlockEntries", delta["perturbedFields"])

    def test_rejects_guest_instruction_proxy_as_valid(self):
        value = fex_snapshot(1)
        bit = proof.FEX_PROOF_METRIC_BITS["executedGuestInstructionEntries"]
        value["validMask"] |= bit
        value["unavailableMask"] &= ~bit
        value["exactMask"] |= bit
        value["dynamicMask"] |= bit
        with self.assertRaisesRegex(
            proof.ProofRejected, "fex-proof-unprovable-total-was-advertised"
        ):
            proof.parse_fex_proof_snapshot(value)

    def test_unavailable_zero_is_not_a_measurement(self):
        result = proof.parse_fex_proof_snapshot(fex_snapshot(1))
        self.assertIn("compiledDmbSites", result["unavailableFields"])
        self.assertNotIn("compiledDmbSites", result["validFields"])


class RuntimeProofCounterContractTests(unittest.TestCase):
    def test_accepts_diagnostic_subset_and_keeps_fastmem_unavailable(self):
        delta = proof.runtime_counter_delta(
            runtime_counters(1, scale=1, mode="dynamic-diagnostic"),
            runtime_counters(2, scale=2, mode="dynamic-diagnostic"),
        )
        self.assertGreater(delta["values"]["guestInvalidationRequests"], 0)
        self.assertIn("fastmemMisses", delta["unavailableFields"])

    def test_accepts_live_quiet_fault_counter_as_unavailable(self):
        result = proof.parse_runtime_counter_snapshot(
            runtime_counters(1, mode="dynamic-diagnostic", fault_valid=False)
        )
        self.assertIn("ownedGeneratedCodeWriteFaults", result["unavailableFields"])

    def test_rejects_fastmem_proxy_as_valid(self):
        value = runtime_counters(1, mode="dynamic-diagnostic")
        bit = proof.RUNTIME_PROOF_METRIC_BITS["fastmemMisses"]
        value["validMask"] |= bit
        value["unavailableMask"] &= ~bit
        value["exactMask"] |= bit
        value["dynamicMask"] |= bit
        value["perturbedMask"] |= bit
        with self.assertRaisesRegex(
            proof.ProofRejected, "runtime-counter-fastmem-falsely-advertised"
        ):
            proof.parse_runtime_counter_snapshot(value)

    def test_unavailable_nonzero_is_rejected(self):
        value = runtime_counters(1, mode="dynamic-diagnostic")
        value["values"]["fastmemMisses"] = 1
        with self.assertRaisesRegex(
            proof.ProofRejected, "runtime-counter-unavailable-nonzero"
        ):
            proof.parse_runtime_counter_snapshot(value)

    def test_combined_snapshot_binds_nested_pid_and_clock(self):
        value = combined_snapshot("token-1", 1, 1_000_000, 2_000_000)
        value["runtime"]["pid"] = 99
        with self.assertRaisesRegex(
            proof.ProofRejected, "proof-snapshot-nested-pid-mismatch"
        ):
            proof.parse_combined_proof_snapshot(value)

    def test_combined_delta_rejects_reused_token(self):
        start = combined_snapshot("same", 1, 1_000_000, 2_000_000)
        end = combined_snapshot("same", 2, 3_000_000, 4_000_000)
        with self.assertRaisesRegex(
            proof.ProofRejected, "proof-snapshot-token-reused"
        ):
            proof.combined_proof_delta(start, end)


class FrameContractTests(unittest.TestCase):
    def test_exact_latency_distribution(self):
        result = proof.parse_surfaceflinger_latency(latency_text())
        self.assertAlmostEqual(result["averageFps"], 30.0, places=3)
        self.assertEqual(result["over40ms"], 0)

    def test_rejects_non_monotonic_latency(self):
        text = "16666666\n1 1 1\n2 2 2\n2 2 2\n"
        with self.assertRaisesRegex(proof.ProofRejected, "sf-latency-nonmonotonic"):
            proof.parse_surfaceflinger_latency(text)

    def test_heavy_route_rejects_single_freeze(self):
        result = proof.parse_surfaceflinger_latency(latency_text())
        result["over100ms"] = 1
        with self.assertRaisesRegex(proof.ProofRejected, "heavy-route-freeze"):
            proof.validate_heavy_route_frames(result)


class BundleContractTests(unittest.TestCase):
    def test_accepts_complete_synchronized_bundle(self):
        result = proof.validate_window_bundle(bundle())
        metrics = result["metrics"]
        self.assertEqual(metrics["hostInstructions"], 1_000_000)
        self.assertEqual(metrics["hostCycles"], 2_000_000)
        self.assertAlmostEqual(metrics["hostInstructionsPerSecond"], 100_000.0)
        self.assertAlmostEqual(metrics["hostCyclesPerSecond"], 200_000.0)
        self.assertAlmostEqual(metrics["hostCyclesPerInstruction"], 2.0)
        self.assertAlmostEqual(metrics["hostInstructionsPerCycle"], 0.5)
        self.assertEqual(metrics["hostL1iMisses"], 3_000)
        self.assertEqual(metrics["hostBranchMisses"], 4_000)
        self.assertAlmostEqual(metrics["hostL1iMissesPerSecond"], 300.0)
        self.assertAlmostEqual(metrics["hostBranchMissesPerSecond"], 400.0)
        self.assertGreater(metrics["hostInstructionsPerFrame"], 0.0)
        self.assertGreater(metrics["hostCyclesPerFrame"], 0.0)
        self.assertFalse(result["fullGuestMetricPanelProven"])

    def test_static_fex_compile_shape_uses_only_exact_static_denominators(self):
        static_delta = proof.fex_proof_delta(
            fex_snapshot(1, mode="static"),
            fex_snapshot(2, scale=2, mode="static"),
        )
        metrics = proof.derive_metrics(
            proof.parse_simpleperf_csv(pmu_csv(), ""),
            proof.parse_surfaceflinger_latency(latency_text()),
            {"values": {}},
            static_delta,
        )
        self.assertAlmostEqual(
            metrics["staticNativeBytesPerCompiledGuestInstructionSite"], 1.5
        )
        self.assertAlmostEqual(
            metrics["staticSpillFillSitesPerCompiledGuestInstructionSite"], 4.25
        )
        self.assertAlmostEqual(
            metrics["staticAllocatedSpillSlotsPerCompiledGuestInstructionSite"],
            1.75,
        )
        self.assertAlmostEqual(
            metrics["staticAllocatedSpillSlotsPerTranslationUnit"], 0.5
        )
        self.assertAlmostEqual(
            metrics["staticFlagSitesPerCompiledGuestInstructionSite"], 2.75
        )
        self.assertAlmostEqual(
            metrics["staticExplicitHelperSitesPerCompiledGuestInstructionSite"],
            4.25,
        )
        self.assertAlmostEqual(
            metrics["staticExplicitFenceDmbSitesPerCompiledGuestInstructionSite"],
            4.5,
        )
        self.assertAlmostEqual(
            metrics["staticIrBlocksPerTranslationUnit"], 1500 / 1400
        )
        self.assertAlmostEqual(
            metrics["staticNativeBytesPerPublishedBlock"], 0.375
        )
        self.assertEqual(
            metrics["staticCompileShapeMetricSource"], "exact-static-fex-delta"
        )
        self.assertFalse(metrics["staticCompileShapeRatiosProductionEligible"])
        self.assertFalse(metrics["staticCompileShapeRatiosPerturbed"])
        self.assertIn("not-executed-spills", metrics["staticSpillSlotSemantics"])

    def test_zero_static_denominator_is_unavailable_not_fake_zero(self):
        static_delta = proof.fex_proof_delta(
            fex_snapshot(1, mode="static"),
            fex_snapshot(2, scale=2, mode="static"),
        )
        static_delta["values"]["compiledGuestInstructionSites"] = 0
        metrics = proof.derive_metrics(
            proof.parse_simpleperf_csv(pmu_csv(), ""),
            proof.parse_surfaceflinger_latency(latency_text()),
            {"values": {}},
            static_delta,
        )
        name = "staticNativeBytesPerCompiledGuestInstructionSite"
        self.assertNotIn(name, metrics)
        self.assertIn(name, metrics["unavailable"])

    def test_dynamic_ir_block_cost_is_explicitly_perturbed_nonproduction(self):
        dynamic_delta = proof.fex_proof_delta(
            fex_snapshot(1, mode="dynamic"),
            fex_snapshot(2, scale=2, mode="dynamic"),
        )
        metrics = proof.derive_metrics(
            proof.parse_simpleperf_csv(pmu_csv(), ""),
            proof.parse_surfaceflinger_latency(latency_text()),
            {"values": {}},
            dynamic_delta,
        )
        self.assertAlmostEqual(
            metrics["instrumentedHostInstructionsPerIrBlockEntry"], 5000.0
        )
        self.assertAlmostEqual(
            metrics["instrumentedCyclesPerIrBlockEntry"], 10000.0
        )
        self.assertFalse(metrics["instrumentedRatiosProductionEligible"])
        self.assertTrue(metrics["instrumentedRatiosPerturbed"])
        self.assertIn(
            "exact-dynamic-fex-ir-block-entries",
            metrics["instrumentedRatioMetricSource"],
        )

    def test_rejects_surface_generation_drift(self):
        value = bundle()
        value["surface"]["generation"] = 8
        with self.assertRaisesRegex(proof.ProofRejected, "surface-generation-window-mismatch"):
            proof.validate_window_bundle(value)

    def test_rejects_v1_overlap_as_causal(self):
        value = bundle()
        value["overlap"] = {"schema": "lsx4-invalidation-overlap/v1"}
        with self.assertRaisesRegex(proof.ProofRejected, "overlap-not-causal-v2"):
            proof.validate_window_bundle(value)

    def test_rejects_window_duration_mismatch(self):
        value = bundle()
        value["window"]["boundary"]["deviceBootEndNs"] += 2_000_000_000
        value["window"]["boundary"]["hostMonotonicEndNs"] += 2_000_000_000
        with self.assertRaisesRegex(proof.ProofRejected, "pmu-window-duration-mismatch"):
            proof.validate_window_bundle(value)

    def test_production_rejects_runtime_instrumentation(self):
        value = bundle()
        value["proofStart"] = combined_snapshot(
            "diag-start", 1, 950_000_000, 960_000_000,
            runtime_mode="dynamic-diagnostic",
        )
        value["proofEnd"] = combined_snapshot(
            "diag-end", 2, 10_800_000_000, 10_810_000_000,
            scale=2, runtime_mode="dynamic-diagnostic",
        )
        with self.assertRaisesRegex(
            proof.ProofRejected, "production-window-instrumented"
        ):
            proof.validate_window_bundle(value)

    def test_runtime_diagnostic_is_explicitly_nonproduction(self):
        value = bundle()
        value["claimClass"] = "diagnostic-runtime-proof"
        value["proofStart"] = combined_snapshot(
            "diag-start", 1, 950_000_000, 960_000_000,
            runtime_mode="dynamic-diagnostic",
        )
        value["proofEnd"] = combined_snapshot(
            "diag-end", 2, 10_800_000_000, 10_810_000_000,
            scale=2, runtime_mode="dynamic-diagnostic",
        )
        result = proof.validate_window_bundle(value)
        self.assertFalse(result["productionFrameTimeEligible"])
        self.assertIn("invalidationRequestsPerFrame", result["metrics"])


class CampaignContractTests(unittest.TestCase):
    def make_campaign(self):
        phases = [bundle(), bundle(), bundle()]
        for index, (phase, variant) in enumerate(zip(phases, ("A", "B", "A"))):
            phase["window"]["windowId"] = f"window-{index}"
            phase["window"]["variant"] = variant
            phase["provenance"]["markers"][
                "run-enable-fex-relaxed-memcpyset-tso-ab"
            ] = variant == "B"
        return {
            "schema": proof.CAMPAIGN_SCHEMA,
            "axis": "MemcpySetTSO",
            "order": "ABA",
            "phases": phases,
        }

    def test_accepts_single_axis_aba(self):
        result = proof.validate_axis_campaign(self.make_campaign())
        self.assertEqual(result["axis"], "MemcpySetTSO")

    def test_rejects_second_experiment_marker(self):
        value = self.make_campaign()
        value["phases"][1]["provenance"]["markers"][
            "run-enable-fex-relaxed-vector-tso-ab"
        ] = True
        with self.assertRaises(proof.ProofRejected):
            proof.validate_axis_campaign(value)

    def test_rejects_driver_drift(self):
        value = self.make_campaign()
        value["phases"][1]["provenance"]["vulkan"]["driverVersion"] = "different"
        with self.assertRaisesRegex(proof.ProofRejected, "provenance-mismatch"):
            proof.validate_axis_campaign(value)


if __name__ == "__main__":
    unittest.main()
