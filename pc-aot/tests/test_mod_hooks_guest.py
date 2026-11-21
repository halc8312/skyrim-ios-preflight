"""Compose MOD hook stages with authored units and hash tables; the pinned lift only when supplied."""
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import classic_skse_guest as classic  # noqa: E402
import mod_hooks_guest as hooks  # noqa: E402

spec = importlib.util.spec_from_file_location("title_assembly_for_hooks", ROOT / "scripts/assemble_title_ipa.py")
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)


class GroupTests(unittest.TestCase):
    def test_parses_staged_groups(self):
        self.assertEqual(hooks.parse_groups("fuz_menu"), 1)
        self.assertEqual(hooks.parse_groups("fuz_menu,fuz_world,vm_lifecycle"), 7)
        self.assertEqual(hooks.parse_groups(" chargen_precache , fuz_menu "), 9)
        self.assertEqual(hooks.parse_groups("camera_observe"), 16)
        self.assertEqual(hooks.parse_groups("camera_render_observe"), 1024)
        self.assertEqual(hooks.parse_groups("camera_cull_observe"), 8192)
        self.assertEqual(hooks.parse_groups("ece_tri_observe"), 2048)
        self.assertEqual(hooks.parse_groups("ece_tri_observe,ece_nose_observe"), 18432)
        self.assertEqual(hooks.parse_groups("vm_lifecycle,highheel_pex_observe"), 32772)
        self.assertEqual(hooks.parse_groups("save_lifecycle"), 64)
        self.assertEqual(hooks.parse_groups("fresh_load,save_lifecycle,vm_lifecycle"), 4164)
        self.assertEqual(hooks.parse_groups("loading_menu_observe"), 128)
        self.assertEqual(hooks.parse_groups("inventory_fit"), 256)
        self.assertEqual(hooks.parse_groups("loading_menu_observe,safety_load"), 640)
        self.assertEqual(hooks.group_names(11), ["fuz_menu", "fuz_world", "chargen_precache"])

    def test_rejects_unknown_duplicate_and_incomplete_lists(self):
        for text, message in (("", "comma-separated"), ("fuz_menu,", "comma-separated"),
                              ("fuz_menu,fuz_menu", "twice"), ("fuz", "Unknown"),
                              ("fuz_world", "list both"), ("safety_load", "list both"),
                              ("fresh_load,save_lifecycle", "needs"), ("ece_nose_observe", "needs"),
                              ("highheel_pex_observe", "needs")):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, message):
                hooks.parse_groups(text)

    def test_bits_and_headers_match_the_abi_and_the_assembly(self):
        header = (ROOT / "ios/private-abi/PcGuestModHooksABI.h").read_text()
        for name, bit in hooks.GROUPS.items():
            self.assertIn(f"PC_GUEST_MOD_HOOK_{name.upper()} = {bit}u", header)
        self.assertEqual({name: path.relative_to(ROOT).parent.as_posix() for name, path in hooks.HEADERS.items()},
                         assembly.MOD_HOOK_HEADERS)
        staged = sum(bit for name, bit in hooks.GROUPS.items() if name in hooks.STAGES)
        self.assertEqual(staged & ~assembly.MOD_HOOK_GROUPS_DEFINED, 0)
        self.assertEqual(set(hooks.STAGES), set(hooks.GROUPS))


class ComposeTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "private/stage"
        self.gen = self.root / "gen"
        self.gen.mkdir(parents=True)
        self.state = dict(hooks.reviewed_generation(), generation_finished=True)
        (self.root / "generation.json").write_text(json.dumps(self.state))
        self.units = {name: self.gen / name for name in ("exe_1.c", "exe_2.c", "exe_3.c", "exe_table.c")}
        for name, path in self.units.items():
            path.write_bytes(b"/* authored " + name.encode() + b" */\n")
        self.sources = list(self.units.values())
        self.output = self.root / "build/mod-hook-sources"

    def stages(self, **groups):
        return mock.patch.dict(hooks.STAGES, groups, clear=True)

    def test_combines_groups_and_accepts_identical_shared_units(self):
        menu = lambda root: {"exe_1.c": b"menu 1\n", "exe_2.c": b"shared\n"}  # noqa: E731
        world = lambda root: {"exe_2.c": b"shared\n", "exe_3.c": b"world 3\n"}  # noqa: E731
        before = {p: p.read_bytes() for p in self.sources}
        with self.stages(fuz_menu=menu, fuz_world=world):
            result, staged = hooks.prepare_sources(self.sources, self.output, 3, self.state, self.root)
        self.assertEqual([p.parent for p in result], [self.output] * 3 + [self.gen])
        self.assertEqual(staged, {name: hashlib.sha256(data).hexdigest() for name, data in
                                  (("exe_1.c", b"menu 1\n"), ("exe_2.c", b"shared\n"), ("exe_3.c", b"world 3\n"))})
        self.assertEqual((self.output / "exe_3.c").read_bytes(), b"world 3\n")
        self.assertEqual({p: p.read_bytes() for p in self.sources}, before)
        # Only the selected group runs.
        with self.stages(fuz_menu=menu, fuz_world=lambda root: self.fail("not selected")):
            result, staged = hooks.prepare_sources(self.sources, self.output, 1, self.state, self.root)
        self.assertEqual(set(staged), {"exe_1.c", "exe_2.c"})
        self.assertEqual(result[2], self.units["exe_3.c"])

    def test_rejects_differing_shared_units(self):
        with self.stages(fuz_menu=lambda root: {"exe_2.c": b"menu\n"},
                         fuz_world=lambda root: {"exe_2.c": b"world\n"}):
            with self.assertRaisesRegex(ValueError, "fuz_menu and fuz_world stages produce different"):
                hooks.prepare_sources(self.sources, self.output, 3, self.state, self.root)
        self.assertFalse(self.output.exists())

    def test_rejects_overlap_with_the_classic_preparation(self):
        renamed = self.root / "build/classic-sources/exe_1.c"
        renamed.parent.mkdir(parents=True)
        renamed.write_bytes(b"void f_8F9720_guest(CPU *c) {\n}\n")
        sources = [renamed, *self.sources[1:]]
        with self.stages(fuz_menu=lambda root: {"exe_1.c": b"staged\n"}):
            with self.assertRaisesRegex(ValueError, "already changed by the Classic"):
                hooks.prepare_sources(sources, self.output, 1, self.state, self.root)
        # An original unit that still holds a Classic entry is refused as well.
        self.units["exe_2.c"].write_bytes(b"void f_A60200(CPU *c) {\n}\n")
        with self.stages(fuz_menu=lambda root: {"exe_2.c": b"staged\n"}):
            with self.assertRaisesRegex(ValueError, "Classic entry point"):
                hooks.prepare_sources(self.sources, self.output, 1, self.state, self.root)
        self.assertFalse(self.output.exists())

    def test_rejects_missing_duplicate_and_unknown_inputs(self):
        with self.stages(fuz_menu=lambda root: {"exe_9.c": b"staged\n"}):
            with self.assertRaisesRegex(ValueError, "missing from the guest sources"):
                hooks.prepare_sources(self.sources, self.output, 1, self.state, self.root)
            with self.assertRaisesRegex(ValueError, "appears twice"):
                hooks.prepare_sources(self.sources + self.sources[:1], self.output, 1, self.state, self.root)
            with self.assertRaisesRegex(ValueError, "No MOD hook groups"):
                hooks.prepare_sources(self.sources, self.output, 0, self.state, self.root)
            with self.assertRaisesRegex(ValueError, "no reviewed stage"):
                hooks.prepare_sources(self.sources, self.output, 4, self.state, self.root)
        self.assertFalse(self.output.exists())

    def test_loading_menu_stage_requires_the_exact_classic_entry_rename(self):
        name = hooks.LOADING_MENU_UNIT
        original = self.gen / name
        entry = b"\nvoid f_A60200(CPU *c) {\n}\n"
        renamed = entry.replace(b"f_A60200(", b"f_A60200_guest(")
        original.write_bytes(entry)
        classic_source = self.root / "classic-sources" / name
        classic_source.parent.mkdir()
        classic_source.write_bytes(renamed)
        staged = b"/* authored loading observation */"
        with self.stages(loading_menu_observe=lambda root: {name: staged + entry}):
            result, _ = hooks.prepare_sources([classic_source], self.output, 128, self.state, self.root)
            self.assertEqual(result[0].read_bytes(), staged + renamed)
            for source_edit in (entry, renamed + b"/* other source edit */"):
                classic_source.write_bytes(source_edit)
                with self.assertRaisesRegex(ValueError, "exactly the reviewed Classic entry rename"):
                    hooks.prepare_sources([classic_source], self.output, 128, self.state, self.root)

    def test_rejects_other_generations(self):
        with self.stages(fuz_menu=lambda root: self.fail("must not stage")):
            for key, value in hooks.reviewed_generation().items():
                changed = dict(self.state, **{key: "other" if value is not True else False})
                with self.subTest(key=key), self.assertRaisesRegex(ValueError, "reviewed Classic generation"):
                    hooks.prepare_sources(self.sources, self.output, 1, changed, self.root)
            (self.root / "generation.json").write_text(json.dumps(dict(self.state, extra=1)))
            with self.assertRaisesRegex(ValueError, "changed while preparing"):
                hooks.prepare_sources(self.sources, self.output, 1, self.state, self.root)

    def test_vm_lifecycle_stage_with_an_authored_unit(self):
        revert, load, delete = (site[3] for site in hooks.VM_SITES)
        reload_call = b"    esp -= 4; M32(esp) = 0x8D6E1Bu; SPILL(); f_8D5DF0(c); RELOAD();\n"
        unit = (b'#include "funcs_exe.h"\n\nvoid f_8D6990(CPU *c) {\n' + revert + b"}\n"
                b"\nvoid f_8D69C0(CPU *c) {\n    eax = 1;\n" + load + b"}\n"
                b"\nvoid f_8D6550(CPU *c) {\n" + delete + b"}\n"
                b"\nvoid f_8D6D00(CPU *c) {\n" + reload_call + b"}\n")
        (self.gen / "exe_199.c").write_bytes(unit)
        sources = [*self.sources, self.gen / "exe_199.c"]
        authored = hashlib.sha256(unit).hexdigest()
        site = ("exe_199.c", authored, "f_8D6D00", reload_call, "b", b"    skyrim_vm_host_before_revert(2u);\n")
        sites = [(u, authored, *rest) for u, _sha, *rest in hooks.VM_SITES]
        with mock.patch.object(hooks, "VM_SCRIPT_RELOAD_SITE", site):
            self.assertEqual(hooks.parse_groups("vm_lifecycle"), 4)
            with self.assertRaisesRegex(ValueError, "reviewed Classic VM lift"):
                hooks.prepare_sources(sources, self.output, 4, self.state, self.root)
            with mock.patch.object(hooks, "VM_SITES", sites):
                result, staged = hooks.prepare_sources(sources, self.output, 4, self.state, self.root)
                data = result[-1].read_bytes()
                self.assertTrue(data.startswith(hooks.VM_HOOK_DECLARATIONS))
                self.assertIn(b"    skyrim_vm_host_before_revert(1u);\n" + revert, data)
                self.assertIn(load + b"    skyrim_vm_host_after_load((uint8_t)c->r[0] != 0);\n", data)
                self.assertIn(b"    skyrim_vm_host_before_revert(2u);\n" + reload_call, data)
                self.assertNotIn(delete, data)
                self.assertIn(b"    f_8D4C40(c);\n    skyrim_vm_host_form_deleted(c, skyrim_deleted_vm, "
                              b"skyrim_deleted_handle);\n", data)
                # A call outside its reviewed function, or a repeated call, is refused.
                for changed in (unit.replace(b"void f_8D6990", b"void f_8D6991"), unit + load):
                    (self.gen / "exe_199.c").write_bytes(changed)
                    moved = [(u, hashlib.sha256(changed).hexdigest(), *rest) for u, _sha, *rest in sites]
                    moved_site = (site[0], moved[0][1], *site[2:])
                    with self.subTest(changed=len(changed)), mock.patch.object(hooks, "VM_SITES", moved), \
                            mock.patch.object(hooks, "VM_SCRIPT_RELOAD_SITE", moved_site), \
                            self.assertRaises(ValueError):
                        hooks.prepare_sources(sources, self.output, 4, self.state, self.root)

    def test_real_precache_stage_with_an_authored_unit(self):
        precache = hooks.load(ROOT / "experiments/classic-chargen-precache", "test_chargen_precache")
        unit = (precache.PRELUDE + precache.WARM_ENTRY + b"    fixture_warm(c);\n}\n"
                + b"\nvoid f_886930(CPU *c) {\n" + precache.SETUP + precache.WARM + precache.FOLLOWUP + b"\n}\n")
        (self.gen / precache.MENU_UNIT).write_bytes(unit)
        sources = [*self.sources, self.gen / precache.MENU_UNIT]
        with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
            hooks.prepare_sources(sources, self.output, 8, self.state, self.root)
        with mock.patch.object(precache, "MENU_HASH", hashlib.sha256(unit).hexdigest()):
            result, staged = hooks.prepare_sources(sources, self.output, 8, self.state, self.root)
        data = result[-1].read_bytes()
        self.assertEqual(list(staged), [precache.MENU_UNIT])
        self.assertIn(precache.WARM_ENTRY + precache.SKIP_WARM, data)
        self.assertIn(precache.WARM, data)
        self.assertEqual(data.replace(precache.HOOK, b"", 1).replace(precache.SKIP_WARM, b"", 1), unit)


