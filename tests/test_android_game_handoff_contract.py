from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ElementTree

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android-app/app/src/main/java/app/lsx4/android"
MAIN = (JAVA / "MainActivity.java").read_text(encoding="utf-8")
BROKER = (JAVA / "GameSessionProvider.java").read_text(encoding="utf-8")
CONNECTION = (JAVA / "GameSessionConnection.java").read_text(encoding="utf-8")


def method(text: str, marker: str) -> str:
    match = re.search(re.escape(marker) + r".*?^    }", text, re.DOTALL | re.MULTILINE)
    if match is None:
        raise AssertionError(f"Missing method: {marker}")
    return match.group(0)


class AndroidGameHandoffContract(unittest.TestCase):
    def test_single_top_automation_finishes_ui_after_broker_handoff(self):
        body = method(MAIN, "protected void onNewIntent")
        self.assertLess(body.index("routeIntentToGameActivity(intent)"),
                        body.index('retireProcessAfterTaskHandoff(this, "launcher-automation-resume")'))

    def test_launcher_finish_keeps_settings_owner_and_broker_alive(self):
        body = method(MAIN, "static void retireProcessAfterTaskHandoff")
        self.assertIn("activity.finish();", body)
        self.assertNotIn("finishAndRemoveTask", body)
        self.assertNotIn("killProcess", body)
        self.assertNotIn("System.exit", body)
        manifest = ElementTree.parse(ROOT / "android-app/app/src/main/AndroidManifest.xml")
        android = "{http://schemas.android.com/apk/res/android}"
        for name in ("GameSessionProvider", "SettingsProvider"):
            provider = next(p for p in manifest.findall(".//provider")
                            if p.get(android + "name").endswith("." + name))
            self.assertEqual(provider.get(android + "exported"), "false")
            self.assertIsNone(provider.get(android + "process"))

    def test_game_holds_a_stable_broker_association_until_process_death(self):
        body = method(CONNECTION, "GameSessionConnection(Activity activity, Runnable stop)")
        self.assertIn("acquireContentProviderClient(", body)
        self.assertNotIn("acquireUnstableContentProviderClient", body)
        self.assertIn('broker.call("register", null, registration)', body)
        failure = body.split("catch (RemoteException | RuntimeException failure)", 1)[1]
        self.assertIn("broker.close()", failure)
        self.assertNotIn("broker.close()", body.split("catch (", 1)[0])
        self.assertLess(MAIN.index("new GameSessionConnection(this, this::finishGameProcess)"),
                        MAIN.index("migrateLegacyPrivateHome();"))

    def test_binder_death_releases_handoff_only_for_the_owning_session(self):
        registration = method(BROKER, "public synchronized Bundle call(")
        self.assertIn("Binder.getCallingUid() != android.os.Process.myUid()", registration)
        self.assertIn("binder.linkToDeath", registration)
        died = method(BROKER, "private synchronized void sessionDied(")
        self.assertLess(died.index("session != dead || sessionToken != token"),
                        died.index("transitions.died(token)"))
        stop = method(BROKER, "private synchronized void stopSession(")
        self.assertIn("retiring.transact(STOP, data, null, IBinder.FLAG_ONEWAY)", stop)
        self.assertNotIn("startActivity", stop)
        self.assertNotIn("transitions.died", stop)

    def test_apk_update_wins_over_an_older_staged_runtime(self):
        body = method(MAIN, "private File selectRuntimeFileForLaunch()")
        self.assertIn("getPackageInfo(getPackageName(), 0).lastUpdateTime", body)
        self.assertIn("staged.lastModified() < packageUpdateTime", body)
        stale_branch = body.split("staged.lastModified() < packageUpdateTime", 1)[1].split("return staged;", 1)[0]
        self.assertIn("return packaged;", stale_branch)

    def test_handoff_uses_fresh_title_preferences_without_replaying_old_snapshots(self):
        route = method(MAIN, "void routeIntentToGameActivity")
        self.assertLess(route.index("String titleId"),
                        route.index("SettingsActivity.prefsForGame(this, titleId)"))
        self.assertLess(route.index("ProcessSafePreferences.refresh(this)"),
                        route.index("SettingsActivity.prefsForGame(this, titleId)"))
        self.assertIn("routed.removeExtra(EXTRA_SETTINGS_SNAPSHOT)", route)
        self.assertIn("routed.removeExtra(EXTRA_SETTINGS_SNAPSHOT_GENERATION)", route)
        self.assertIn("GameSessionProvider.launch(this, routed)", route)
        self.assertNotIn("startActivity(", route)
        self.assertNotIn("captureCrossProcessSettings", MAIN)
        self.assertNotIn("applyCrossProcessSettingsSnapshot", MAIN)
        refresh = method(MAIN, "private void synchronizeGameMenuPreferences(")
        self.assertIn("ProcessSafePreferences.refresh(this)", refresh)
        self.assertNotIn(".edit()", refresh)


if __name__ == "__main__":
    unittest.main()
