import copy
import copy
import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "bloodborne-proof" / "run_campaign.py"
ROUTE_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "routes" /
    "bath_messenger_one_cross.json"
)
MOVING_ROUTE_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "routes" /
    "hunters_dream_forward_swipe_14s.json"
)
MOVING_INPUT_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "inputs" /
    "hunters_dream_forward_swipe_14s.json"
)
STATIC_INPUT_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "inputs" / "static-no-input.json"
)
MOVING_DECLARATION_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "declarations" /
    "current_vivo_moving_declaration.json"
)
SPEC = importlib.util.spec_from_file_location("bloodborne_proof_campaign", MODULE_PATH)
campaign = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = campaign
SPEC.loader.exec_module(campaign)


ADB_TARGET = "adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp"


def pmu_capabilities():
    return {
        "schema": "lsx4-pmu-negative-control/v1",
        "accepted": True,
        "l1iValidated": True,
        "branchValidated": True,
        "dmbPmuValidated": False,
        "dmbMetricSource": "fex-exact-runtime-counter",
    }


def baseline_backend_status():
    return {
        "schema": "lsx4.guest-cpu-backend.v1",
        "platform": "ps4",
        "requested": "fex-experimental",
        "effective": "fex-experimental",
        "supported": True,
        "fallbackReason": "none",
        "adapterAbi": "lsx4-fex-backend-v3/v3",
        "mappedEntryAbi": "v5",
        "cacheNamespace": (
            "fex-2608-ps4-v3-test-lookup-throughput-sm-swtso"
        ),
        "configureFlags": 0,
        "providedCapabilities": 8191,
        "requiredCapabilities": 4095,
        "forbiddenCapabilities": 0,
        "strongMemoryModel": (
            "FEX software TSO (hardware TSO when enabled by the host OS)"
        ),
        "faultSignalOwner": "unselected",
        "hostIsaFeatures": 63,
        "hostOsFeatures": 1,
        "hostSveVectorBytes": 64,
        "selectionLocked": True,
    }


def expected_state():
    managed = sorted(campaign.MANAGED_MARKERS)
    backend = baseline_backend_status()
    return {
        "schema": "lsx4-bloodborne-expected-state/v1",
        "backend": {
            "status": backend,
            "statusSha256": campaign.canonical_sha256(backend),
            "required": {
                "requested": "fex-experimental",
                "effective": "fex-experimental",
                "supported": True,
                "fallbackReason": "none",
                "selectionLocked": True,
            },
        },
        "markers": {
            "allowlist": managed + ["run-disable-motion-blur"],
            "present": ["run-disable-motion-blur"],
            "presentSha256": campaign.canonical_sha256(["run-disable-motion-blur"]),
        },
    }


def proof_snapshot(mode="static", token="token-start", pid=123, sequence=1,
                   epoch=99, required=campaign.STATIC_REQUIRED_METRICS):
    boot_base = 1_000_000 + (sequence - 1) * 1_000
    requested = sum(campaign.PROOF_METRIC_BITS.values())
    valid = 0 if mode == "off" else sum(
        campaign.PROOF_METRIC_BITS[name] for name in required
    )
    values = {
        name: (7 if valid & bit else 0)
        for name, bit in campaign.PROOF_METRIC_BITS.items()
    }
    dynamic_bit = campaign.PROOF_METRIC_BITS["executedIrBlockEntries"]
    dynamic = dynamic_bit if mode == "dynamic" else 0
    fex = {
        "schema": "lsx4-fex-proof-metrics/v1",
        "source": "fex-runtime-diagnostic-v1",
        "effectiveBackend": "fex-experimental",
        "pid": pid,
        "instrumentationState": mode,
        "queryStatus": 0,
        "status": 0,
        "abiVersion": 1,
        "structSize": 256,
        "bootBeforeNs": boot_base + 10,
        "bootAfterNs": boot_base + 20,
        "captureFlags": 0 if mode == "off" else 2,
        "requestedMask": requested,
        "validMask": valid,
        "unavailableMask": requested & ~valid,
        "exactMask": valid,
        "sampledMask": 0,
        "staticMask": valid & ~dynamic,
        "dynamicMask": dynamic,
        "perturbedMask": dynamic,
        "attemptedNotRetiredMask": 0,
        "epoch": epoch,
        "sequence": sequence,
        "values": values,
    }
    return {
        "schema": campaign.PROOF_SNAPSHOT_SCHEMA,
        "requestToken": token,
        "pid": pid,
        "processStarttime": 456,
        "bootBeforeNs": boot_base,
        "bootAfterNs": boot_base + 100,
        "fex": fex,
        "runtime": {
            "schema": "lsx4-runtime-proof-counters/v1",
            "source": "funnel-runtime-diagnostic-v1",
            "effectiveBackend": "fex-experimental",
            "pid": pid,
            "bootBeforeNs": boot_base + 30,
            "bootAfterNs": boot_base + 40,
            "queryStatus": 0,
            "status": 0,
            "structSize": 256,
            "abiVersion": 1,
            "instrumentationState": "off",
            "captureFlags": 0,
            "saturated": 0,
            "requestedMask": sum(campaign.RUNTIME_METRIC_BITS.values()),
            "validMask": 0,
            "unavailableMask": sum(campaign.RUNTIME_METRIC_BITS.values()),
            "exactMask": 0,
            "sampledMask": 0,
            "staticMask": 0,
            "dynamicMask": 0,
            "perturbedMask": 0,
            "attemptedNotRetiredMask": 0,
            "epoch": 0,
            "sequence": 0,
            "values": {name: 0 for name in campaign.RUNTIME_METRIC_BITS},
        },
        "hud": {
            "schema": campaign.HUD_COUNTER_SCHEMA,
            "source": "runtimeHudStats-exact-cumulative",
            "pid": pid,
            "bootBeforeNs": boot_base + 50,
            "bootAfterNs": boot_base + 60,
            "valueCount": 11,
            "indices": {
                "generatedBlocks": 0,
                "drawCalls": 6,
                "submitCalls": 8,
                "presentCalls": 10,
            },
            "counters": {
                "generatedBlocks": 7 * sequence,
                "drawCalls": 70 * sequence,
                "submitCalls": 80 * sequence,
                "presentCalls": 90 * sequence,
            },
            "rawValues": [
                7 * sequence, 0, 0, 0, 0, 0, 70 * sequence, 0,
                80 * sequence, 0, 90 * sequence,
            ],
        },
    }


