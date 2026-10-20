import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "parse_surfaceflinger_timestats.py"
)
SPEC = importlib.util.spec_from_file_location(
    "bloodborne_proof_surfaceflinger", MODULE_PATH
)
surface = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = surface
SPEC.loader.exec_module(surface)


IDENTITY = (
    "f7b4ab9 "
    "SurfaceView[app.lsx4.android/app.lsx4.android.GameActivity](BLAST) "
    "08-21 18:50:00.841#50085"
)
BUCKETS = (0, 16, 33, 40, 50, 102, 200, 500, 1000)


def histogram(**counts):
    values = {int(key.removeprefix("b")): value for key, value in counts.items()}
    return " ".join(f"{bucket}ms={values.get(bucket, 0)}" for bucket in BUCKETS)


def layer_section(
    identity,
    *,
    display=60,
    render=60,
    frames,
    dropped=0,
    average=30.0,
    histogram_line,
):
    return f"""
displayRefreshRate = {display} fps
renderRate = {render} fps
uid = 10358
layerName = {identity}
packageName =
gameMode = Unsupported
totalFrames = {frames}
droppedFrames = {dropped}
lateAcquireFrames = 0
badDesiredPresentFrames = 0
Jank payload for this layer:
totalTimelineFrames = 0
jankyFrames = 0
SetFrameRate vote for this layer:
frameRate = 0.00
frameRateCompatibility = Undefined
seamlessness = Undefined
averageFPS = {average}
present2present histogram is as below:
{histogram_line}
latch2present histogram is as below:
{histogram()}
"""


def capture(*sections, start=1787349216, end=1787349236):
    return (
        "SurfaceFlinger TimeStats:\n"
        "Legacy stats are as follows:\n"
        f"statsStart = {start}\n"
        f"statsEnd = {end}\n"
        "totalFrames = 481\n"
        "missedFrames = 73\n"
        + "".join(sections)
    )


