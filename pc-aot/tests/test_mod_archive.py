"""Real MOD archive reader, data-root detection and launch view; authored files only."""
import errno
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class ModArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler required; run on WSL/Mac")
        cls.tmp = tempfile.TemporaryDirectory(prefix="mod-archive-")
        cls.dir = Path(cls.tmp.name)
        cls.bin = cls.dir / "driver"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, "-I", str(PORT), str(PORT / "mod_archive.c"), str(PORT / "mod_deploy.c"),
                        str(PORT / "mod_plugins.c"), str(ROOT / "tests/native/test_mod_archive.c"),
                        "-lz", "-o", str(cls.bin)], check=True, timeout=120)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_driver(self, *args):
        out = subprocess.run([str(self.bin), *map(str, args)], check=True, text=True, capture_output=True, timeout=60)
        return out.stdout

    def make_zip(self, name, files, **kw):
        path = self.dir / name
        with zipfile.ZipFile(path, "w", compression=kw.get("compression", zipfile.ZIP_DEFLATED)) as z:
            for arc, data in files.items():
                if isinstance(data, zipfile.ZipInfo):
                    z.writestr(data, b"x")
                else:
                    z.writestr(arc, data)
        return path

    def test_extract_with_wrapper_folder_and_crc(self):
        big = os.urandom(300_000) + b"a" * 400_000
        z = self.make_zip("wrapped.zip", {
            "Cool Mod v1/Data/textures/Armor/A.dds": big,
            "Cool Mod v1/Data/CoolMod.esp": b"TES4",
            "Cool Mod v1/Data/CoolMod.bsa": b"",
            "Cool Mod v1/readme.txt": b"hi",
        })
        self.assertIn("root 1 [Cool Mod v1/Data/] installer 0", self.run_driver("root", z))
        dest = self.dir / "out1"; dest.mkdir()
        out = self.run_driver("extract", z, dest)
        self.assertIn("extract textures/Armor/A.dds ok", out)
        self.assertEqual((dest / "textures/Armor/A.dds").read_bytes(), big)
        self.assertFalse((dest / "readme.txt").exists())
        # Existing files are never replaced.
        self.assertIn("extract CoolMod.esp write", self.run_driver("extract", z, dest))

    def test_stored_zip64_and_installer_detection(self):
        path = self.dir / "z64.zip"
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
            with z.open("meshes/x.nif", "w", force_zip64=True) as f:
                f.write(b"nif" * 1000)
            z.writestr("fomod/ModuleConfig.xml", "<config/>")
        self.assertIn("root 1 [] installer 1", self.run_driver("root", path))
        dest = self.dir / "out2"; dest.mkdir()
        self.assertIn("extract meshes/x.nif ok", self.run_driver("extract", path, dest))

    def test_installer_path_is_a_complete_fomod_config_name(self):
        archive = self.make_zip("installer-backups.zip", {
            "Backup/FOMOD/ModuleConfig.xml.bak": b"backup",
            "Data/interface/a.txt": b"authored",
        })
        self.assertIn("root 1 [Data/] installer 0", self.run_driver("root", archive))
        archive = self.make_zip("installer-only.zip", {
            "Wrapper/FOMOD/MODULECONFIG.XML": b"<config/>",
            "Wrapper/Blue/payload.txt": b"option",
        })
        self.assertIn("root 0 [] installer 1", self.run_driver("root", archive))

    def test_source_project_does_not_override_sibling_data(self):
        bundle = self.make_zip("with-source.zip", {
            "Package/DaTa/Scripts/Example.pex": b"authored compiled script",
            "Package/Src/skse/example.cpp": b"source",
            "Package/Src/skse/example.h": b"header",
            "Package/Src/skse/Example.vcxproj": b"project",
            "Package/Example.exe": b"other platform executable",
        })
        self.assertIn("root 1 [Package/DaTa/] installer 0", self.run_driver("root", bundle))
        dest = self.dir / "with-source"
        dest.mkdir()
        self.assertIn("extract Scripts/Example.pex ok", self.run_driver("extract", bundle, dest))
        self.assertEqual((dest / "Scripts/Example.pex").read_bytes(), b"authored compiled script")
        self.assertEqual(set(dest.iterdir()), {dest / "Scripts"})

    def test_data_aliases_score_as_one_root(self):
        bundle = self.make_zip("mixed-case-data.zip", {
            "Output/Data/Meshes/first.nif": b"first mesh",
            "output/DATA/meshes/behaviors/second.hkx": b"second behavior",
            "Output/Examples/meshes/other.nif": b"unselected example",
            "Output/Examples/meshes/another.nif": b"unselected example",
        })
        root = self.run_driver("root", bundle)
        self.assertIn("root 1 [output/data/] installer 0", root.lower())
        dest = self.dir / "mixed-case"
        dest.mkdir()
        self.run_driver("extract", bundle, dest)
        files = {p.relative_to(dest).as_posix().lower(): p.read_bytes()
                 for p in dest.rglob("*") if p.is_file()}
        self.assertEqual(files, {
            "meshes/first.nif": b"first mesh",
            "meshes/behaviors/second.hkx": b"second behavior",
        })

    def test_generated_assets_with_sibling_tool_files(self):
        # Authored bytes following the LE BodySlide and FNIS output layout.
        body = "meshes/actors/character/character assets/femalebody_0.nif"
        behavior = "meshes/actors/character/characters female/defaultfemale.hkx"
        extra = "meshes/actors/character/behaviors/authored_generate_only.hkx"
        bundle = self.make_zip("generated-assets.zip", {
            f"Tool Bundle/Data/{body}": b"generated body",
            f"Tool Bundle/data/{behavior}": b"generated behavior",
            f"Tool Bundle/Data/{extra}": b"generated extra",
            "Tool Bundle/CalienteTools/BodySlide/BodySlide.exe": b"tool",
            "Tool Bundle/CalienteTools/BodySlide/SliderSets/example.osp": b"preset",
            "Tool Bundle/CalienteTools/BodySlide/ShapeData/example.osd": b"shape",
        })
        self.assertIn("root 1 [tool bundle/data/] installer 0",
                      self.run_driver("root", bundle).lower())
        data = self.dir / "generated-data"
        data.mkdir()
        self.run_driver("extract", bundle, data)

        def write_package(name, files):
            directory = self.dir / name
            for path, content in files.items():
                target = directory / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
            return directory

        def files_in(view):
            root = view / "Data"
            return {p.relative_to(root).as_posix().lower(): p.read_bytes()
                    for p in root.rglob("*") if p.is_file()}

        game = self.dir / "generated-game"
        (game / "Data/Meshes").mkdir(parents=True)
        first_base = {body: b"first base body", behavior: b"first base behavior"}
        second_base = {body: b"second base body", behavior: b"second base behavior"}
        base = write_package("generated-base", first_base)
        other_base = write_package("generated-other-base", second_base)
        view = self.dir / "generated-view"
        conflicts = self.dir / "generated-conflicts.tsv"
        other_view = self.dir / "generated-other-view"
        other_conflicts = self.dir / "generated-other-conflicts.tsv"
        result = self.run_driver("deploy", game, view, conflicts, "generated", base, data)
        self.assertIn("deploy 0  matches 1", result)
        self.assertEqual(files_in(view), {
            body: b"generated body", behavior: b"generated behavior", extra: b"generated extra",
        })
        self.assertIn(f"{body}\t0\t1", conflicts.read_text().lower().splitlines())
        self.assertIn(f"{behavior}\t0\t1", conflicts.read_text().lower().splitlines())
        result = self.run_driver("deploy", game, other_view, other_conflicts, "other-base", other_base)
        self.assertIn("deploy 0  matches 1", result)
        self.assertEqual(files_in(other_view), second_base)
        result = self.run_driver("deploy", game, view, conflicts, "base-only", base)
        self.assertIn("deploy 0  matches 1", result)
        self.assertEqual(files_in(view), first_base)
        self.assertEqual(files_in(other_view), second_base)
        self.assertEqual(files_in(game), {})

    def test_game_assets_at_root_stay_with_their_tools(self):
        bundle = self.make_zip("game-assets-and-tools.zip", {
            "Bundle/meshes/body.nif": b"body",
            "Bundle/CalienteTools/BodySlide/Preset.osp": b"preset",
            "Bundle/Data/meshes/example.nif": b"example",
        })
        self.assertIn("root 1 [Bundle/] installer 0", self.run_driver("root", bundle))

    def test_unsafe_names_are_refused(self):
        for i, name in enumerate(["../evil.esp", "/abs.esp", "C:/x.esp", "a/../../b.esp", "con?.esp"]):
            z = self.make_zip(f"bad{i}.zip", {name: b"x"})
            self.assertIn("status unsafe-path", self.run_driver("list", z), name)
        z = self.make_zip("dup.zip", {"Textures/a.dds": b"1", "textures/A.DDS": b"2"})
        self.assertIn("status duplicate", self.run_driver("list", z))
        info = zipfile.ZipInfo("link.esp"); info.create_system = 3; info.external_attr = (0o120777 << 16)
        z = self.make_zip("link.zip", {"link.esp": info})
        self.assertIn("status link", self.run_driver("list", z))
        z = self.dir / "bz.zip"
        with zipfile.ZipFile(z, "w", compression=zipfile.ZIP_BZIP2) as f:
            f.writestr("a.esp", b"x" * 100)
        self.assertIn("status unsupported-method", self.run_driver("list", z))
        (self.dir / "notzip.zip").write_bytes(b"hello world, not a zip file at all")
        self.assertIn("status not-zip", self.run_driver("list", self.dir / "notzip.zip"))

    def test_backslashes_and_non_utf8_names(self):
        path = self.dir / "names.zip"
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("Data\\Textures\\b.dds", b"1")
            info = zipfile.ZipInfo("x.txt"); z.writestr(info, b"2")
        raw = path.read_bytes().replace(b"x.txt", b"\x93\xfa.tx")  # Shift-JIS bytes, no UTF-8 flag
        path.write_bytes(raw)
        out = self.run_driver("list", path)
        self.assertIn("status ok", out)
        self.assertIn("skipped 1", out)
        self.assertIn("file Data/Textures/b.dds", out)

    def test_no_game_data(self):
        z = self.make_zip("docs.zip", {"readme.txt": b"x", "images/a.png": b"y"})
        self.assertIn("root 0", self.run_driver("root", z))

    def test_racemenu_preset_stays_in_the_selected_profile_overlay(self):
        source = self.dir / "chosen.jslot"
        payload = b'{"authoredFixture":true}'
        source.write_bytes(payload)
        package = self.dir / "preset-package"
        package.mkdir()
        for bad in ("other.slot", ".jslot", "../outside.jslot", "absolute\\outside.jslot", "C:alias.jslot"):
            self.assertNotIn("preset 0\n", self.run_driver("preset", source, bad, package))
        self.assertEqual(list(package.iterdir()), [])
        self.assertFalse((self.dir / "outside.jslot").exists())

        name = "Authored Appearance.JSLOT"
        relative = Path("SKSE/Plugins/CharGen/Presets") / name
        self.assertEqual(self.run_driver("preset", source, name, package), "preset 0\n")
        self.assertEqual((package / relative).read_bytes(), payload)
        replacement = self.dir / "changed.jslot"
        replacement.write_bytes(b"different")
        self.assertNotIn("preset 0\n", self.run_driver("preset", replacement, name, package))
        self.assertEqual((package / relative).read_bytes(), payload)

        game = self.dir / "preset-game"
        (game / "Data").mkdir(parents=True)
        enabled, empty = self.dir / "preset-enabled", self.dir / "preset-disabled"
        for key in ("first-launch", "rebuilt-view"):
            self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, enabled,
                          self.dir / "preset-conflicts.tsv", key, package))
            self.assertEqual((enabled / "Data" / relative).read_bytes(), payload)
        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, empty,
                      self.dir / "other-preset-conflicts.tsv", "another-profile"))
        self.assertFalse((empty / "Data" / relative).exists())
        self.assertEqual(list((game / "Data").iterdir()), [])
        self.assertEqual(source.read_bytes(), payload)

    def test_ece_slot_import_is_private_and_uses_only_the_numbered_slot(self):
        """Verify storage for a compatible ECE runtime without invoking the DLL or game."""
        source = self.dir / "authored_original.slot"
        source.write_bytes(b"authored ECE slot")
        inbox = self.dir / "ece-inbox"
        inbox.mkdir()
        staged = inbox / "first.slot"
        shutil.copyfile(source, staged)
        active, other = self.dir / "ece-active", self.dir / "ece-other"
        active.mkdir()
        other.mkdir()
        for bad in ("legacy.slot", "_0_commonTriRaces.slot", "_01_commonTriRaces.slot",
                    "_41_commonTriRaces.slot", "_410_commonTriRaces.slot",
                    "_+1_commonTriRaces.slot", "_1_customRace.slot", "_1_commonTriRaces.jslot",
                    "../_1_commonTriRaces.slot", "dir\\_1_commonTriRaces.slot",
                    "_1_commonTriRaces.slot.tmp", "_1_commonTriRaces.slot "):
            with self.subTest(name=bad):
                self.assertEqual(self.run_driver("ece-slot", staged, bad, active), f"ece-slot {errno.EINVAL} 0\n")
        self.assertEqual(list(active.iterdir()), [])
        selected_link = inbox / "from-link.slot"
        selected_link.symlink_to(source)
        self.assertEqual(self.run_driver("ece-slot", selected_link, "_2_commonTriRaces.slot", active),
                         f"ece-slot {errno.EINVAL} 2\n")
        self.assertEqual(list(active.iterdir()), [])

        saves = Path("Documents/My Games/Skyrim")
        cme = active / saves / "cme_Save"
        cme.mkdir(parents=True)
        self.assertEqual(self.run_driver("ece-slot", staged, "_1_COMMONTRIRACES.SLOT", active), "ece-slot 0 1\n")
        staged.unlink()
        target = cme / "_1_commonTriRaces.slot"
        self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertEqual(list(other.iterdir()), [])
        target.write_bytes(b"independent profile data")
        self.assertEqual(source.read_bytes(), b"authored ECE slot")
        self.assertFalse((active / "Data").exists())

        staged = inbox / "second.slot"
        staged.write_bytes(b"different profile slot")
        self.assertEqual(self.run_driver("ece-slot", staged, "_1_commonTriRaces.slot", active),
                         f"ece-slot {errno.EEXIST} 1\n")
        self.assertEqual(target.read_bytes(), b"independent profile data")
        self.assertEqual(self.run_driver("ece-slot", staged, "_1_commonTriRaces.slot", other), "ece-slot 0 1\n")
        staged.unlink()
        self.assertEqual((other / saves / "CME_save" / target.name).read_bytes(), b"different profile slot")

        outside = self.dir / "ece-outside"
        outside.mkdir()
        link_profile = self.dir / "ece-linked"
        (link_profile / saves).mkdir(parents=True)
        (link_profile / saves / "CME_save").symlink_to(outside, target_is_directory=True)
        staged = inbox / "slot40.slot"
        staged.write_bytes(b"maximum slot")
        end = "_40_commonTriRaces.slot"
        self.assertNotEqual(self.run_driver("ece-slot", staged, end, link_profile), "ece-slot 0 40\n")
        self.assertEqual(list(outside.iterdir()), [])
        self.assertEqual(self.run_driver("ece-slot", staged, end, active), "ece-slot 0 40\n")
        self.assertEqual((cme / end).read_bytes(), b"maximum slot")
        self.assertFalse((other / saves / "CME_save" / end).exists())

        occupied = other / saves / "CME_save" / "_40_COMMONTRIRACES.SLOT"
        occupied.write_bytes(b"older case spelling")
        staged.unlink()
        staged.write_bytes(b"new maximum slot")
        self.assertEqual(self.run_driver("ece-slot", staged, end, other), f"ece-slot {errno.EEXIST} 40\n")
        self.assertEqual(occupied.read_bytes(), b"older case spelling")
        self.assertEqual(sum(p.name.lower() == end.lower() for p in occupied.parent.iterdir()), 1)

    def test_deploy_overlay_priority_protection_and_reuse(self):
        game = self.dir / "game"; (game / "Data/Textures").mkdir(parents=True)
        (game / "Data/Skyrim.esm").write_bytes(b"orig-esm")
        (game / "Data/Textures/Sky.dds").write_bytes(b"orig-sky")
        (game / "TESV_unpacked.exe").write_bytes(b"exe")
        a = self.dir / "pkgA"; (a / "textures").mkdir(parents=True)
        (a / "textures/sky.DDS").write_bytes(b"A-sky")
        (a / "textures/new.dds").write_bytes(b"A-new")
        (a / "skyrim.esm").write_bytes(b"evil")
        b = self.dir / "pkgB"; (b / "TEXTURES").mkdir(parents=True)
        (b / "TEXTURES/new.dds").write_bytes(b"B-new")
        (b / "B.esp").write_bytes(b"TES4")
        view = self.dir / "view"; conflicts = self.dir / "conflicts.tsv"
        out = self.run_driver("deploy", game, view, conflicts, "key-1", a, b)
        self.assertIn("deploy 0  matches 1", out)
        self.assertIn("signature-stable 1", out)
        self.assertEqual((view / "Data/Textures/Sky.dds").read_bytes(), b"A-sky")
        self.assertEqual((view / "Data/Textures/new.dds").read_bytes(), b"B-new")
        self.assertEqual((view / "Data/Skyrim.esm").read_bytes(), b"orig-esm")
        self.assertEqual((view / "Data/B.esp").read_bytes(), b"TES4")
        self.assertFalse((view / "Data/textures").exists() and (view / "Data/Textures").exists()
                         and not os.path.samefile(view / "Data/textures", view / "Data/Textures"))
        self.assertEqual((game / "Data/Textures/Sky.dds").read_bytes(), b"orig-sky")
        rows = conflicts.read_text().splitlines()
        self.assertIn("Textures/Sky.dds\t-1\t0", rows)
        self.assertIn("Textures/new.dds\t0\t1", rows)
        self.assertIn("Skyrim.esm\t0\t-1", rows)  # The protected original wins.
        (view / "TESV_unpacked.exe").write_bytes(b"isolated test write")
        (view / "Data/B.esp").write_bytes(b"isolated package write")
        self.assertEqual((game / "TESV_unpacked.exe").read_bytes(), b"exe")
        self.assertEqual((b / "B.esp").read_bytes(), b"TES4")
        before_conflicts = conflicts.read_bytes()
        self.assertNotIn("deploy 0 ", self.run_driver("deploy", game, view, conflicts, "incomplete", self.dir / "missing-package"))
        self.assertEqual(conflicts.read_bytes(), before_conflicts)
        self.assertEqual((view / "Data/B.esp").read_bytes(), b"isolated package write")
        # Rebuild with fewer packages replaces the view atomically.
        out = self.run_driver("deploy", game, view, conflicts, "key-2")
        self.assertIn("deploy 0  matches 1", out)
        self.assertEqual((view / "Data/Textures/Sky.dds").read_bytes(), b"orig-sky")
        self.assertFalse((view / "Data/B.esp").exists())
        self.assertFalse(Path(str(view) + ".old").exists())


    def test_prior_view_survives_a_late_conflict_report_failure(self):
        root = self.dir / "rollback"
        original_files = {
            "Data/Skyrim.esm": b"original base master",
            "Data/Dragonborn.esm": b"original DLC master",
            "Data/Dragonborn.bsa": b"original DLC archive",
            "Data/Textures/Sky.dds": b"original texture",
        }

        def package(name, files):
            folder = root / name
            for relative, data in files.items():
                path = folder / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            return folder

        def snapshot(directory):
            return {p.relative_to(directory).as_posix(): p.read_bytes()
                    for p in directory.rglob("*") if p.is_file()}

        game = package("game", original_files)
        previous = package("previous", {"Textures/sky.DDS": b"old profile texture", "interface/Old.txt": b"old file"})
        candidate = package("candidate", {
            "textures/SKY.dds": b"next profile texture",
            "meshes/New.nif": b"next file",
            "dragonBORN.ESM": b"imported master",
            "DRAGONBORN.BSA": b"imported archive",
        })
        view = root / "view"
        report = root / "committed.tsv"
        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, view, report, "previous", previous))
        previous_view, previous_report = snapshot(view), report.read_bytes()

        # An interrupted swap left the last complete view in .old and an incomplete stage.
        old = view.with_name("view.old")
        building = view.with_name("view.building")
        view.rename(old)
        package("view.building", {"partial.txt": b"incomplete view"})

        # The candidate is valid. A directory at the report path makes its final rename fail.
        blocked_report = package("blocked.tsv", {"keep.txt": b"unrelated directory content"})
        failure = self.run_driver("deploy", game, view, blocked_report, "candidate", candidate)
        self.assertIn(int(failure.split()[1]), {errno.EISDIR, errno.ENOTDIR})
        self.assertIn("matches 0", failure)
        self.assertEqual(snapshot(view), previous_view)
        self.assertEqual(report.read_bytes(), previous_report)
        self.assertEqual(snapshot(blocked_report), {"keep.txt": b"unrelated directory content"})
        self.assertEqual(snapshot(game), original_files)
        self.assertFalse(building.exists())
        self.assertFalse(old.exists())
        self.assertEqual(list(root.glob("view.conflicts.*")), [])

        # The same candidate publishes with the original files intact on retry.
        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, view, report, "candidate", candidate))
        self.assertEqual((view / "Data/Textures/Sky.dds").read_bytes(), b"next profile texture")
        self.assertEqual((view / "Data/meshes/New.nif").read_bytes(), b"next file")
        self.assertFalse((view / "Data/interface/Old.txt").exists())
        self.assertEqual((view / "Data/Dragonborn.esm").read_bytes(), original_files["Data/Dragonborn.esm"])
        self.assertEqual((view / "Data/Dragonborn.bsa").read_bytes(), original_files["Data/Dragonborn.bsa"])
        rows = report.read_text().splitlines()
        self.assertIn("Dragonborn.esm\t0\t-1", rows)
        self.assertIn("Dragonborn.bsa\t0\t-1", rows)
        self.assertEqual(snapshot(game), original_files)


    def test_deploy_rejects_a_view_published_before_its_report(self):
        root = self.dir / "interrupted-publish"
        game, before, after = (root / name for name in ("game", "before", "after"))
        data = game / "Data"
        data.mkdir(parents=True)
        before.mkdir()
        after.mkdir()
        (data / "Skyrim.esm").write_bytes(b"original master")
        (before / "Skyrim.esm").write_bytes(b"old package replacement")
        (before / "Prior.esp").write_bytes(b"prior profile MOD")
        (after / "Next.esp").write_bytes(b"next profile MOD")
        view, report = root / "view", root / "report.tsv"
        candidate, candidate_report = root / "candidate", root / "candidate.tsv"
        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, view, report, "before", before))
        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, candidate,
                                                           candidate_report, "after", after))
        previous_report = report.read_bytes()
        old = view.with_name("view.old")
        building = view.with_name("view.building")

        # Simulate a process interruption after publishing the view but before its report.
        view.rename(old)
        candidate.rename(view)
        self.assertEqual(self.run_driver("matches", view, report, "after"), "matches 0\n")
        self.assertEqual(self.run_driver("matches", view, root / "missing.tsv", "after"), "matches 0\n")

        # An unrelated failed retry must leave the previous complete generation in place.
        failed = self.run_driver("deploy", game, view, report, "other", root / "missing-package")
        self.assertIn(f"deploy {errno.ENOENT} ", failed)
        self.assertFalse(old.exists())
        self.assertFalse(building.exists())
        self.assertEqual(self.run_driver("matches", view, report, "before"), "matches 1\n")
        self.assertEqual(report.read_bytes(), previous_report)
        self.assertEqual((view / "Data/Prior.esp").read_bytes(), b"prior profile MOD")
        self.assertFalse((view / "Data/Next.esp").exists())

        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, view, report, "after", after))
        self.assertEqual((view / "Data/Next.esp").read_bytes(), b"next profile MOD")
        self.assertEqual(report.read_bytes(), candidate_report.read_bytes())
        self.assertFalse((view / "Data/Prior.esp").exists())

        # An interruption after publishing the report should keep the coherent newer view.
        self.assertIn("deploy 0  matches 1", self.run_driver("deploy", game, old,
                                                           root / "old.tsv", "before", before))
        failed = self.run_driver("deploy", game, view, report, "newer", root / "missing-package")
        self.assertIn(f"deploy {errno.ENOENT} ", failed)
        self.assertFalse(old.exists())
        self.assertEqual(self.run_driver("matches", view, report, "after"), "matches 1\n")
        self.assertEqual((view / "Data/Next.esp").read_bytes(), b"next profile MOD")
        self.assertEqual(report.read_bytes(), candidate_report.read_bytes())
        self.assertEqual((data / "Skyrim.esm").read_bytes(), b"original master")


if __name__ == "__main__":
    unittest.main()
