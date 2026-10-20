import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "bloodborne-proof" / "capture_provenance.py"
SPEC = importlib.util.spec_from_file_location("bloodborne_surface_identity", MODULE_PATH)
provenance = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = provenance
SPEC.loader.exec_module(provenance)


COMPONENT = (
    "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST)"
)
CURRENT = f"19589a2 {COMPONENT} 08-24 19:11:37.060#20907"
OLD = f"19589a2 {COMPONENT} 08-24 18:28:00.098#20322"
EXPECTED = {
    "requiredComponent": COMPONENT,
    "displayId": "0",
    "identityRegex": (
        r"layerId=(?P<layerId>\d+).*generation=(?P<generation>\d+)"
        r".*displayId=(?P<displayId>\d+)"
    ),
}


def listing(*names):
    return "\n".join(
        f"RequestedLayerState{{{name} parentId=20906}}" for name in names
    )


def timestats(*names):
    return "\n".join(f"layerName = {name}" for name in names)


def full(*entries):
    body = [
        "SurfaceFlinger New Frontend Enabled:true",
        "Active Layers - layers with client handles (count = 205)",
        "",
        "Composition list (bottom to top)",
    ]
    for display_id, layer_id, name, visibility in entries:
        body.extend([
            f"LayerStack={display_id}",
            f"  Layer [{layer_id}] {name}",
            f"    {visibility} reason= buffer=1 frame=2",
        ])
    body.extend(["Offscreen Hierarchy", "  historical data"])
    return "\n".join(body)


class BloodborneSurfaceIdentityTests(unittest.TestCase):
    def test_historical_timestats_do_not_make_current_surface_ambiguous(self):
        result = provenance.parse_active_surface_identity(
            listing(CURRENT),
            timestats(OLD, CURRENT),
            full(("0", "20907", CURRENT, "visible")),
            EXPECTED,
        )
        self.assertEqual(result, {
            "layerId": "20907",
            "generation": "20907",
            "displayId": "0",
            "layerName": CURRENT,
        })

    def test_two_current_requested_layers_are_rejected(self):
        second = f"deadbee {COMPONENT} 08-24 19:12:00.000#20999"
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "surface-layer-not-unique"
        ):
            provenance.parse_active_surface_identity(
                listing(CURRENT, second),
                timestats(CURRENT, second),
                full(
                    ("0", "20907", CURRENT, "visible"),
                    ("0", "20999", second, "visible"),
                ),
                EXPECTED,
            )

    def test_current_layer_must_exist_in_timestats(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected,
            "surface-current-layer-missing-from-timestats",
        ):
            provenance.parse_active_surface_identity(
                listing(CURRENT),
                timestats(OLD),
                full(("0", "20907", CURRENT, "visible")),
                EXPECTED,
            )

    def test_two_active_layer_ids_for_same_name_are_rejected(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "surface-active-identity-not-unique"
        ):
            provenance.parse_active_surface_identity(
                listing(CURRENT),
                timestats(CURRENT),
                full(
                    ("0", "20907", CURRENT, "visible"),
                    ("0", "20908", CURRENT, "visible"),
                ),
                EXPECTED,
            )

    def test_invisible_only_layer_is_rejected(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "surface-active-identity-not-unique"
        ):
            provenance.parse_active_surface_identity(
                listing(CURRENT),
                timestats(CURRENT),
                full(("0", "20907", CURRENT, "invisible")),
                EXPECTED,
            )

    def test_wrong_display_stack_is_rejected(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "surface-display-mismatch"
        ):
            provenance.parse_active_surface_identity(
                listing(CURRENT),
                timestats(CURRENT),
                full(("1", "20907", CURRENT, "visible")),
                EXPECTED,
            )


if __name__ == "__main__":
    unittest.main()
