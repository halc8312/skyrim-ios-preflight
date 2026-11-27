"""Pin the original SkyUI-facing registration behavior in the staged runtime.

This suite covers Skydream's menu, key and ModEvent path, including its current
handling of script revert. Companion-record replay and control-event lifetimes
have independent tests. Version values, INI lookup and the single
IOS_CLASSIC_RELEASE line are pinned by test_classic_skse_ini.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
sys.path.insert(0, str(PORT))
import prepare_classic_skse as classic


class SkyUIRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        names = (*classic.PINNED, "skse_translation.c", classic.PRESENT_FILE, classic.INPUT_FILE)
        if not (cls.vendor / "runtime").is_dir():
            raise unittest.SkipTest("pinned Skydream source required")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.stage = Path(cls.tmp.name) / "runtime"
        cls.stage.mkdir()
        for name in names:
            shutil.copyfile(cls.vendor / "runtime" / name, cls.stage / name)
        classic.prepare(cls.vendor, cls.stage)
        cls.system = (cls.stage / "skse_system.c").read_text()

    def change_registration(self):
        start = self.system.index("static void change_registration(")
        return self.system[start:self.system.index("static void pump_events(", start)]

    def test_registering_twice_for_one_handle_replaces_the_first_registration(self):
        body = self.change_registration()
        remove = body.index("*link=r->next;")
        add = body.index("if(add) {")
        self.assertLess(remove, add, "matching entries must be removed before the new one is added")
        # One registration per handle, kind and menu or event name (or key).
        self.assertIn("r->handle==handle && r->kind==kind", body)
        self.assertIn("!strcmp(r->name,name)", body)
        self.assertEqual(body.count("registrations=r;"), 1)
        # Every added entry takes exactly one handle reference and every removal gives one back.
        self.assertEqual(body.count("handle_ref(c,handle,1)"), 1)
        self.assertEqual(body.count("handle_ref(c,r->handle,0)"), 1)

    def test_menu_registration_starts_from_the_current_menu_state(self):
        # A menu that is already open at registration must not deliver a stale OnMenuOpen.
        self.assertIn("r->active=kind==0 ? !!skse_menu_view(name) : 0;", self.change_registration())

    def test_script_revert_does_not_clear_registrations(self):
        for name in ("skse_system.c", "skse_compat.c"):
            text = (self.stage / name).read_text()
            self.assertNotRegex(text, r"(?i)revert[^\n]*registrations|registrations[^\n]*revert", name)
        body = self.change_registration()
        self.assertEqual(body.count("registrations=r;"), 1)
        self.assertNotIn("Revert", body)

    def test_events_are_delivered_from_a_copy_so_callbacks_may_register_again(self):
        start = self.system.index("static void pump_events(")
        end = self.system.index("void skse_ui_pump(", start)
        body = self.system[start:end]
        self.assertIn("Copy deliveries before calling the VM", body)
        self.assertLess(body.index("pthread_mutex_unlock(&event_lock);"), body.index("send_event(c,&d->r,d->event,count,values);"))

    def test_phone_density_is_applied_only_with_an_exact_live_config_pair(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler required")
        self.assertIn('#include "classic_skyui_density.c.in"', self.system)
        self.assertIn("if (!density_invoke(c, cmd, view, target, gfx))", self.system)
        experiment = ROOT / "experiments/classic-skyui-density"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "skyui-layout"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(PORT), "-I", str(experiment),
                            str(experiment / "skyui_density.c"), str(experiment / "test_skyui_density.c"),
                            "-o", str(output)], check=True, timeout=60)
            subprocess.run([str(output)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