def overlap_v2_blob(*, dropped=0, include_present=True):
    records = [
        (110, 300, 1, 7, 0x1000, 4, 1 | (1 << 8)),
        (200, 200, 1, 1, 7, 0, 2 | (1 << 8)),
        (200, 250, 1, 7, 7, 1, 3 | (1 << 8)),
        (400, 400, 1, 7, 0, 1, 4 | (1 << 8)),
    ]
    if include_present:
        records.append((450, 460, 1, 7, 0, 0, 5 | (1 << 8)))
    header = campaign.OVERLAP_HEADER_V2.pack(
        b"LSX4IOT2", 2, campaign.OVERLAP_HEADER_V2.size,
        campaign.OVERLAP_RECORD_V2.size, 1,
        1024, len(records), len(records), dropped,
        0, 0, 1, 1, 0x7F,
        100, 100, 1000,
        0, 0, 0, 0, 0, 0,
    )
    return header + b"".join(campaign.OVERLAP_RECORD_V2.pack(*record)
                              for record in records)


def runtime_proof_snapshot(token="token-start", sequence=1, epoch=77,
                           include_fault=False):
    value = proof_snapshot(
        mode="off", token=token, sequence=sequence, epoch=99, required=()
    )
    runtime = value["runtime"]
    valid = sum(campaign.RUNTIME_METRIC_BITS[name] for name in
                campaign.RUNTIME_INVALIDATION_REQUIRED)
    if include_fault:
        valid |= campaign.RUNTIME_METRIC_BITS["ownedGeneratedCodeWriteFaults"]
    requested = sum(campaign.RUNTIME_METRIC_BITS.values())
    runtime.update({
        "instrumentationState": "dynamic-diagnostic",
        "captureFlags": 7 if include_fault else 3,
        "saturated": 0,
        "validMask": valid,
        "unavailableMask": requested & ~valid,
        "exactMask": valid,
        "dynamicMask": valid,
        "perturbedMask": valid,
        "attemptedNotRetiredMask": (
            campaign.RUNTIME_METRIC_BITS["guestInvalidationRequests"] |
            (campaign.RUNTIME_METRIC_BITS["ownedGeneratedCodeWriteFaults"]
             if include_fault else 0)
        ),
        "epoch": epoch,
        "sequence": sequence,
        "values": {
            "guestInvalidationRequests": 100 + sequence,
            "guestInvalidationCompletions": 90 + sequence,
            "guestInvalidationWaitNs": 1000 + sequence,
            "ownedGeneratedCodeWriteFaults": 4 if include_fault else 0,
            "fastmemMisses": 0,
        },
    })
    return value


class PlanFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.route = json.loads(ROUTE_PATH.read_text(encoding="utf-8"))
        self.expected = expected_state()
        self.pmu = pmu_capabilities()
        self.route_path = self.root / "route.json"
        self.expected_path = self.root / "expected.json"
        self.pmu_path = self.root / "pmu.json"
        self.route_path.write_text(json.dumps(self.route), encoding="utf-8")
        self.expected_path.write_text(json.dumps(self.expected), encoding="utf-8")
        self.pmu_path.write_text(json.dumps(self.pmu), encoding="utf-8")

    def plan(self, **overrides):
        values = {
            "route": self.route,
            "expected_state": self.expected,
            "pmu_capabilities": self.pmu,
            "route_path": self.route_path,
            "expected_state_path": self.expected_path,
            "pmu_capabilities_path": self.pmu_path,
            "output_root": self.root / "out",
            "campaign_id": "campaign-test",
            "adb_target": ADB_TARGET,
            "kind": "production",
        }
        values.update(overrides)
        return campaign.build_plan(**values)


