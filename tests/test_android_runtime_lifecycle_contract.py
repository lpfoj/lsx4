import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
JAVA_ROOT = ROOT / "android-app/app/src/main/java/app/lsx4/android"
CONTROLLER = JAVA_ROOT / "RuntimePauseController.java"
DRAWER_CONTROLLER = JAVA_ROOT / "GameDrawerController.java"
ACTIVITY = JAVA_ROOT / "MainActivity.java"
JAVA_BRIDGE = JAVA_ROOT / "RuntimeBridge.java"
JNI_BRIDGE = ROOT / "android-app/app/src/main/cpp/runtime_bridge.cpp"
RUNTIME = ROOT / "src/executor/native_runtime_api.cpp"
PS1_RUNTIME = ROOT / "src/executor/ps1_classic_runtime.cpp"
PS5_RUNTIME = ROOT / "src/executor/ps5_desktop/runtime_api.cpp"
RETIRING_CORE = ROOT / "src/executor/dynamic_translation/retiring_execution_core.cpp"
DEBUG_STATE_H = ROOT / "funnel-arm/src/core/debug_state.h"
DEBUG_STATE_CPP = ROOT / "funnel-arm/src/core/debug_state.cpp"
SIGNALS = ROOT / "funnel-arm/src/core/signals.cpp"


