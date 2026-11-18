"""Authored package and in-memory HTTP checks; no signing or listening server."""
import contextlib
from datetime import datetime, timedelta, timezone
from email.parser import Parser
import hashlib
import html
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import tempfile
import time
import unittest
from urllib.parse import quote
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("private_ota_products", ROOT / "scripts/serve_private_ota.py")
ota = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ota)


class MemorySocket:
    def __init__(self, request):
        self.request = io.BytesIO(request)
        self.output = bytearray()

    def makefile(self, *_args, **_kwargs):
        return self.request

    def sendall(self, data):
        self.output.extend(data)


def request(package, path, *, login="fixture@example.invalid", method="GET", extra=()):
    headers = ["Host: fixture.ts.net", "Connection: close"]
    if login is not None:
        headers.append("Tailscale-User-Login: " + login)
    headers.extend(extra)
    data = (method + " " + path + " HTTP/1.1\r\n" + "\r\n".join(headers) + "\r\n\r\n").encode()
    connection = MemorySocket(data)
    with contextlib.redirect_stdout(io.StringIO()):
        ota.handler_for(package)(connection, ("127.0.0.1", 1), object())
    header, _, body = bytes(connection.output).partition(b"\r\n\r\n")
    status_line, _, lines = header.decode().partition("\r\n")
    return int(status_line.split()[1]), Parser().parsestr(lines), body


class PrivateOTAProductTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.counter = 0

    def authored(self, bundle="org.skyrimios.pc2011", *, ipa_bundle=None, profile_bundle=None,
                 missing_profile=False, expired_profile=False, required=(), entitlements=None, build="14"):
        """Synthetic metadata only; these bytes are not an Apple-signed application."""
        self.counter += 1
        ipa = self.root / f"authored-{self.counter}.ipa"
        actual = ipa_bundle or bundle
        expiry = datetime.now(timezone.utc).replace(tzinfo=None) + timedelta(days=-1 if expired_profile else 1)
        info = {"CFBundleIdentifier": actual, "CFBundleVersion": build}
        ent = {"application-identifier": "ABCDEFGHIJ." + (profile_bundle or actual), **(entitlements or {})}
        profile = {"Entitlements": ent, "ProvisionedDevices": ["SYNTHETIC-NOT-A-DEVICE"], "ExpirationDate": expiry}
        with zipfile.ZipFile(ipa, "x") as archive:
            archive.writestr("Payload/Authored.app/Info.plist", plistlib.dumps(info))
            if not missing_profile:
                archive.writestr("Payload/Authored.app/embedded.mobileprovision", plistlib.dumps(profile))
        config = {"ipa": str(ipa), "sha256": hashlib.sha256(ipa.read_bytes()).hexdigest(),
                  "tailscale_login": "fixture@example.invalid", "token": "AuthoredTokenOnly_NotASecret_1234567890",
                  "base_url": "https://fixture.ts.net" + ("/metal-probe" if bundle == "org.skyrimios.metalprobe" else "/skyrim-ios"),
                  "expires_unix": int(time.time()) + 600, "bundle_id": bundle,
                  "build": build, "required_entitlements": list(required)}
        return config

    def test_existing_skyrim_manifest_page_and_status_are_unchanged(self):
        config = self.authored()
        package = ota.Package(config)
        self.assertEqual(package.filename, "SkyrimIOS.ipa")
        self.assertEqual(package.base_path, "/skyrim-ios")
        expected_manifest = {"items": [{"assets": [{"kind": "software-package", "url": package.url + "SkyrimIOS.ipa"}],
            "metadata": {"bundle-identifier": "org.skyrimios.pc2011", "bundle-version": "14",
                         "kind": "software", "title": "SkyrimIOS"}}]}
        self.assertEqual(package.manifest, plistlib.dumps(expected_manifest, fmt=plistlib.FMT_XML))
        expected_page = (ROOT / "scripts/private_ota.html.in").read_text(encoding="utf-8")
        replacements = {"@@BUILD@@": "14", "@@SIZE@@": f"{package.signature.st_size / 1_000_000:.1f}",
                        "@@INSTALL@@": "itms-services://?action=download-manifest&url=" + quote(package.url + "manifest.plist", safe="")}
        for key, value in replacements.items():
            expected_page = expected_page.replace(key, html.escape(value, quote=True))
        self.assertEqual(package.page, expected_page.encode())
        self.assertEqual(json.loads(package.status), {"ready": True, "build": "14",
                         "bytes": package.signature.st_size, "sha256": config["sha256"]})

    def test_probe_selects_only_its_own_route_filename_title_and_page(self):
        package = ota.Package(self.authored("org.skyrimios.metalprobe", build="1"))
        self.assertEqual(package.base_path, "/metal-probe")
        self.assertEqual(package.filename, "MetalProbe.ipa")
        item = plistlib.loads(package.manifest)["items"][0]
        self.assertEqual(item["assets"][0]["url"], package.url + "MetalProbe.ipa")
        self.assertEqual(item["metadata"]["bundle-identifier"], "org.skyrimios.metalprobe")
        self.assertEqual(item["metadata"]["title"], "Metal比較")
        self.assertIn("Metal比較をインストール", package.page.decode())
        self.assertIn("iOS 26以上", package.page.decode())
        self.assertNotIn("SkyrimIOS.ipa", package.page.decode())
        self.assertNotIn("@@", package.page.decode())

    def test_cross_product_routes_resources_and_unknown_products_are_rejected(self):
        pairs = (("org.skyrimios.pc2011", "/metal-probe", "MetalProbe.ipa"),
                 ("org.skyrimios.metalprobe", "/skyrim-ios", "SkyrimIOS.ipa"))
        for bundle, wrong_route, wrong_file in pairs:
            with self.subTest(bundle=bundle):
                config = self.authored(bundle)
                package = ota.Package(config)
                self.assertEqual(request(package, wrong_route + package.route + package.filename)[0], 404)
                self.assertEqual(request(package, package.public_path + wrong_file)[0], 404)
                config["base_url"] = "https://fixture.ts.net" + wrong_route
                with self.assertRaisesRegex(ValueError, "Unexpected private route"):
                    ota.Package(config)
        config = self.authored("org.unrelated.app")
        with self.assertRaisesRegex(ValueError, "Unexpected application identity"):
            ota.Package(config)

    def test_identity_hash_profile_and_build_mismatches_cannot_create_ready_package(self):
        config = self.authored("org.skyrimios.metalprobe", ipa_bundle="org.skyrimios.pc2011")
        with self.assertRaisesRegex(ValueError, "identity/build"):
            ota.Package(config)
        config = self.authored("org.skyrimios.metalprobe", profile_bundle="org.skyrimios.pc2011")
        with self.assertRaisesRegex(ValueError, "profile do not match"):
            ota.Package(config)
        config = self.authored("org.skyrimios.metalprobe", missing_profile=True)
        with self.assertRaises(KeyError):
            ota.Package(config)
        config = self.authored("org.skyrimios.metalprobe")
        config["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "hash differs"):
            ota.Package(config)
        config = self.authored("org.skyrimios.metalprobe")
        config["build"] = "99"
        with self.assertRaisesRegex(ValueError, "identity/build"):
            ota.Package(config)

    def test_profile_expiry_entitlement_and_link_expiry_checks_are_preserved(self):
        for bundle in ("org.skyrimios.pc2011", "org.skyrimios.metalprobe"):
            with self.subTest(bundle=bundle):
                with self.assertRaisesRegex(ValueError, "profile has expired"):
                    ota.Package(self.authored(bundle, expired_profile=True))
                with self.assertRaisesRegex(ValueError, "entitlement is missing"):
                    ota.Package(self.authored(bundle, required=("authored.required",)))
                config = self.authored(bundle)
                config["expires_unix"] = int(time.time()) - 1
                with self.assertRaisesRegex(ValueError, "download link has expired"):
                    ota.Package(config)

    def test_authentication_tokens_queries_and_runtime_expiry_remain_required(self):
        package = ota.Package(self.authored("org.skyrimios.metalprobe"))
        for login in (None, "someone@example.invalid"):
            self.assertEqual(request(package, package.public_path, login=login)[0], 404)
        for path in ("/metal-probe/wrong-token/", package.public_path + "?download=1", package.public_path + "../manifest.plist"):
            self.assertEqual(request(package, path)[0], 404)
        package.expires = time.time() - 1
        self.assertEqual(request(package, package.public_path)[0], 410)

    def test_full_and_stripped_routes_stream_only_the_matching_package(self):
        for bundle in ("org.skyrimios.pc2011", "org.skyrimios.metalprobe"):
            package = ota.Package(self.authored(bundle))
            for route in (package.public_path, package.route):
                with self.subTest(bundle=bundle, route_form="full" if route == package.public_path else "stripped"):
                    status, headers, body = request(package, route + package.filename)
                    self.assertEqual(status, 200)
                    self.assertEqual(body, package.ipa.read_bytes())
                    self.assertEqual(headers["Cache-Control"], "private, no-store")
                    self.assertEqual(headers["Referrer-Policy"], "no-referrer")
                    self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
                    status, headers, body = request(package, route + package.filename, extra=("Range: bytes=0-15",))
                    self.assertEqual(status, 206)
                    self.assertEqual(body, package.ipa.read_bytes()[:16])
                    self.assertEqual(headers["Content-Range"], f"bytes 0-15/{package.signature.st_size}")
                    status, headers, body = request(package, route + package.filename, method="HEAD")
                    self.assertEqual(status, 200)
                    self.assertEqual(body, b"")
                    self.assertEqual(int(headers["Content-Length"]), package.signature.st_size)

    def test_modified_package_refuses_further_transfer(self):
        package = ota.Package(self.authored("org.skyrimios.metalprobe"))
        with package.ipa.open("ab") as stream:
            stream.write(b"authored changed bytes")
        self.assertEqual(request(package, package.public_path + package.filename)[0], 503)


if __name__ == "__main__":
    unittest.main()