class RouteContractTests(PlanFixture):
    def test_canonical_route_is_one_cross_state_anchored_hunters_dream(self):
        campaign.validate_route(self.route)
        actions = self.route["launch"]["actions"]
        self.assertEqual([action["kind"] for action in actions], ["cross-hold"])
        self.assertEqual(
            [action["stateAnchorId"] for action in actions],
            ["hunters-dream-world"],
        )
        self.assertTrue(self.route["gameplayGate"]["forbidAdditionalCross"])
        self.assertIn("Hunter's Dream", self.route["gameplayGate"]["expectedScene"])
        self.assertGreaterEqual(
            self.route["gameplayGate"]["minimumDrawCallsPerSecond"], 6000
        )
        thermal = self.route["thermal"]
        self.assertLessEqual(thermal["maximumStatus"], 2)
        self.assertGreaterEqual(thermal["minimumBatteryLevelPercent"], 20)
        self.assertTrue(thermal["requireEqualExternalPowerAcrossPhases"])
        self.assertTrue(thermal["requireEqualCpuPolicyLimitsAcrossPhases"])
        self.assertEqual(set(thermal["minimumCpuPolicyMaxKHz"]), {"0", "6"})

    def test_second_cross_is_rejected(self):
        bad = copy.deepcopy(self.route)
        bad["launch"]["actions"].append(copy.deepcopy(bad["launch"]["actions"][0]))
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "route-must-have-exactly-one-cross"):
            campaign.validate_route(bad)

    def test_moving_route_is_exact_full_window_virtual_left_stick_swipe(self):
        moving = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        campaign.validate_route(moving)
        self.assertEqual(moving["measurement"]["durationSeconds"], 14)
        self.assertEqual(
            moving["measurement"]["input"],
            campaign.MOVING_MEASUREMENT_SWIPE,
        )
        self.assertNotEqual(
            (moving["measurement"]["input"]["x1"],
             moving["measurement"]["input"]["y1"]),
            (2903, 1231),
        )
        evidence = moving["measurement"]["movementEvidence"]
        self.assertEqual(evidence["schema"], campaign.MOVEMENT_EVIDENCE_SCHEMA)
        self.assertGreaterEqual(evidence["minimumHammingDistance"], 12)
        declared_input = json.loads(MOVING_INPUT_PATH.read_text(encoding="utf-8"))
        self.assertEqual(
            {"kind": declared_input["kind"], **declared_input["gesture"]},
            moving["measurement"]["input"],
        )

    def test_moving_declaration_differs_only_by_route_and_input_roots(self):
        baseline_path = (
            ROOT / "tools" / "bloodborne-proof" / "declarations" /
            "current_vivo_baseline_declaration.json"
        )
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
        moving = json.loads(MOVING_DECLARATION_PATH.read_text(encoding="utf-8"))
        baseline["inventories"]["route"]["roots"] = [
            campaign.MOVING_ROUTE_RELATIVE_PATH
        ]
        baseline["inventories"]["input"]["roots"] = [
            campaign.MOVING_INPUT_RELATIVE_PATH
        ]
        self.assertEqual(moving, baseline)
        normalized_text = MOVING_DECLARATION_PATH.read_text(encoding="utf-8").replace(
            campaign.MOVING_ROUTE_RELATIVE_PATH,
            "tools/bloodborne-proof/routes/bath_messenger_one_cross.json",
        ).replace(
            campaign.MOVING_INPUT_RELATIVE_PATH,
            "tools/bloodborne-proof/inputs/static-no-input.json",
        )
        self.assertEqual(normalized_text, baseline_path.read_text(encoding="utf-8"))

    def test_moving_route_requires_scene_change_evidence_contract(self):
        missing = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        del missing["measurement"]["movementEvidence"]
        with self.assertRaisesRegex(
            campaign.CampaignRejected, "route-movement-evidence-fields-invalid"
        ):
            campaign.validate_route(missing)
        weak = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        weak["measurement"]["movementEvidence"]["minimumHammingDistance"] = 1
        with self.assertRaisesRegex(
            campaign.CampaignRejected, "route-movement-evidence-distance-invalid"
        ):
            campaign.validate_route(weak)

    def test_scene_change_metadata_never_claims_traversal_or_fps(self):
        moving = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        evidence = moving["measurement"]["movementEvidence"]
        before = "0" * 256
        unchanged = campaign.movement_scene_change_metadata(
            before, before, evidence
        )
        self.assertFalse(unchanged["sceneChangeProven"])
        changed = campaign.movement_scene_change_metadata(
            before, f"{(1 << 12) - 1:0256x}", evidence
        )
        self.assertTrue(changed["sceneChangeProven"])
        self.assertFalse(changed["routeTraversalDistanceClaimed"])
        self.assertFalse(changed["fpsVerdictAllowed"])

    def test_post_gameplay_cross_input_kind_is_rejected(self):
        bad = copy.deepcopy(self.route)
        bad["measurement"]["input"] = {"kind": "cross-hold"}
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "route-measurement-input-invalid"):
            campaign.validate_route(bad)

    def test_moving_swipe_coordinate_bounds_are_fail_closed(self):
        bad = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        bad["measurement"]["input"]["x1"] = campaign.DISPLAY_WIDTH
        with self.assertRaisesRegex(
            campaign.CampaignRejected,
            "route-measurement-swipe-coordinate-out-of-bounds",
        ):
            campaign.validate_route(bad)

    def test_moving_swipe_must_span_exact_measurement_window(self):
        bad = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        bad["measurement"]["durationSeconds"] = 30
        with self.assertRaisesRegex(
            campaign.CampaignRejected,
            "route-measurement-swipe-window-duration-mismatch",
        ):
            campaign.validate_route(bad)

    def test_in_bounds_but_different_swipe_is_not_matched_evidence(self):
        bad = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        bad["measurement"]["input"]["y2"] += 1
        with self.assertRaisesRegex(
            campaign.CampaignRejected,
            "route-measurement-swipe-contract-invalid",
        ):
            campaign.validate_route(bad)

    def test_missing_or_swapped_scene_anchor_is_rejected(self):
        missing = copy.deepcopy(self.route)
        del missing["visualAnchors"]["hunters-dream-world"]
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "route-visual-anchor-set-invalid"):
            campaign.validate_route(missing)
        swapped = copy.deepcopy(self.route)
        swapped["launch"]["actions"][0]["stateAnchorId"] = "unknown-world"
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "route-state-anchor-sequence-invalid"):
            campaign.validate_route(swapped)

    def test_world_anchor_is_bound_to_current_device_evidence_and_excludes_overlays(self):
        anchor = self.route["visualAnchors"]["hunters-dream-world"]
        self.assertEqual(
            anchor["sourceEvidenceSha256"],
            "7eb45679cc99127886fd208a3db72abe7612da36daf4070c3ad2220b80e3f7fd",
        )
        self.assertEqual(
            anchor["roi"], {"x": 350, "y": 300, "width": 2400, "height": 670}
        )
        self.assertEqual(len(anchor["descriptorHex"]), 256)

    def test_cache_deletion_policy_is_fail_closed(self):
        bad = copy.deepcopy(self.route)
        bad["cachePolicy"]["allowDeletion"] = True
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "route-cache-policy-not-preserving"):
            campaign.validate_route(bad)

    def test_physical_usb_serial_is_not_accepted_as_alias(self):
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "adb-target-not-exact-vivo-alias"):
            self.plan(adb_target=campaign.EXPECTED_SERIAL)