class AndroidRuntimeLifecycleContractTest(unittest.TestCase):
    def test_activity_owns_independent_pause_reasons(self):
        source = ACTIVITY.read_text(encoding="utf-8")
        for token in (
            "REASON_ACTIVITY_BACKGROUND",
            "REASON_ACTIVITY_STOPPED",
            "REASON_WINDOW_FOCUS_LOST",
            "REASON_GAME_MENU",
            "terminateRuntimePauseController(\"game-exit\")",
            "terminateRuntimePauseController(\"activity-destroy\")",
        ):
            self.assertIn(token, source)
        self.assertIn(
            "setRuntimePauseReason(RuntimePauseController.REASON_GAME_MENU, active,",
            source,
        )

    def test_drawer_has_one_back_owner_and_no_boolean_animation_state(self):
        source = ACTIVITY.read_text(encoding="utf-8")
        self.assertIn('gameBackCallback = () -> requestGameDrawerToggle("on-back-invoked")', source)
        self.assertIn("Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU && !pressed", source)
        self.assertIn("return true;", source[source.index("if (systemBackEvent)") :][:900])
        self.assertNotIn("gameMenuOpen", source)
        self.assertNotIn("gameMenuEdge.setOnTouchListener", source)
        self.assertNotIn("gameMenuPanel.setOnTouchListener", source)
        self.assertIn(
            "Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU",
            source,
        )
        self.assertIn("return super.dispatchKeyEvent(event);", source)

    def test_bridge_is_explicit_and_backend_capability_is_fail_closed(self):
        java = JAVA_BRIDGE.read_text(encoding="utf-8")
        bridge = JNI_BRIDGE.read_text(encoding="utf-8")
        runtime = RUNTIME.read_text(encoding="utf-8")
        ps1_runtime = PS1_RUNTIME.read_text(encoding="utf-8")
        ps5_runtime = PS5_RUNTIME.read_text(encoding="utf-8")
        retiring_core = RETIRING_CORE.read_text(encoding="utf-8")
        signals = SIGNALS.read_text(encoding="utf-8")
        debug_h = DEBUG_STATE_H.read_text(encoding="utf-8")
        debug_cpp = DEBUG_STATE_CPP.read_text(encoding="utf-8")

        self.assertIn("native int setRuntimePaused(boolean paused)", java)
        self.assertIn("native String runtimeLifecycleStatus()", java)
        self.assertIn("executor_lsx4_runtime_set_paused", bridge)
        self.assertIn("executor_lsx4_ps5_runtime_set_paused", bridge)
        self.assertIn("result=unsupported", bridge)
        self.assertIn("executor_lsx4_runtime_set_paused", runtime)
        self.assertIn("executor_lsx4_runtime_lifecycle_status", runtime)
        self.assertIn("if (!ps1_active && !Core::HasSleepSignalHandler())", runtime)
        self.assertIn("(void)Core::Signals::Instance();", runtime)
        self.assertIn("executor_lsx4_install_jit_signal_handlers();", runtime)
        self.assertIn("TryPauseGuestThreads", debug_h)
        self.assertIn("Core::HasSleepSignalHandler()", debug_cpp)
        self.assertIn("sigaction(SIGSLEEP, nullptr, &action)", signals)
        self.assertIn("guest_threads_resumed.wait", debug_cpp)
        self.assertIn("guest_threads_resumed.notify_all", debug_cpp)
        self.assertIn("state_changed.wait", ps1_runtime)
        self.assertIn("deadline = Clock::now()", ps1_runtime)
        self.assertIn("executor_lsx4_ps5_runtime_set_paused", ps5_runtime)
        self.assertIn("entry-vblank-hle", ps5_runtime)
        self.assertIn("Do not replay the vblanks skipped", ps5_runtime)
        self.assertIn("executor_lsx4_ps5_runtime_wait_if_paused", retiring_core)
        self.assertIn("ClearRuntimePauseForShutdown();", ps5_runtime)

    def test_reason_mask_semantics_execute_without_android(self):
        javac = shutil.which("javac")
        java = shutil.which("java")
        if not javac or not java:
            self.skipTest("JDK is unavailable")

        harness = textwrap.dedent(
            """
            package app.lsx4.android;

            public final class RuntimePauseControllerHarness {
                public static void main(String[] args) {
                    StringBuilder transitions = new StringBuilder();
                    RuntimePauseController controller = new RuntimePauseController(paused -> {
                        transitions.append(paused ? 'P' : 'R');
                        return 0;
                    });
                    controller.setRuntimeReady(true);
                    controller.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
                    controller.setReason(RuntimePauseController.REASON_WINDOW_FOCUS_LOST, false);
                    controller.setReason(RuntimePauseController.REASON_GAME_MENU, true);
                    controller.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, true);
                    controller.setReason(RuntimePauseController.REASON_GAME_MENU, false);
                    if (!controller.shouldBePaused()) throw new AssertionError("background lost");
                    controller.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
                    controller.setReason(RuntimePauseController.REASON_GAME_MENU, true);
                    controller.terminate();
                    controller.setReason(RuntimePauseController.REASON_GAME_MENU, false);
                    controller.setRuntimeReady(false);
                    controller.setRuntimeReady(true);
                    if (!"PRPRP".contentEquals(transitions)) {
                        throw new AssertionError(transitions.toString());
                    }
                    if ((controller.reasonMask() & RuntimePauseController.REASON_TERMINATING) == 0) {
                        throw new AssertionError("terminating reason lost");
                    }

                    StringBuilder resync = new StringBuilder();
                    RuntimePauseController second = new RuntimePauseController(paused -> {
                        resync.append(paused ? 'P' : 'R');
                        return 0;
                    });
                    second.setRuntimeReady(true);
                    second.setRuntimeReady(false);
                    second.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
                    second.setReason(RuntimePauseController.REASON_WINDOW_FOCUS_LOST, false);
                    second.setRuntimeReady(true);
                    if (!"PR".contentEquals(resync)) {
                        throw new AssertionError("ready resync=" + resync);
                    }

                    transitions.setLength(0);
                    RuntimePauseController lifecycle = new RuntimePauseController(paused -> {
                        transitions.append(paused ? 'P' : 'R');
                        return 0;
                    });
                    lifecycle.setRuntimeReady(true);
                    lifecycle.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
                    lifecycle.setReason(RuntimePauseController.REASON_WINDOW_FOCUS_LOST, false);
                    transitions.setLength(0);
                    StringBuilder drawerEvents = new StringBuilder();
                    GameDrawerController drawer = new GameDrawerController(
                            new GameDrawerController.Host() {
                                @Override
                                public void setDrawerPauseReason(boolean active, String source) {
                                    lifecycle.setReason(
                                            RuntimePauseController.REASON_GAME_MENU, active);
                                }

                                @Override
                                public void beginOpen(int generation, String source) {
                                    drawerEvents.append('O').append(generation);
                                }

                                @Override
                                public void beginClose(int generation, String source) {
                                    drawerEvents.append('C').append(generation);
                                }
                            });
                    drawer.requestOpen("swipe");
                    drawer.requestOpen("synthetic-click-after-swipe");
                    if (!"P".contentEquals(transitions) ||
                            !"O1".contentEquals(drawerEvents)) {
                        throw new AssertionError("duplicate open=" + transitions + "/" + drawerEvents);
                    }
                    lifecycle.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, true);
                    drawer.requestClose("back");
                    if (!drawer.requiresPause() || !"P".contentEquals(transitions)) {
                        throw new AssertionError("pause released before close animation");
                    }
                    if (!drawer.completeClose(drawer.generation(), "animation")) {
                        throw new AssertionError("close lease rejected");
                    }
                    if (!"P".contentEquals(transitions)) {
                        throw new AssertionError("drawer resumed background runtime");
                    }
                    lifecycle.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
                    if (!"PR".contentEquals(transitions)) {
                        throw new AssertionError("aggregate resume=" + transitions);
                    }

                    transitions.setLength(0);
                    drawerEvents.setLength(0);
                    drawer.requestOpen("first");
                    int firstOpen = drawer.generation();
                    drawer.requestClose("first-close");
                    int staleClose = drawer.generation();
                    drawer.requestOpen("reopen");
                    int currentOpen = drawer.generation();
                    if (drawer.completeClose(staleClose, "stale") ||
                            drawer.completeOpen(firstOpen) ||
                            !drawer.completeOpen(currentOpen)) {
                        throw new AssertionError("stale animation lease accepted");
                    }
                }
            }
            """
        )
        with tempfile.TemporaryDirectory() as raw_temp:
            package = pathlib.Path(raw_temp) / "app/lsx4/android"
            package.mkdir(parents=True)
            shutil.copy2(CONTROLLER, package / CONTROLLER.name)
            shutil.copy2(DRAWER_CONTROLLER, package / DRAWER_CONTROLLER.name)
            harness_path = package / "RuntimePauseControllerHarness.java"
            harness_path.write_text(harness, encoding="utf-8")
            subprocess.run(
                [javac, str(package / CONTROLLER.name),
                 str(package / DRAWER_CONTROLLER.name), str(harness_path)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                [java, "-cp", raw_temp, "app.lsx4.android.RuntimePauseControllerHarness"],
                check=True,
                capture_output=True,
                text=True,
            )


if __name__ == "__main__":
    unittest.main()
