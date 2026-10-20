import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIVERPOOL = (
    ROOT / "funnel-arm" / "src" / "video_core" / "amdgpu" / "liverpool.cpp"
).read_text(encoding="utf-8")


class Pm4Type0PacketContractTest(unittest.TestCase):
    def test_cached_plan_is_fully_validated_before_it_can_be_returned(self):
        validate = LIVERPOOL.split("Liverpool::ValidatePm4DescriptorPlan", 1)[1].split(
            "Liverpool::FindPm4DescriptorPlan", 1
        )[0]
        find = LIVERPOOL.split("Liverpool::FindPm4DescriptorPlan", 1)[1].split(
            "Liverpool::AdmitPm4DescriptorPlanBuild", 1
        )[0]

        self.assertIn("for (const auto& descriptor : plan.packets)", validate)
        self.assertIn("ValidatePm4PacketDescriptor(descriptor", validate)
        self.assertIn("return offset_words == stream.size()", validate)
        self.assertIn("for (u32 way = 0; way < Pm4DescriptorCacheWays; ++way)", find)
        rejection = find.split("!ValidatePm4DescriptorPlan(*line.plan, stream)", 1)[1]
        self.assertLess(rejection.index("continue;"), rejection.index("return line.plan;"))
        self.assertLess(
            find.index("ValidatePm4DescriptorPlan(*line.plan, stream)"),
            find.index("return line.plan;"),
        )
        for forbidden in ("WaitIdle", "Finish()", "Drain", "std::scoped_lock", "memcpy"):
            self.assertNotIn(forbidden, validate + find)

    def test_stale_header_recovery_is_absent_and_trusted_loops_are_o1(self):
        for forbidden in (
            "recovered_header_kind",
            "EXECUTOR_PM4_HEADER_RECOVERED",
            'return "set-sh-bank"',
            'return "set-sh-pair"',
            'return "set-context-pair"',
        ):
            self.assertNotIn(forbidden, LIVERPOOL)

        ce = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessCeUpdate", 1)[1]
        ce = ce.split("const u32 type =", 1)[0]
        graphics = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessGraphics", 1)[1]
        graphics = graphics.split("const u32 type =", 1)[0]
        compute = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessCompute", 1)[1]
        compute = compute.split("const u32 packet_type =", 1)[0]

        for loop, size_name in ((ce, "ccb.size()"), (graphics, "dcb.size()"), (compute, "acb.size()")):
            self.assertNotIn("ValidatePm4PacketDescriptor", loop)
            self.assertIn("descriptor.offset_words == offset_words", loop)
            self.assertIn(f"descriptor.packet_words <= {size_name}", loop)
            self.assertIn("cached_descriptor == nullptr", loop)
            invariant_failure = loop.split("if (cached_descriptor == nullptr)", 1)[1]
            self.assertIn("state->abort = true", invariant_failure)
            self.assertLess(
                invariant_failure.index("state->abort = true"),
                invariant_failure.index("break;"),
            )

    def test_equal_coarse_keys_select_only_the_matching_late_layout(self):


        stream_a = ((3, 0x10, 3), (3, 0x20, 3), (3, 0x76, 3), (3, 0x3C, 4))
        stream_b = ((3, 0x10, 3), (3, 0x20, 3), (3, 0x79, 2), (3, 0x48, 5))

        def coarse_key(stream):
            return sum(packet[2] for packet in stream), stream[:2]

        def plan_matches(plan, stream):
            offset = 0
            for expected, actual in zip(plan, stream):
                if expected != actual:
                    return False
                offset += expected[2]
            return len(plan) == len(stream) and offset == sum(p[2] for p in stream)

        self.assertEqual(coarse_key(stream_a), coarse_key(stream_b))
        side_effects = []
        selected = next(
            (
                plan
                for plan in (stream_a, stream_a, stream_b, stream_a)
                if plan_matches(plan, stream_b)
            ),
            None,
        )
        self.assertEqual(side_effects, [])
        self.assertIs(selected, stream_b)
        side_effects.append(selected[2][1])
        self.assertEqual(side_effects, [0x79])
        self.assertIsNone(
            next((plan for plan in (stream_a,) * 4 if plan_matches(plan, stream_b)), None)
        )

    def test_cond_exec_cannot_land_beyond_the_validated_stream(self):
        cond_exec = LIVERPOOL.split("case PM4ItOpcode::CondExec:", 1)[1].split(
            "default:", 1
        )[0]
        self.assertIn("static_cast<u64>(packet_words)", cond_exec)
        self.assertIn("skip_words > dcb.size()", cond_exec)
        overflow = cond_exec.split("skip_words > dcb.size()", 1)[1]
        self.assertLess(overflow.index("state->abort = true"), overflow.index("break;"))
        self.assertIn(
            "NextPacket(dcb, static_cast<std::size_t>(skip_words))", cond_exec
        )

    def test_graphics_parser_consumes_complete_type0_with_bounded_register_write(self):
        graphics = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessGraphics", 1)[1]
        graphics = graphics.split("Liverpool::Task Liverpool::ProcessCompute", 1)[0]

        sizing = graphics.split("size_t packet_words = 1;", 1)[1].split(
            "switch (type)", 1
        )[0]
        self.assertIn("type == 0", sizing)
        self.assertIn("header->type0.NumWords()) + 1", sizing)
        self.assertIn("packet_words > dcb.size()", sizing)

        type0 = graphics.split("case 0:", 1)[1].split("case 2:", 1)[0]
        self.assertIn("ExecutorSafeRegArrayWrite", type0)
        self.assertIn("Regs::NumRegs", type0)
        self.assertIn("NotifyGraphicsStateChanged", type0)
        self.assertIn("dcb = NextPacket(dcb, packet_words)", type0)
        self.assertIn("continue;", type0)
        self.assertNotIn("state->abort = true", type0)
        self.assertNotIn("Unsupported PM4 type 0", type0)

    def test_descriptor_plan_keeps_complete_type0_with_its_payload(self):
        build = LIVERPOOL.split("Liverpool::BuildPm4DescriptorPlan", 1)[1].split(
            "Liverpool::ValidatePm4PacketDescriptor", 1
        )[0]
        validate = LIVERPOOL.split("Liverpool::ValidatePm4PacketDescriptor", 1)[
            1
        ].split("Liverpool::FindPm4DescriptorPlan", 1)[0]

        self.assertIn("type == 0", build)
        type0_build = build.split("if (type == 0)", 1)[1].split(
            "else if (type == 3)", 1
        )[0]
        self.assertIn("header->type0.NumWords()) + 1", type0_build)
        self.assertIn("expanded_words > stream.size() - offset", type0_build)
        self.assertIn("packet_words = static_cast<u32>(expanded_words)", type0_build)
        self.assertIn("type == 0", validate)
        type0_validate = validate.split("if (type == 0)", 1)[1].split(
            "if (type == 2)", 1
        )[0]
        self.assertIn("header->type0.NumWords()) + 1", type0_validate)
        self.assertIn("expanded_words <= remaining_stream.size()", type0_validate)
        self.assertIn("descriptor.packet_words == expanded_words", type0_validate)

    def test_root_plan_is_captured_before_enqueue_and_never_reparsed_late(self):
        capture = LIVERPOOL.split("Liverpool::CapturePm4DescriptorPlan", 1)[1].split(
            "Liverpool::FindPm4DescriptorPlan", 1
        )[0]
        submit = LIVERPOOL.split("void Liverpool::SubmitGfx", 1)[1].split(
            "Liverpool::ExecutorCaptureGnmcapFrame", 1
        )[0]
        graphics = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessGraphics", 1)[1]
        bind = graphics.split("const auto bind_descriptor_plan =", 1)[1].split(
            "bind_descriptor_plan();", 1
        )[0]

        self.assertIn("static thread_local CaptureCache cache", capture)
        self.assertIn("constexpr u32 CaptureCacheSets = 64", capture)
        self.assertIn("ValidatePm4DescriptorPlan(*line.plan, stream)", capture)
        self.assertIn("BuildPm4DescriptorPlan(key, stream, malformed)", capture)
        self.assertIn("saw_layout_mismatch = true", capture)
        self.assertIn("!saw_layout_mismatch || allow_layout_replacement", capture)
        self.assertEqual(
            submit.count("BeginFexDeferredWriteSubmitBoundary()"), 1
        )
        self.assertLess(
            submit.index("BeginFexDeferredWriteSubmitBoundary()"),
            submit.index("CopyGuestPm4Words("),
        )
        self.assertLess(
            submit.rindex("EndFexDeferredWriteSubmitBoundary(epoch_boundary)"),
            submit.index("ProcessGraphics(dcb, ccb"),
        )
        self.assertLess(
            submit.index("CapturePm4DescriptorPlan("),
            submit.index("ProcessGraphics(dcb, ccb"),
        )
        self.assertEqual(submit.count("std::vector<u32> refreshed;"), 1)
        self.assertIn("if (malformed || layout_mismatch)", submit)
        self.assertIn("owned_dcb = std::move(refreshed)", submit)
        second_capture = submit.split("owned_dcb = std::move(refreshed)", 1)[1]
        self.assertIn("&malformed, &layout_mismatch, true", second_capture)
        self.assertIn("[EXECUTOR_PM4_CAPTURE_REJECTED]", submit)
        rejection = submit.split("[EXECUTOR_PM4_CAPTURE_REJECTED]", 1)[0]
        rejection = rejection.rsplit("if (", 1)[1]
        self.assertIn("!recopied || malformed", rejection)
        self.assertNotIn("layout_mismatch", rejection)
        self.assertIn("std::move(captured_root_plan)", submit)

        captured = bind.split("if (captured_root_plan)", 1)[1].split(
            "if (!VideoCore::IsManagedVulkanDriverOptimization()", 1
        )[0]
        self.assertIn("descriptor_plan = std::move(captured_root_plan)", captured)
        self.assertIn("return;", captured)
        self.assertNotIn("ValidatePm4", captured)
        self.assertNotIn("CopyGuestPm4Words", bind)
        for forbidden in (
            "std::this_thread::yield",
            "WaitIdle",
            "Finish()",
            "std::scoped_lock",
            "atomic_thread_fence",
            "recovered_header_kind",
        ):
            self.assertNotIn(forbidden, capture + bind + submit)

    def test_layout_mismatch_recopies_then_accepts_a_valid_replacement(self):



        cached = ((3, 0x10, 3), (3, 0x20, 3), (3, 0x76, 3), (3, 0x3C, 4))
        replacement = ((3, 0x10, 3), (3, 0x20, 3), (0, 0, 2), (3, 0x48, 5))

        def coarse_key(stream):
            return sum(packet[2] for packet in stream), stream[:2]

        def structurally_valid(stream):
            return all(packet_type in (0, 2, 3) and words >= 1 for packet_type, _, words in stream)

        self.assertEqual(coarse_key(cached), coarse_key(replacement))
        cache = [cached]
        first_copy = replacement
        mismatch = cache[0] != first_copy
        self.assertTrue(mismatch)
        self.assertTrue(structurally_valid(first_copy))
        self.assertEqual(cache, [cached], "the provisional first copy must not replace the cache")

        recopied = replacement
        self.assertTrue(structurally_valid(recopied))
        cache[0] = recopied
        self.assertEqual(cache[0], replacement)
        self.assertEqual(cache[0][2][0], 0, "a complete TYPE0 replacement remains valid")

    def test_eop_scanner_skips_type0_payload(self):
        scanner = LIVERPOOL.split("void ExecutorEopTraceHleSubmit", 1)[1].split(
            "void ExecutorEopTraceEqTrigger", 1
        )[0]
        self.assertIn("type == 0", scanner)
        self.assertIn("advance = header->type0.NumWords() + 1", scanner)


if __name__ == "__main__":
    unittest.main()
