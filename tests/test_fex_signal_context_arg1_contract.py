import os
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src" / "executor" / "fex_backend_abi_v3.h").read_text(
    encoding="utf-8"
)
NATIVE = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
EXCEPTION = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "exception.cpp"
).read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def adapter_source() -> Path | None:
    candidates = []
    configured = os.environ.get("LSX4_FEX2608_ADAPTER_SOURCE_DIR")
    if configured:
        candidates.append(Path(configured) / "src" / "fex_context_bridge.cpp")
    candidates.extend(
        (
            ROOT.parent
            / "refs"
            / "PS4Run"
            / "fex-2608-adapter-v3"
            / "src"
            / "fex_context_bridge.cpp",
            ROOT
            / "refs"
            / "PS4Run"
            / "fex-2608-adapter-v3"
            / "src"
            / "fex_context_bridge.cpp",
        )
    )
    return next((candidate for candidate in candidates if candidate.is_file()), None)


class FexSignalContextArg1ContractTests(unittest.TestCase):
    def test_flag_has_a_new_layout_neutral_bit(self):
        self.assertIn(
            "LSX4_FEX_SIGNAL_CONTEXT_ARG1 = UINT32_C(1) << 2", ABI
        )
        delivery = section(
            ABI,
            "typedef struct Lsx4FexGuestSignalDeliveryV3",
            "} Lsx4FexGuestSignalDeliveryV3;",
        )
        self.assertIn("uint32_t delivery_flags", delivery)
        self.assertNotIn("context_arg1", delivery.lower())

    def test_runtime_wrapper_forwards_only_the_explicit_context_arg1_flag(self):
        wrapper = section(
            NATIVE,
            "executor_lsx4_android_queue_guest_signal_for_thread_flags(",
            "executor_lsx4_android_queue_guest_signal_for_thread_context(",
        )
        self.assertIn(
            "delivery_flags & ~LSX4_FEX_SIGNAL_CONTEXT_ARG1", wrapper
        )
        self.assertIn("if (!RuntimeGuestFexActive())", wrapper)
        self.assertIn("delivery.delivery_flags = delivery_flags", wrapper)
        self.assertIn("api->queue_guest_signal", wrapper)

    def test_only_live_mono_kernel_exception_fex_route_sets_the_flag(self):
        pthread_kill = section(
            EXCEPTION,
            "s32 PS4_SYSV_ABI posix_pthread_kill",
            "static constexpr std::array<int, 33> signals",
        )
        route = section(
            pthread_kill,
            "if (live_mono_sigusr1 && !deliver_mono_sigusr1",
            "if (mono_coop && named_mono_thread",
        )
        self.assertIn("ExecutorFexSignalApiActive()", route)
        self.assertIn("HandlerKinds[sig] == 3", route)
        self.assertIn(
            "executor_lsx4_android_queue_guest_signal_for_thread_flags(", route
        )
        self.assertIn("LSX4_FEX_SIGNAL_CONTEXT_ARG1", route)
        self.assertIn(
            "else if (!kernel_exception_context_arg1", route
        )

        install = section(
            EXCEPTION,
            "int PS4_SYSV_ABI sceKernelInstallExceptionHandler",
            "int PS4_SYSV_ABI sceKernelRemoveExceptionHandler",
        )
        assignment = install.index("HandlerKinds[signum] = 3")
        self.assertNotIn(
            "ExecutorJitSignalParityActive", install[: assignment + 1]
        )

    def test_adapter_swaps_arguments_only_when_flag_is_present(self):
        source_path = adapter_source()
        if source_path is None:
            self.skipTest("pinned FEX-2608 adapter source is not present")
        adapter = source_path.read_text(encoding="utf-8")
        deliver = section(adapter, "int32_t DeliverSignal(", "int32_t ProcessQueuedSignals(")
        self.assertIn(
            "delivery.delivery_flags & LSX4_FEX_SIGNAL_CONTEXT_ARG1", deliver
        )
        self.assertIn(
            "context_arg1 && (action.action_flags & kOrbisSaSigInfo) == 0",
            deliver,
        )

        flagged = section(deliver, "if (context_arg1) {", "} else if")
        self.assertIn("REG_RSI] = context_address", flagged)
        self.assertIn("REG_RDX] = siginfo_address", flagged)

        standard = section(
            deliver,
            "else if ((action.action_flags & kOrbisSaSigInfo) != 0) {",
            "} else {",
        )
        self.assertIn("REG_RSI] = siginfo_address", standard)
        self.assertIn("REG_RDX] = context_address", standard)

    def test_adapter_rejects_unknown_flags_but_accepts_context_arg1(self):
        source_path = adapter_source()
        if source_path is None:
            self.skipTest("pinned FEX-2608 adapter source is not present")
        adapter = source_path.read_text(encoding="utf-8")
        queue = section(adapter, "int32_t QueueGuestSignal(", "int32_t QueryRuntime(")
        allowed = section(queue, "delivery->delivery_flags & ~(", ")) != 0")
        self.assertIn("LSX4_FEX_SIGNAL_USE_EXPLICIT_CONTEXT", allowed)
        self.assertIn("LSX4_FEX_SIGNAL_SYNCHRONOUS", allowed)
        self.assertIn("LSX4_FEX_SIGNAL_CONTEXT_ARG1", allowed)


if __name__ == "__main__":
    unittest.main()