class SurfaceFlingerProofParserTests(unittest.TestCase):
    def test_aggregates_one_exact_identity_across_rate_pairs_and_ignores_parent(self):
        parent = layer_section(
            "app.lsx4.android/app.lsx4.android.GameActivity "
            "08-21 18:50:00.832#50079",
            frames=99,
            average=60,
            histogram_line=histogram(b16=99),
        )
        at_60 = layer_section(
            IDENTITY,
            frames=12,
            average=30,
            histogram_line=histogram(b33=6, b40=4, b50=2),
        )
        at_120 = layer_section(
            IDENTITY,
            display=120,
            render=120,
            frames=8,
            average=40,
            histogram_line=histogram(b16=2, b33=2, b102=3, b1000=1),
        )

        result = surface.parse_surfaceflinger_timestats(
            capture(parent, at_60, at_120), expected_layer_identity=IDENTITY
        )

        self.assertEqual(result["schema"], surface.SCHEMA)
        self.assertEqual(result["layer"]["identity"], IDENTITY)
        self.assertEqual(result["layer"]["generation"], 50085)
        self.assertEqual(result["layer"]["totalFrames"], 20)
        self.assertEqual(result["layer"]["droppedFrames"], 0)
        self.assertAlmostEqual(result["layer"]["aggregateReportedFps"], 100 / 3)
        self.assertEqual(result["ignoredParentGameActivityRecords"], 1)
        self.assertEqual(
            [
                (item["displayRefreshRateFps"], item["renderRateFps"], item["totalFrames"])
                for item in result["layer"]["rateBreakdown"]
            ],
            [(60, 60, 12), (120, 120, 8)],
        )

        distribution = result["present2present"]
        self.assertEqual(distribution["sampleCount"], 20)
        self.assertEqual(distribution["p50UpperBoundMs"], 33)
        self.assertEqual(distribution["p90UpperBoundMs"], 102)
        self.assertEqual(distribution["p95UpperBoundMs"], 102)
        self.assertIsNone(distribution["p99UpperBoundMs"])
        self.assertTrue(distribution["p99RightCensored"])
        self.assertAlmostEqual(distribution["le33MsFraction"], 0.50)
        self.assertAlmostEqual(distribution["le40MsFraction"], 0.70)
        self.assertAlmostEqual(distribution["gt40MsFraction"], 0.30)
        self.assertAlmostEqual(distribution["gt50MsFraction"], 0.20)
        self.assertAlmostEqual(distribution["ge100MsFraction"], 0.20)
        self.assertAlmostEqual(distribution["ge200MsFraction"], 0.05)
        self.assertAlmostEqual(distribution["ge500MsFraction"], 0.05)
        self.assertAlmostEqual(distribution["ge1000MsFraction"], 0.05)

    def test_1000ms_bucket_is_right_censored_and_never_an_exact_maximum(self):
        text = capture(
            layer_section(
                IDENTITY,
                frames=2,
                average=1,
                histogram_line=histogram(b500=1, b1000=1),
            )
        )
        distribution = surface.parse_surfaceflinger_timestats(text)["present2present"]

        self.assertTrue(distribution["rightCensored"])
        self.assertEqual(distribution["rightCensorLowerBoundMs"], 1000)
        self.assertEqual(distribution["rightCensoredCount"], 1)
        self.assertEqual(distribution["largestFiniteNonzeroBucketUpperBoundMs"], 500)
        self.assertIsNone(distribution["exactMaximumMs"])
        self.assertFalse(distribution["maximumIsExact"])
        self.assertIsNone(distribution["p90UpperBoundMs"])
        self.assertTrue(distribution["p90RightCensored"])

    def test_rejects_two_distinct_positive_blast_generations(self):
        other = IDENTITY.rsplit("#", 1)[0] + "#50086"
        text = capture(
            layer_section(
                IDENTITY,
                frames=1,
                histogram_line=histogram(b33=1),
            ),
            layer_section(
                other,
                display=120,
                render=120,
                frames=1,
                histogram_line=histogram(b33=1),
            ),
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError, "distinct_positive_blast_generations"
        ):
            surface.parse_surfaceflinger_timestats(text)

    def test_rejects_distinct_full_identity_even_with_same_generation(self):
        other = IDENTITY.replace("f7b4ab9", "deadbee")
        text = capture(
            layer_section(
                IDENTITY,
                frames=1,
                histogram_line=histogram(b33=1),
            ),
            layer_section(
                other,
                display=120,
                render=120,
                frames=1,
                histogram_line=histogram(b33=1),
            ),
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError, "distinct_positive_blast_identities"
        ):
            surface.parse_surfaceflinger_timestats(text)

    def test_rejects_duplicate_rate_pair_for_same_identity(self):
        section = layer_section(
            IDENTITY,
            frames=1,
            histogram_line=histogram(b33=1),
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError, "duplicate_layer_rate_pair"
        ):
            surface.parse_surfaceflinger_timestats(capture(section, section))

    def test_parent_gameactivity_cannot_substitute_for_blast(self):
        parent = layer_section(
            "app.lsx4.android/app.lsx4.android.GameActivity "
            "08-21 18:50:00.832#50079",
            frames=60,
            average=60,
            histogram_line=histogram(b16=60),
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError, "no_positive_game_blast_layer"
        ):
            surface.parse_surfaceflinger_timestats(capture(parent))

    def test_expected_parent_identity_is_rejected_even_when_blast_exists(self):
        text = capture(
            layer_section(
                IDENTITY,
                frames=1,
                histogram_line=histogram(b33=1),
            )
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError, "expected_identity_is_not_game_blast"
        ):
            surface.parse_surfaceflinger_timestats(
                text,
                expected_layer_identity=(
                    "app.lsx4.android/app.lsx4.android.GameActivity "
                    "08-21 18:50:00.832#50079"
                ),
            )

    def test_rejects_histogram_sample_count_mismatch(self):
        text = capture(
            layer_section(
                IDENTITY,
                frames=3,
                histogram_line=histogram(b33=2),
            )
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError,
            "present2present_histogram_sum_mismatch",
        ):
            surface.parse_surfaceflinger_timestats(text)

    def test_rejects_different_bucket_layouts_across_rate_pairs(self):
        first = layer_section(
            IDENTITY,
            frames=1,
            histogram_line=histogram(b33=1),
        )
        second = layer_section(
            IDENTITY,
            display=120,
            render=120,
            frames=1,
            histogram_line=histogram(b33=1).replace(" 200ms=0", ""),
        )
        with self.assertRaisesRegex(
            surface.SurfaceFlingerParseError, "histogram_bucket_layout_mismatch"
        ):
            surface.parse_surfaceflinger_timestats(capture(first, second))

    def test_cli_emits_canonical_json_and_fail_closed_error_json(self):
        valid = capture(
            layer_section(
                IDENTITY,
                frames=1,
                histogram_line=histogram(b33=1),
            )
        )
        invalid = valid.replace("33ms=1", "33ms=0")
        with tempfile.TemporaryDirectory() as directory:
            valid_path = pathlib.Path(directory) / "valid.txt"
            invalid_path = pathlib.Path(directory) / "invalid.txt"
            valid_path.write_text(valid, encoding="utf-8")
            invalid_path.write_text(invalid, encoding="utf-8")

            accepted = subprocess.run(
                [sys.executable, str(MODULE_PATH), str(valid_path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            rejected = subprocess.run(
                [sys.executable, str(MODULE_PATH), str(invalid_path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertEqual(json.loads(accepted.stdout)["layer"]["totalFrames"], 1)
        self.assertEqual(rejected.returncode, 2)
        error = json.loads(rejected.stderr)
        self.assertFalse(error["accepted"])
        self.assertEqual(error["reason"], "present2present_histogram_sum_mismatch")


if __name__ == "__main__":
    unittest.main()