class CampaignPlanTests(PlanFixture):
    def moving_plan(self, **overrides):
        moving = json.loads(MOVING_ROUTE_PATH.read_text(encoding="utf-8"))
        moving_expected = copy.deepcopy(self.expected)
        moving_expected["inventories"] = {
            "route": {
                "source": "host",
                "roots": [campaign.MOVING_ROUTE_RELATIVE_PATH],
                "sha256": campaign.single_file_inventory_sha256(MOVING_ROUTE_PATH),
            },
            "input": {
                "source": "host",
                "roots": [campaign.MOVING_INPUT_RELATIVE_PATH],
                "sha256": campaign.single_file_inventory_sha256(MOVING_INPUT_PATH),
            },
        }
        moving_expected_path = self.root / "moving-expected.json"
        moving_expected_path.write_text(
            json.dumps(moving_expected), encoding="utf-8"
        )
        values = {
            "route": moving,
            "route_path": MOVING_ROUTE_PATH,
            "expected_state": moving_expected,
            "expected_state_path": moving_expected_path,
        }
        values.update(overrides)
        return self.plan(**values)

    def test_execute_contract_requires_read_only_bootstrap_before_mutation(self):
        plan = self.plan()
        self.assertTrue(
            plan["invariants"]["readOnlyPreMutationBootstrapRequired"]
        )
        source = MODULE_PATH.read_text(encoding="utf-8")
        bootstrap = source.index('"pre-mutation-bootstrap"')
        phase_loop = source.index('for phase in self.plan["phases"]')
        self.assertLess(bootstrap, phase_loop)

    def test_cli_defaults_comparable_campaigns_to_moving_route(self):
        args = campaign.parse_args([
            "--expected-state", str(self.expected_path),
            "--pmu-capabilities", str(self.pmu_path),
            "--adb-target", ADB_TARGET,
        ])
        self.assertEqual(args.route.resolve(), MOVING_ROUTE_PATH.resolve())

    def test_axis_aba_has_one_axis_and_multiblock_off(self):
        plan = self.plan(kind="axis", axis="MemcpySetTSO", order="ABA")
        self.assertEqual([phase["variant"] for phase in plan["phases"]],
                         ["A", "B", "A"])
        marker = campaign.AXIS_MARKERS["MemcpySetTSO"]
        self.assertNotIn(marker, plan["phases"][0]["enabledManagedMarkers"])
        self.assertIn(marker, plan["phases"][1]["enabledManagedMarkers"])
        for phase in plan["phases"]:
            self.assertNotIn(campaign.MULTIBLOCK_MARKER,
                             phase["enabledManagedMarkers"])

    def test_axis_bab_supported(self):
        plan = self.plan(kind="axis", axis="VectorTSO", order="BAB")
        self.assertEqual([phase["variant"] for phase in plan["phases"]],
                         ["B", "A", "B"])

    def test_memory_axis_b_derives_exact_backend_payload_and_hash(self):
        cases = {
            "MemcpySetTSO": {
                "configureFlags": 2,
                "cacheSuffix": "-lookup-throughput-ab-mtso0-sm-swtso",
                "strongMemoryModel": (
                    "FEX software TSO (MemcpySetTSO relaxed A/B)"
                ),
            },
            "VectorTSO": {
                "configureFlags": 1,
                "cacheSuffix": "-lookup-throughput-ab-vtso0-sm-swtso",
                "strongMemoryModel": (
                    "FEX software TSO (VectorTSO relaxed A/B)"
                ),
            },
        }
        baseline_backend = self.expected["backend"]
        for axis, wanted in cases.items():
            with self.subTest(axis=axis):
                plan = self.plan(kind="axis", axis=axis, order="ABA",
                                 campaign_id=f"campaign-{axis.lower()}")
                a1, b, a2 = [phase["expectedState"]["backend"]
                             for phase in plan["phases"]]
                self.assertEqual(a1, baseline_backend)
                self.assertEqual(a2, baseline_backend)
                status = b["status"]
                self.assertEqual(status["configureFlags"],
                                 wanted["configureFlags"])
                self.assertTrue(status["cacheNamespace"].endswith(
                    wanted["cacheSuffix"]
                ))
                self.assertEqual(status["providedCapabilities"], 8187)
                self.assertEqual(status["requiredCapabilities"], 8187)
                self.assertEqual(status["forbiddenCapabilities"], 4)
                self.assertEqual(status["strongMemoryModel"],
                                 wanted["strongMemoryModel"])
                self.assertEqual(b["statusSha256"],
                                 campaign.canonical_sha256(status))

    def test_memory_axis_rejects_noncanonical_baseline_contract(self):
        mutations = {
            "flag": lambda status: status.update(configureFlags=1),
            "suffix": lambda status: status.update(cacheNamespace="bad-cache"),
            "provided-caps": lambda status: status.update(
                providedCapabilities=8187),
            "required-caps": lambda status: status.update(
                requiredCapabilities=8187),
            "forbidden-caps": lambda status: status.update(
                forbiddenCapabilities=4),
            "model": lambda status: status.update(
                strongMemoryModel="wrong-model"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                broken = copy.deepcopy(self.expected)
                mutate(broken["backend"]["status"])
                broken["backend"]["statusSha256"] = campaign.canonical_sha256(
                    broken["backend"]["status"]
                )
                with self.assertRaises(campaign.CampaignRejected):
                    self.plan(kind="axis", axis="MemcpySetTSO", order="ABA",
                              expected_state=broken,
                              campaign_id=f"campaign-bad-{name}")

    def test_each_phase_has_full_and_cleanup_preflight_before_measurement(self):
        plan = self.plan(kind="axis", axis="VectorTSO", order="ABA")
        for phase in plan["phases"]:
            actions = phase["actions"]
            preflights = [action for action in actions
                          if action["kind"] == "mandatory-performance-preflight"]
            self.assertEqual([item["cleanupOnly"] for item in preflights],
                             [False, True])
            cleanup_index = actions.index(preflights[1])
            window_index = next(i for i, item in enumerate(actions)
                                if item["kind"] == "measurement-window")
            self.assertLess(cleanup_index, window_index)

    def test_moving_input_occurs_only_after_exact_gameplay_gate(self):
        plan = self.moving_plan()
        actions = plan["phases"][0]["actions"]
        kinds = [action["kind"] for action in actions]
        route_index = kinds.index("one-cross-state-anchored-route")
        gameplay_index = kinds.index("hunters-dream-gameplay-gate")
        measurement_index = kinds.index("measurement-window")
        self.assertLess(route_index, gameplay_index)
        self.assertLess(gameplay_index, measurement_index)
        self.assertTrue(actions[gameplay_index]["forbidAdditionalCross"])
        self.assertEqual(
            actions[measurement_index]["input"],
            campaign.MOVING_MEASUREMENT_SWIPE,
        )
        self.assertEqual(
            plan["invariants"]["measurementInputSha256"],
            campaign.canonical_sha256(campaign.MOVING_MEASUREMENT_SWIPE),
        )
        evidence = actions[measurement_index]["movementEvidence"]
        self.assertEqual(
            plan["invariants"]["movementEvidenceSha256"],
            campaign.canonical_sha256(evidence),
        )
        self.assertTrue(plan["invariants"]["movementSceneChangeRequired"])

    def test_executor_orders_visual_gameplay_gate_before_moving_window(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        run_body = source[source.index("    def run(self) -> dict[str, Any]:"):]
        route = run_body.index("self._launch_route(phase_dir)")
        gameplay = run_body.index("self._gameplay_gate(phase, phase_dir, identity)")
        measurement = run_body.index("self._measurement(phase, phase_dir, identity, surface)")
        self.assertLess(route, gameplay)
        self.assertLess(gameplay, measurement)
        measurement_body = source[
            source.index("    def _measurement("):source.index(
                "    def run(self) -> dict[str, Any]:"
            )
        ]
        self.assertIn('input_spec = measurement_action["input"]', measurement_body)
        self.assertNotIn('input_spec = self.route["measurement"]', measurement_body)
        before = measurement_body.index('movement_contract, phase_dir, "before"')
        simpleperf = measurement_body.index("simpleperf_process = subprocess.Popen(")
        after = measurement_body.index('movement_contract, phase_dir, "after"')
        self.assertLess(before, simpleperf)
        self.assertLess(simpleperf, after)

    def test_plan_rejects_movement_evidence_drift(self):
        plan = self.moving_plan()
        action = next(
            item for item in plan["phases"][0]["actions"]
            if item["kind"] == "measurement-window"
        )
        action["movementEvidence"]["minimumHammingDistance"] += 1
        with self.assertRaisesRegex(
            campaign.CampaignRejected, "plan-movement-evidence-hash-mismatch"
        ):
            campaign.validate_plan(plan)

    def test_plan_rejects_measurement_before_gameplay_gate(self):
        plan = self.moving_plan()
        actions = plan["phases"][0]["actions"]
        gameplay_index = next(
            i for i, action in enumerate(actions)
            if action["kind"] == "hunters-dream-gameplay-gate"
        )
        measurement_index = next(
            i for i, action in enumerate(actions)
            if action["kind"] == "measurement-window"
        )
        actions[gameplay_index], actions[measurement_index] = (
            actions[measurement_index], actions[gameplay_index]
        )
        with self.assertRaisesRegex(
            campaign.CampaignRejected, "plan-gameplay-measurement-order-invalid"
        ):
            campaign.validate_plan(plan)

    def test_plan_rejects_any_cross_after_gameplay_gate(self):
        plan = self.moving_plan()
        actions = plan["phases"][0]["actions"]
        gameplay_index = next(
            i for i, action in enumerate(actions)
            if action["kind"] == "hunters-dream-gameplay-gate"
        )
        actions.insert(gameplay_index + 1, {"kind": "cross-hold"})
        with self.assertRaisesRegex(
            campaign.CampaignRejected, "plan-cross-after-gameplay-gate"
        ):
            campaign.validate_plan(plan)

    def test_moving_route_rejects_duration_override_that_outlasts_input(self):
        with self.assertRaisesRegex(
            campaign.CampaignRejected,
            "route-measurement-swipe-window-duration-mismatch",
        ):
            self.moving_plan(duration_seconds=30)

    def test_moving_runner_rejects_static_route_input_inventory_hashes(self):
        wrong = copy.deepcopy(self.expected)
        wrong["inventories"] = {
            "route": {
                "source": "host",
                "roots": [campaign.MOVING_ROUTE_RELATIVE_PATH],
                "sha256": campaign.single_file_inventory_sha256(ROUTE_PATH),
            },
            "input": {
                "source": "host",
                "roots": [campaign.MOVING_INPUT_RELATIVE_PATH],
                "sha256": campaign.single_file_inventory_sha256(STATIC_INPUT_PATH),
            },
        }
        wrong_path = self.root / "wrong-static-inventories.json"
        wrong_path.write_text(json.dumps(wrong), encoding="utf-8")
        with self.assertRaisesRegex(
            campaign.CampaignRejected,
            "moving-expected-inventory-hash-mismatch",
        ):
            self.moving_plan(
                expected_state=wrong, expected_state_path=wrong_path
            )

    def test_every_direct_adb_action_declares_event_and_both_ownership_gates(self):
        plan = self.plan(kind="axis", axis="MemcpySetTSO", order="ABA")
        expected = ["event", "own:device:vivo",
                    f"own:adb:{campaign.EXPECTED_SERIAL}"]
        for phase in plan["phases"]:
            adb_actions = [action for action in phase["actions"]
                           if action.get("adbTargetPolicy")]
            self.assertGreater(len(adb_actions), 0)
            self.assertTrue(all(action["agentChatGate"] == expected
                                for action in adb_actions))

    def test_production_has_no_proof_instrumentation(self):
        plan = self.plan()
        phase = plan["phases"][0]
        self.assertEqual(phase["instrumentationMode"], "off")
        self.assertEqual(len(set(phase["proofTokens"].values())), 2)
        self.assertTrue(phase["fpsVerdictAllowed"])
        self.assertFalse(set(phase["enabledManagedMarkers"]) &
                         set(campaign.INSTRUMENTATION_MARKERS.values()))

    def test_static_and_dynamic_are_diagnostic_only_and_exclusive(self):
        for mode in ("static", "dynamic"):
            plan = self.plan(kind=mode, campaign_id=f"campaign-{mode}")
            phase = plan["phases"][0]
            self.assertFalse(phase["fpsVerdictAllowed"])
            self.assertEqual(phase["instrumentationMode"], mode)
            enabled = set(phase["enabledManagedMarkers"])
            self.assertEqual(enabled & set(campaign.INSTRUMENTATION_MARKERS.values()),
                             {campaign.INSTRUMENTATION_MARKERS[mode]})
            self.assertEqual(len(set(phase["proofTokens"].values())), 2)
            transport = phase["proofSnapshotTransport"]
            self.assertEqual(transport["kind"], "shell-broadcast")
            self.assertEqual(transport["component"], campaign.PROOF_RECEIVER)
            self.assertFalse(transport["activityLaunchAllowed"])

    def test_instrumentation_cannot_be_combined_with_axis(self):
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "axis-only-valid-for-axis"):
            self.plan(kind="dynamic", axis="MemcpySetTSO")

    def test_simpleperf_plan_attaches_to_fresh_existing_pid(self):
        plan = self.plan()
        action = next(action for action in plan["phases"][0]["actions"]
                      if action["kind"] == "measurement-window")
        self.assertEqual(action["attachPid"], "fresh-game-pid")
        self.assertEqual(action["simpleperfLaunchContext"], "shell")
        self.assertEqual(action["simpleperfAppScope"], campaign.PACKAGE)
        self.assertFalse(action["simpleperfChildWorkload"])
        self.assertEqual(action["simpleperfEvents"], list(campaign.PMU_EVENTS))
        self.assertNotIn("command", action)

    def test_runner_materializes_scene_normalized_cost_metrics(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn("proof_contract.derive_metrics(", source)
        self.assertIn('"derivedMetrics": derived_metrics', source)
        self.assertIn('proof_delta["runtime"], proof_delta["fex"]', source)

    def test_simpleperf_attach_uses_shell_app_scope_without_child_workload(self):
        argv = campaign.simpleperf_attach_argv(
            "adb", ADB_TARGET, 12345, 14
        )
        self.assertEqual(argv[:6], [
            "adb", "-s", ADB_TARGET, "shell", "/system/bin/simpleperf", "stat",
        ])
        self.assertEqual(argv[6:11], [
            "--csv", "--app", campaign.PACKAGE, "-p", "12345",
        ])
        self.assertEqual(argv[11:], [
            "--duration", "14", "-e", ",".join(campaign.PMU_EVENTS),
        ])
        self.assertNotIn("run-as", argv)
        self.assertEqual(argv[-1], ",".join(campaign.PMU_EVENTS))

    def test_simpleperf_attach_rejects_invalid_pid_and_duration(self):
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "simpleperf-attach-pid-invalid"):
            campaign.simpleperf_attach_argv("adb", ADB_TARGET, 0, 14)
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "simpleperf-duration-invalid"):
            campaign.simpleperf_attach_argv("adb", ADB_TARGET, 12345, 0)

    def test_plan_contains_no_blind_cache_delete(self):
        encoded = json.dumps(self.plan(), sort_keys=True).lower()
        self.assertNotIn("rm -rf", encoded)
        self.assertNotIn("pm clear", encoded)
        self.assertEqual(self.route["cachePolicy"]["mode"],
                         "preserve-matched-inventory")

    def test_complete_panel_is_explicit_and_not_a_proxy(self):
        plan = self.plan(kind="dynamic", campaign_id="campaign-panel",
                         require_complete_panel=True)
        required = set(plan["phases"][0]["proofMetricsRequired"])
        self.assertEqual(required, set(campaign.COMPLETE_PANEL_METRICS))
        self.assertIn("executedGuestInstructionEntries", required)
        self.assertIn("executedDispatcherEntries", required)

    def test_overlap_is_separate_perturbed_diagnostic_with_v2_selector(self):
        plan = self.plan(kind="overlap", campaign_id="campaign-overlap")
        phase = plan["phases"][0]
        self.assertFalse(phase["fpsVerdictAllowed"])
        self.assertEqual(phase["enabledManagedMarkers"], [campaign.OVERLAP_MARKER])
        kinds = [action["kind"] for action in phase["actions"]]
        self.assertIn("prepare-causal-overlap-v2-control", kinds)
        window = next(action for action in phase["actions"]
                      if action["kind"] == "measurement-window")
        self.assertEqual(window["overlapMode"], "causal-v2-instrumented")

    def test_runtime_invalidation_campaign_is_perturbed_and_not_fps_evidence(self):
        plan = self.plan(kind="runtime", campaign_id="campaign-runtime")
        phase = plan["phases"][0]
        self.assertFalse(phase["fpsVerdictAllowed"])
        self.assertEqual(phase["runtimeInstrumentationMode"],
                         "dynamic-diagnostic")
        self.assertEqual(phase["enabledManagedMarkers"],
                         [campaign.RUNTIME_COUNTER_MARKER])
        self.assertEqual(set(phase["runtimeMetricsRequired"]),
                         set(campaign.RUNTIME_INVALIDATION_REQUIRED))

    def test_runtime_fault_campaign_refuses_live_quiet(self):
        self.expected["markers"]["allowlist"].append("run-live-quiet")
        self.expected["markers"]["present"].append("run-live-quiet")
        self.expected["markers"]["presentSha256"] = campaign.canonical_sha256(
            sorted(self.expected["markers"]["present"])
        )
        self.expected_path.write_text(json.dumps(self.expected), encoding="utf-8")
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "runtime-fault-proof-requires-live-quiet-off"):
            self.plan(kind="runtime-faults", campaign_id="campaign-fault")

    def test_runtime_fault_campaign_requires_base_invalidation_counters_too(self):
        plan = self.plan(kind="runtime-faults", campaign_id="campaign-fault")
        required = set(plan["phases"][0]["runtimeMetricsRequired"])
        self.assertEqual(required, set(campaign.RUNTIME_FAULT_REQUIRED))
        self.assertTrue(set(campaign.RUNTIME_INVALIDATION_REQUIRED) <= required)
        self.assertIn("ownedGeneratedCodeWriteFaults", required)

    def test_production_refuses_historical_expensive_markers(self):
        marker = "run-enable-fex-buffer-capture-ack"
        self.expected["markers"]["allowlist"].append(marker)
        self.expected["markers"]["present"].append(marker)
        self.expected["markers"]["presentSha256"] = campaign.canonical_sha256(
            sorted(self.expected["markers"]["present"])
        )
        self.expected_path.write_text(json.dumps(self.expected), encoding="utf-8")
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "baseline-forbidden-marker-present"):
            self.plan(campaign_id="campaign-forbidden")