class OwnedLiftTests(unittest.TestCase):
    def test_all_stages_compose_and_parse_with_the_pinned_lift(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation.")
        compiler = shutil.which(os.environ.get("CC", "clang"))
        if not compiler:
            self.skipTest("A C compiler is required.")
        root = Path(generation).resolve()
        state = json.loads((root / "generation.json").read_text(encoding="utf-8"))
        classic.validate_generation(state)
        groups = hooks.parse_groups("fuz_menu,fuz_world,vm_lifecycle,chargen_precache,chargen_slider,camera_observe,"
                                    "loading_menu_observe,inventory_fit,safety_load,camera_render_observe,"
                                    "ece_tri_observe,save_lifecycle,fresh_load,camera_cull_observe,ece_nose_observe,"
                                    "highheel_pex_observe")
        originals = sorted((root / "gen").glob("exe_*.c"))
        before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}
        with tempfile.TemporaryDirectory(prefix="mod-hooks-") as scratch:
            work = Path(scratch)
            sources = classic.prepare_sources(originals, work / "classic-sources")
            result, staged = hooks.prepare_sources(sources, work / "mod-hook-sources", groups, state, root)
            self.assertTrue({"exe_95.c", "exe_100.c", "exe_101.c", "exe_189.c", "exe_35.c", "exe_188.c",
                             "exe_66.c", "exe_106.c", "exe_107.c", "exe_185.c", "exe_186.c", "exe_199.c", "exe_259.c",
                             "exe_262.c", "exe_275.c", "exe_359.c"} <= set(staged))
            frame = (work / "mod-hook-sources" / "exe_107.c").read_bytes()
            self.assertEqual(frame.count(b"skyrim_camera_cull_guest_notify(SkyrimCameraCullPrepare,"), 1)
            self.assertEqual(frame.count(b"skyrim_camera_observe_render(edi, eax, edx)"), 1)
            tri = hooks.load(ROOT / "experiments/classic-racemenu-apply", "stage_ece_tri")
            tri_stage = (work / "mod-hook-sources" / tri.UNIT).read_bytes()
            tri_original = tri.stage((root / "gen" / tri.UNIT).read_bytes())
            from_start = tri_original.index(b"void f_59F270(CPU *c) {")
            from_end = tri_original.index(b"\n}\n", from_start) + len(b"\n}\n")
            self.assertIn(tri_original[from_start:from_end], tri_stage)
            self.assertEqual(tri_stage.count(b"ece_nose_parse(c,"), 1)
            reader = (root / "gen" / "exe_261.c").read_bytes()
            start, end = hooks.function_span(reader, "f_A580A0")
            caller = re.search(rb"LOOSE_READ_RETURN\s*=\s*(0x[0-9A-F]+)",
                               (ROOT / "ios/runtime-port/mod_ece_tri_host.c").read_bytes()).group(1)
            calls = re.findall(rb"0x106B284u\)\); esp -= 4; M32\(esp\) = (0x[0-9A-F]+)u", reader[start:end])
            self.assertEqual(calls, [caller])
            self.assertIn(b"skyrim_ece_nose_guest_selected &&", (work / "mod-hook-sources" / "exe_185.c").read_bytes())
            with self.assertRaisesRegex(ValueError, "unreviewed earlier stage"):
                hooks.stage_ece_nose_observe(root, {tri.UNIT: tri_original + b"// unrelated edit\n"})
            menu = (work / "mod-hook-sources" / "exe_262.c").read_bytes()
            self.assertEqual(menu.count(b"skyrim_inventory_fit_enabled()"), 1)
            self.assertEqual(menu.count(b"skyrim_loading_menu_observe(0x"), 2)
            self.assertEqual(menu.count(b"void f_A60200_guest(CPU *c)"), 1)
            selected = (work / "mod-hook-sources" / "exe_103.c").read_bytes()
            self.assertEqual(selected.count(b"skyrim_load_notify(c, SkyrimFreshFinish"), 1)
            self.assertEqual(selected.count(b"skyrim_save_notify_begin(c,"), 1)
            startup = (work / "mod-hook-sources" / "exe_107.c").read_bytes()
            notice = (b"    if ((uint8_t)eax) (void)skyrim_load_notify(c, SkyrimFreshPluginsReady, 0, "
                      b"TL32((uint32_t)(0x12E2CB8u)), 0);\n")
            self.assertEqual(startup.count(notice), 1)
            self.assertIn(b"    esp -= 4; M32(esp) = 0x699350u; SPILL(); f_4434A0(c); RELOAD();\n" + notice, startup)
            start, end = hooks.function_span(startup, "f_699100")
            original = (root / "gen/exe_107.c").read_bytes()
            old_start, old_end = hooks.function_span(original, "f_699100")
            self.assertEqual(startup[start:end].replace(notice, b"", 1), original[old_start:old_end])
            linker = (work / "mod-hook-sources" / "exe_346.c").read_bytes()
            self.assertEqual(linker.count(b"skyrim_highheel_pex_begin(c, "), 1)
            self.assertEqual(linker.count(b"skyrim_highheel_pex_linked(c, "), 1)
            self.assertEqual(linker.count(b"M32(esp) = 0xC690DAu; SPILL(); f_C34F20(c); RELOAD();"), 2)
            vm = (work / "mod-hook-sources" / "exe_199.c").read_bytes()
            self.assertEqual(vm.count(b"skyrim_load_notify(c, SkyrimFreshVmBegin"), 1)
            self.assertEqual(vm.count(b"skyrim_vm_host_after_load((uint8_t)"), 1)
            runtime = root / "source/runtime"
            for path in (p for p in result if p.name in staged):
                with self.subTest(unit=path.name):
                    subprocess.run([compiler, "-std=gnu11", "-fsyntax-only", "-w", "-DGAME_SKYRIM",
                                    "-I", str(runtime), "-I", str(root / "gen"), "-I", str(hooks.SEAMS),
                                    "-I", str(hooks.SAVE_SEAMS), "-I", str(ROOT / "ios/private-abi"),
                                    str(path)], check=True, timeout=300)
            library = ROOT / "ios/private-abi/pc_guest_mod_hooks_library.c"
            subprocess.run([compiler, "-std=gnu11", "-fsyntax-only", "-DSKYRIM_IOS_CLASSIC_SKSE=1",
                            f"-DPC_GUEST_MOD_HOOKS_COMPILED={groups}u", "-I", str(runtime),
                            "-I", str(ROOT / "ios/private-abi"), "-I", str(hooks.SEAMS), str(library)],
                           check=True, timeout=60)
        self.assertEqual({p: hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}, before)


if __name__ == "__main__":
    unittest.main()
