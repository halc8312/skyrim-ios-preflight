"""Check exact ECE 1.59 winners in a hosted copy of the deployed Data view."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
MESHES = Path("meshes")
CME = MESHES / "CharacterMakingExtender"
COMMON = CME / "CommonTriRaces"
FACE = MESHES / "actors/character/character assets/FemaleHeadCharGen.tri"
RACE = CME / "CME_VanillaRaces.ini"
SLIDERS = COMMON / "HumanRaces_Slider.ini"
MATCH, MISSING, NONDEFAULT, UNREADABLE = range(4)

# These files contain authored marker bytes, which the test's digest callback
# replays. Only the optional private test hashes the actual packaged assets.
MARKERS = {
    FACE: (803424, "39f940fcf6f2b3cb3f9ffefca09e5bc82ea676ec12251e4e61cf1abd5df09475"),
    RACE: (2832, "694fb7b161e4df9857b3139e27c6376c0444a00c3fd42f4d202a9467927c3723"),
    SLIDERS: (31405, "efedb3ae0e1bd7a591b06a0b6316b19ad58ea31029f5df4caa766c3664054ea8"),
    COMMON / "baseHeadFemale.tri": (803424, "acb351463ee77cd7a68cbe227adc651f43490cb42ef405b43e5233a7a57fe57c"),
    COMMON / "CME_HeadFemale.tri": (564171, "0491beb8586d8ac705d7ca246a4890f9f826cb45e8b0382eb7ee2a557b28c312"),
    COMMON / "ECEHeadFemale.tri": (1344402, "0ef14130814522db82ead87331ba017a59b9cd27dafcdacc04bdd06e0a421508"),
    COMMON / "RANs_HeadFemale.tri": (468274, "9282f9667ffde39aba8e758fe1dc8d9e435a6883dccba8d27b38313d8518bd85"),
    COMMON / "femalehead.tri": (323838, "35319e40e7d801ec7d644c3a30db047b93228e4b0f1366c2438e840f6f1e3e47"),
}


@unittest.skipUnless(os.name == "posix", "Requires a POSIX C build toolchain")
class EceProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "ece-profile"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_ece_profile.c",
                        PORT / "mod_ece_profile.c", "-o", cls.binary], check=True, timeout=60)

    def setUp(self):
        view = tempfile.TemporaryDirectory()
        self.addCleanup(view.cleanup)
        self.data = Path(view.name)
        for relative, (size, digest) in MARKERS.items():
            path = self.data / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("wb") as stream:
                stream.write(bytes.fromhex(digest))
                stream.truncate(size)

    def checked(self, data=None):
        command = [self.binary, self.data, "replay"] if data is None else [self.binary, data, "sha256", sys.executable]
        return tuple(map(int, subprocess.check_output(command, text=True, timeout=10).split()))

    def test_default_files_and_case_variations(self):
        self.assertEqual(self.checked(), (MATCH, len(MARKERS)))
        (self.data / FACE).rename(self.data / FACE.with_name(FACE.name.lower()))
        (self.data / COMMON).rename(self.data / COMMON.with_name("commontriraces"))
        self.assertEqual(self.checked(), (MATCH, len(MARKERS)))

    def test_missing_effective_winner(self):
        (self.data / FACE).unlink()
        self.assertEqual(self.checked(), (MISSING, 0))

    def test_refuses_wrong_tri_even_with_expected_length(self):
        path = self.data / FACE
        with path.open("r+b") as stream:
            stream.write(b"other same-length TRI winner")
        self.assertEqual(self.checked(), (NONDEFAULT, 1))
        path.write_bytes(b"shorter TRI")
        self.assertEqual(self.checked(), (NONDEFAULT, 0))

    def test_refuses_modified_configuration_or_added_slider_scale(self):
        path = self.data / SLIDERS
        with path.open("r+b") as stream:
            stream.write(b"other same-length slider configurations")
        self.assertEqual(self.checked(), (NONDEFAULT, 3))
        (self.data / CME / "sLiDeRsCaLe.InI").write_bytes(b"SliderScale=2")
        self.assertEqual(self.checked(), (NONDEFAULT, 1))

    def test_refuses_additional_race_ini_and_alternate_source_tri(self):
        extra = self.data / CME / "CME_OtherRaces.INI"
        extra.write_bytes(b"EDID = NordRace")
        self.assertEqual(self.checked(), (NONDEFAULT, 1))
        extra.unlink()
        (self.data / COMMON / "ECEHeadFemale.tri").write_bytes(b"alternate morph source")
        self.assertEqual(self.checked(), (NONDEFAULT, 5))

    def test_refuses_linked_asset_or_directory(self):
        path = self.data / FACE
        original = path.with_name("actual.tri")
        path.rename(original)
        path.symlink_to(original)
        self.assertEqual(self.checked(), (UNREADABLE, 0))
        path.unlink()
        original.rename(path)
        directory = self.data / CME
        original = directory.with_name("other")
        directory.rename(original)
        directory.symlink_to(original, target_is_directory=True)
        self.assertEqual(self.checked(), (UNREADABLE, 1))

    def test_refuses_case_ambiguous_winner(self):
        path = self.data / FACE
        other = path.with_name(path.name.lower())
        if other.exists():
            self.skipTest("Case-insensitive test directory")
        other.write_bytes(b"conflicting loose file")
        self.assertEqual(self.checked(), (UNREADABLE, 0))

    def test_prepared_skydream_reads_the_qualified_tri(self):
        from test_directory_index import kernel_file_fixture
        with tempfile.TemporaryDirectory(prefix="ece-tri-read-") as folder:
            root = Path(folder)
            data = root / "Data"
            shutil.copytree(self.data, data)
            shutil.copyfile(data / FACE, data / "other.tri")
            digest = bytes.fromhex("01e4816a1ea6c5bcb6a4c3bf62c9cdca6574600ddc441b95341b4942df3bdf03")
            (data / "Effect.pex").write_bytes(digest + b"\0" * (5875 - len(digest)))
            (data / "OtherEffect.pex").write_bytes(b"\0" * 5875)
            binary = kernel_file_fixture(root, ece_witness=True)
            entry = r"Data\meshes\Actors\Character\Character Assets\FemaleHeadCharGen.tri"
            completed = subprocess.run([binary, root, entry], capture_output=True, check=True, timeout=30)
            self.assertEqual(completed.stdout, (self.data / FACE).read_bytes())
            lines = [line for line in completed.stderr.decode().splitlines() if line.startswith("IOS_ECE_TRI")]
            self.assertEqual(lines, [
                "IOS_ECE_TRI schema=1 source=1 parsed=1",
                "IOS_ECE_TRI schema=1 source=2 parsed=1",
                "IOS_ECE_TRI schema=1 source=0 parsed=0",
                "IOS_ECE_TRI schema=1 source=0 parsed=1",
            ])
            nose = [line for line in completed.stderr.decode().splitlines() if line.startswith("IOS_ECE_NOSE")]
            self.assertEqual(nose, [
                "IOS_ECE_NOSE schema=1 female_nord=1 head=1 geometry=1 path=1 fresh=1 record=1",
                "IOS_ECE_NOSE schema=1 female_nord=1 head=1 geometry=1 path=1 fresh=0 record=1",
            ])
            heels = [line for line in completed.stderr.decode().splitlines() if line.startswith("IOS_CLASSIC_HIGHHEEL_PEX")]
            self.assertEqual(heels, [
                "IOS_CLASSIC_HIGHHEEL_PEX schema=1 phase=0 verified=0",
                "IOS_CLASSIC_HIGHHEEL_PEX schema=1 phase=0 verified=1",
                "IOS_CLASSIC_HIGHHEEL_PEX schema=1 phase=1 verified=1",
            ])

    def test_private_owned_default_and_modified_effective_tri(self):
        private = os.environ.get("SKYRIM_TEST_ECE_DEFAULT_DATA")
        if not private:
            self.skipTest("Set SKYRIM_TEST_ECE_DEFAULT_DATA to the private Data contents from ECE 1.59 00 Main.")
        root = Path(private)
        self.assertEqual(self.checked(root), (MATCH, len(MARKERS)))
        for path in MARKERS:
            target = self.data / path
            # Preserve the release member's original spelling for private staged input.
            source = root
            for component in path.parts:
                candidates = [p for p in source.iterdir() if p.name.lower() == component.lower()]
                self.assertEqual(len(candidates), 1)
                source = candidates[0]
            shutil.copyfile(source, target)
        self.assertEqual(self.checked(self.data), (MATCH, len(MARKERS)))
        with (self.data / FACE).open("r+b") as stream:
            stream.seek(-1, os.SEEK_END)
            last = stream.read(1)
            stream.seek(-1, os.SEEK_END)
            stream.write(bytes([last[0] ^ 1]))
        self.assertEqual(self.checked(self.data), (NONDEFAULT, 1))


if __name__ == "__main__":
    unittest.main()