class ParserAndSnapshotTests(unittest.TestCase):
    def test_hud_draw_gate_uses_conservative_exact_counter_lower_bound(self):
        start = proof_snapshot(sequence=1)["hud"]
        end = proof_snapshot(sequence=2)["hud"]
        end["counters"]["drawCalls"] = 7070
        end["rawValues"][6] = 7070
        result = campaign.hud_draw_rate_lower_bound(start, end, pid=123)
        self.assertGreater(result["drawCallsPerSecondLowerBound"], 6_000_000)
        self.assertFalse(result["fpsVerdictAllowed"])

    def test_hud_draw_gate_rejects_index_spoof_and_counter_regression(self):
        start = proof_snapshot(sequence=1)["hud"]
        bad_index = copy.deepcopy(start)
        bad_index["indices"]["drawCalls"] = 7
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "proof-hud-index-contract-invalid"):
            campaign.validate_hud_counters(bad_index, pid=123)

        end = proof_snapshot(sequence=2)["hud"]
        end["counters"]["drawCalls"] = 1
        end["rawValues"][6] = 1
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "proof-hud-counter-regressed"):
            campaign.hud_draw_rate_lower_bound(start, end, pid=123)

    def test_runtime_invalidation_snapshot_accepts_only_explicit_exact_subset(self):
        start = runtime_proof_snapshot()
        end = runtime_proof_snapshot(token="token-end", sequence=2)
        campaign.validate_proof_snapshot(
            start, mode="off", token="token-start", pid=123,
            process_starttime=456, required_metrics=(),
            runtime_mode="dynamic-diagnostic",
            runtime_required_metrics=campaign.RUNTIME_INVALIDATION_REQUIRED,
        )
        campaign.validate_proof_snapshot(
            end, mode="off", token="token-end", pid=123,
            process_starttime=456, required_metrics=(),
            runtime_mode="dynamic-diagnostic",
            runtime_required_metrics=campaign.RUNTIME_INVALIDATION_REQUIRED,
            previous=start,
        )

    def test_runtime_fastmem_zero_is_rejected_as_unavailable_not_evidence(self):



        value = runtime_proof_snapshot(include_fault=True)
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "runtime-counter-unavailable: fastmemMisses"):
            campaign.validate_proof_snapshot(
                value, mode="off", token="token-start", pid=123,
                process_starttime=456, required_metrics=(),
                runtime_mode="dynamic-diagnostic",
                runtime_required_metrics=campaign.RUNTIME_COMPLETE_PANEL,
            )

    def test_runtime_saturation_is_rejected(self):
        value = runtime_proof_snapshot()
        value["runtime"]["saturated"] = 1
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "runtime-counter-saturated"):
            campaign.validate_proof_snapshot(
                value, mode="off", token="token-start", pid=123,
                process_starttime=456, required_metrics=(),
                runtime_mode="dynamic-diagnostic",
                runtime_required_metrics=campaign.RUNTIME_INVALIDATION_REQUIRED,
            )

    def test_overlap_v2_requires_complete_request_to_present_membership(self):
        parsed = campaign.parse_overlap_v2(overlap_v2_blob())
        self.assertEqual(parsed["schema"], "lsx4-invalidation-overlap/v2")
        self.assertEqual(parsed["causalPairs"], 1)
        self.assertTrue(parsed["causalMembershipExact"])
        self.assertFalse(parsed["delayCausalityClaimed"])
        self.assertFalse(parsed["productionFrametime"])

    def test_overlap_v2_rejects_drop_and_missing_present(self):
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "overlap-v2-inexact-header"):
            campaign.parse_overlap_v2(overlap_v2_blob(dropped=1))
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "overlap-v2-no-complete-causal-membership"):
            campaign.parse_overlap_v2(overlap_v2_blob(include_present=False))

    def test_preflight_surface_temperature_band_parser(self):
        text = (
            "Vivo preflight sample: cpuIdle=88% memAvailable=4000 MiB "
            "thermalStatus=1 surface=[quiet=37.2C, battery=36.0C]."
        )
        self.assertEqual(campaign.parse_preflight_surface_max_c(text), 37.2)
        self.assertIsNone(campaign.parse_preflight_surface_max_c(
            "surface=[unavailable]."
        ))

    def test_preflight_admission_parser_is_exact_and_fail_closed(self):
        text = (
            "Vivo preflight sample: cpuIdle=88% memAvailable=4000 MiB "
            "thermalStatus=0 surface=[quiet=37.2C] batteryLevel=31% "
            "externalPower=true "
            "cpuPolicies=[policy0=1440000/537600/2361600KHz,"
            "policy6=883200/883200/2438400KHz]."
        )
        parsed = campaign.parse_preflight_admission(text)
        self.assertEqual(parsed["batteryLevelPercent"], 31)
        self.assertTrue(parsed["externalPower"])
        self.assertEqual(parsed["cpuPolicies"]["0"]["maximumKHz"], 2361600)
        self.assertEqual(parsed["cpuPolicies"]["6"]["minimumKHz"], 883200)
        with self.assertRaisesRegex(
            campaign.CampaignRejected, "preflight-admission-evidence-unavailable"
        ):
            campaign.parse_preflight_admission("batteryLevel=31%")

    def test_campaign_preflight_uses_utf8_safe_powershell(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        preflight = source.split("def _preflight", 1)[1].split(
            "def _set_markers", 1
        )[0]
        self.assertIn('"pwsh", "-NoProfile"', preflight)
        self.assertNotIn('"powershell", "-NoProfile"', preflight)

    def test_campaign_cooldown_uses_vivo_dumpsys_before_optional_cmd_fallback(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        thermal = source.split("def _thermal_status", 1)[1].split(
            "def _cooldown", 1
        )[0]
        dump = '["shell", "dumpsys", "thermalservice"]'
        fallback = '["shell", "cmd", "thermalservice", "get-status"]'
        self.assertIn(dump, thermal)
        self.assertIn(fallback, thermal)
        self.assertLess(thermal.index(dump), thermal.index(fallback))
        self.assertGreaterEqual(thermal.count("allow_failure=True"), 2)
        self.assertIn(r'^Thermal Status:\s*([0-6])\s*$', thermal)

    def test_proc_stat_uses_linux_field_22_after_parenthesized_comm(self):
        fields_3_to_22 = ["S"] + [str(value) for value in range(4, 23)]
        text = "123 (thread name) " + " ".join(fields_3_to_22)
        pid, comm, starttime = campaign.parse_proc_stat(text)
        self.assertEqual((pid, comm, starttime), (123, "thread name", 22))

    def test_surface_identity_must_be_one_exact_generation(self):
        component = (
            "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST)"
        )
        line = f"abc {component} 08-24 10:00:00.000#42"
        self.assertEqual(campaign.select_exact_game_surface(line, component),
                         {"identity": line, "generation": 42})
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "surface-identity-not-unique"):
            campaign.select_exact_game_surface(f"{line}\n{line}", component)

    def test_snapshot_rejects_required_unavailable_metric(self):
        value = proof_snapshot()
        metric = "compiledSpillSites"
        bit = campaign.PROOF_METRIC_BITS[metric]
        value["fex"]["unavailableMask"] |= bit
        value["fex"]["validMask"] &= ~bit
        value["fex"]["exactMask"] &= ~bit
        value["fex"]["staticMask"] &= ~bit
        value["fex"]["values"][metric] = 0
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "proof-metric-unavailable"):
            campaign.validate_proof_snapshot(
                value, mode="static", token="token-start", pid=123,
                process_starttime=456,
                required_metrics=campaign.STATIC_REQUIRED_METRICS,
                runtime_mode="off", runtime_required_metrics=(),
            )

    def test_snapshot_rejects_stale_sequence_and_mixed_epoch(self):
        start = proof_snapshot()
        stale = proof_snapshot(token="token-end", sequence=1)
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "proof-snapshot-sequence-stale"):
            campaign.validate_proof_snapshot(
                stale, mode="static", token="token-end", pid=123,
                process_starttime=456,
                required_metrics=campaign.STATIC_REQUIRED_METRICS,
                runtime_mode="off", runtime_required_metrics=(),
                previous=start,
            )
        mixed = proof_snapshot(token="token-end", sequence=2, epoch=100)
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "proof-snapshot-epoch-mixed"):
            campaign.validate_proof_snapshot(
                mixed, mode="static", token="token-end", pid=123,
                process_starttime=456,
                required_metrics=campaign.STATIC_REQUIRED_METRICS,
                runtime_mode="off", runtime_required_metrics=(),
                previous=start,
            )

    def test_complete_panel_rejects_missing_exact_guest_denominator(self):
        value = proof_snapshot(mode="dynamic", required=campaign.DYNAMIC_REQUIRED_METRICS)
        with self.assertRaisesRegex(campaign.CampaignRejected,
                                   "proof-metric-unavailable"):
            campaign.validate_proof_snapshot(
                value, mode="dynamic", token="token-start", pid=123,
                process_starttime=456,
                required_metrics=campaign.COMPLETE_PANEL_METRICS,
                runtime_mode="off", runtime_required_metrics=(),
            )


