"""Self-authored minimal headers only: packaging gates, never executable probes."""
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("owned_ipa_packager", Path(__file__).with_name("package-owned-host-ipa.py"))
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def image(kind, platform=2, signature=None):
    commands = struct.pack("<6I", 0x32, 24, platform, 0x1A0000, 0x1A0500, 0)
    if signature is not None:
        commands += struct.pack("<4I", signature, 16, 0, 0)
    return struct.pack("<8I", 0xFEEDFACF, 0x100000C, 0, kind, 1 + (signature is not None),
                       len(commands), 0, 0) + commands


class PublicUnsignedPackageTests(unittest.TestCase):
    def test_native_provider_requires_its_marker_and_excludes_legacy_framework(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = self.app(root)
            info_path = app / "Info.plist"
            info = plistlib.loads(info_path.read_bytes())
            info["SKYRIMRendererBackend"] = "native-hal"
            info_path.write_bytes(plistlib.dumps(info))
            with self.assertRaises(ValueError):
                packager.package(app, root / "reject.ipa", root / "reject.json", "native-hal")
            self.assertFalse((root / "reject.ipa").exists())
            (app / "Frameworks/WgpuNative.framework/WgpuNative").unlink()
            packager.package(app, root / "native.ipa", root / "native.json", "native-hal")
            report = json.loads((root / "native.json").read_text())
            self.assertEqual(report["renderer"], "native-hal")
            self.assertEqual(set(report["images"]), {"OwnedGameHost", "Frameworks/PrivateGuest.framework/PrivateGuest"})
            del info["SKYRIMRendererBackend"]
            info_path.write_bytes(plistlib.dumps(info))
            with self.assertRaises(ValueError):
                packager.package(app, root / "missing.ipa", root / "missing.json", "native-hal")

    def app(self, root):
        app = root / "OwnedGameHost.app"
        app.mkdir()
        (app / "Info.plist").write_bytes(plistlib.dumps(dict(
            CFBundleIdentifier="com.halc8312.ownedgamehost", CFBundleExecutable="OwnedGameHost")))
        for name, kind in packager.EXPECTED_IMAGES.items():
            p = app / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(image(kind))
        return app

    def test_unsigned_three_images_and_plain_zip_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = self.app(root)
            ipa, metadata = root / "public.ipa", root / "public.json"
            packager.package(app, ipa, metadata)
            report = json.loads(metadata.read_text())
            self.assertTrue(report["unsigned_device_ipa"])
            self.assertFalse(report["game_executed"])
            self.assertEqual(set(report["images"]), set(packager.EXPECTED_IMAGES))
            with zipfile.ZipFile(ipa) as archive:
                self.assertIsNone(archive.testzip())
                self.assertTrue(all(not i.extra and i.filename.startswith("Payload/OwnedGameHost.app/")
                                    for i in archive.infolist()))

    def test_signature_even_empty_drs_platform_and_truncations_refuse(self):
        for command in [0x1D, 0x2B]:
            with self.assertRaises(ValueError):
                packager.image_metadata(image(6, signature=command), 6)
        with self.assertRaises(ValueError):
            packager.image_metadata(image(6, platform=7), 6)
        original = image(2)
        for end in [0, 31, 33, len(original) - 1]:
            with self.assertRaises(ValueError):
                packager.image_metadata(original[:end], 2)

    def test_unknown_executable_and_signing_artifacts_refuse_before_output(self):
        for path, data in [("Frameworks/other.dylib", image(6)),
                           ("embedded.mobileprovision", b"self-authored"),
                           ("_CodeSignature/CodeResources", b"self-authored")]:
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                app = self.app(root)
                p = app / path
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
                with self.assertRaises(ValueError):
                    packager.package(app, root / "public.ipa", root / "public.json")
                self.assertFalse((root / "public.ipa").exists())

    def test_existing_output_and_symbolic_input_refuse(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = self.app(root)
            output = root / "public.ipa"
            output.write_bytes(b"keep existing public output")
            with self.assertRaises(ValueError):
                packager.package(app, output, root / "public.json")
            self.assertEqual(output.read_bytes(), b"keep existing public output")
            (app / "alias").symlink_to(app / "OwnedGameHost")
            with self.assertRaises(ValueError):
                packager.package(app, root / "new.ipa", root / "public.json")


if __name__ == "__main__":
    unittest.main()
