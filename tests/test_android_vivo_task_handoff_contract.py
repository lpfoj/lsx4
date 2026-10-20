"""Android wiring around the separately executed GameSessionTransitions model."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android-app/app/src/main/java/app/lsx4/android"
MAIN = (JAVA / "MainActivity.java").read_text(encoding="utf-8")
BROKER = (JAVA / "GameSessionProvider.java").read_text(encoding="utf-8")


class AndroidVivoTaskHandoffContract(unittest.TestCase):
    def test_broker_replaces_inherited_clear_task_flags_for_every_device(self):


        launch = BROKER.split("@Override public void launch(Intent target, long token)", 1)[1].split(
            "@Override public void stop(long token)", 1
        )[0]
        self.assertIn("Intent intent = new Intent(target)", launch)
        self.assertIn("intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP)", launch)
        self.assertLess(launch.index("intent.setFlags("), launch.index("startActivity(intent)"))
        self.assertNotIn("FLAG_ACTIVITY_CLEAR_TASK", launch)
        self.assertNotIn("Build.MANUFACTURER", launch)
        self.assertIn("token == 0 ? MainActivity.class : GameActivity.class", launch)

    def test_library_and_automation_launches_both_enter_the_broker(self):
        launcher = (JAVA / "LauncherUi.java").read_text(encoding="utf-8")
        self.assertIn("activity.routeIntentToGameActivity(intent)", launcher)
        route = MAIN.split("void routeIntentToGameActivity", 1)[1].split(
            "static int crossProcessTaskFlags", 1
        )[0]
        self.assertIn("GameSessionProvider.launch(this, routed)", route)
        self.assertNotIn("startActivity(", route)
        self.assertIn('transitions.request(new Intent(target), true)', BROKER)
        self.assertIn('Intent library = libraryIntent(getContext());', BROKER)
        self.assertIn('transitions.request(library, false)', BROKER)

    def test_unregistered_launch_cannot_reuse_a_live_game_process(self):
        watchdog = re.search(r"private synchronized void expireUnregisteredLaunch\(.*?^    }",
                             BROKER, re.DOTALL | re.MULTILINE).group(0)
        self.assertIn("transitions.waitingForRegistration(token)", watchdog)
        self.assertIn("process.uid == android.os.Process.myUid()", watchdog)
        self.assertIn('(getContext().getPackageName() + ":game").equals(process.processName)', watchdog)
        self.assertIn("android.os.Process.killProcess(process.pid)", watchdog)
        waiting, confirmed_dead = watchdog.split("if (found)", 1)[1].split("} else {", 1)
        self.assertIn("expireUnregisteredLaunch(token)", waiting)
        self.assertNotIn("transitions.died", waiting)
        self.assertIn("transitions.died(token)", confirmed_dead)


if __name__ == "__main__":
    unittest.main()