class FakeHost:
    def __init__(self):
        self.argv = []

    def run(self, argv, **_kwargs):
        self.argv.append(list(argv))
        return campaign.CommandResult(tuple(argv), 0, b"ok\n", b"")


class ExecutionSafetyTests(unittest.TestCase):
    def test_absence_probe_accepts_only_clean_test_exit_one(self):
        campaign.require_unambiguous_absence(
            campaign.CommandResult(("adb",), 1, b"", b""), label="fresh-token"
        )
        for result in (
            campaign.CommandResult(("adb",), 0, b"", b""),
            campaign.CommandResult(("adb",), 2, b"", b""),
            campaign.CommandResult(("adb",), 1, b"", b"transport warning"),
        ):
            with self.assertRaises(campaign.CampaignRejected):
                campaign.require_unambiguous_absence(result, label="not-proven")

    def test_guarded_adb_gates_each_call_before_adb(self):
        host = FakeHost()
        guarded = campaign.GuardedAdb(
            host, adb="adb", target=ADB_TARGET,
            agentchat=pathlib.Path("agentchat.py"), agent="codex",
            room="увеличить fps",
        )
        guarded.run(["shell", "true"], summary="unit-test")
        self.assertEqual(len(host.argv), 4)
        self.assertIn("event", host.argv[0])
        self.assertIn("device:vivo", host.argv[1])
        self.assertIn(f"adb:{campaign.EXPECTED_SERIAL}", host.argv[2])
        self.assertEqual(host.argv[3][:3], ["adb", "-s", ADB_TARGET])

    def test_artifact_directory_cannot_be_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            existing = pathlib.Path(directory) / "existing"
            existing.mkdir()
            with self.assertRaisesRegex(campaign.CampaignRejected,
                                       "artifact-directory-already-exists"):
                campaign.ExclusiveStore(existing)

    def test_cli_dry_run_does_not_create_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            expected_path = root / "expected.json"
            pmu_path = root / "pmu.json"
            expected_path.write_text(json.dumps(expected_state()), encoding="utf-8")
            pmu_path.write_text(json.dumps(pmu_capabilities()), encoding="utf-8")
            output = root / "must-not-exist"
            result = subprocess.run([
                sys.executable, str(MODULE_PATH),
                "--dry-run", "--route", str(ROUTE_PATH),
                "--expected-state", str(expected_path),
                "--pmu-capabilities", str(pmu_path),
                "--output-root", str(output),
                "--campaign-id", "dry-run-test",
                "--adb-target", ADB_TARGET,
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertTrue(payload["dryRun"])
            self.assertEqual(payload["plan"]["schema"], campaign.PLAN_SCHEMA)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
