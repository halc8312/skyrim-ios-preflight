"""Serve one pre-signed IPA through a private, authenticated Tailscale Serve proxy.

Only listens on loopback. Configuration and all IPA/signing data stay private.
This does not build, re-sign, upload, or automatically install an application.
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import html
import json
import plistlib
import re
import time
import zipfile
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit


class Package:
    def __init__(self, config: dict):
        self.ipa = Path(config["ipa"]).resolve(strict=True)
        self.signature = self.ipa.stat()
        self.sha256 = config["sha256"]
        with self.ipa.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != self.sha256:
                raise ValueError("IPA hash differs from the verified signing result")
        self.login = config["tailscale_login"]
        self.token = config["token"]
        if not self.login or not re.fullmatch(r"[a-zA-Z0-9_-]{32,96}", self.token):
            raise ValueError("Missing private identity or invalid access token")
        base = urlsplit(config["base_url"])
        if (base.scheme != "https" or not base.hostname or
                not base.hostname.endswith(".ts.net") or base.username or
                base.password or base.query or base.fragment or base.port):
            raise ValueError("Use the private Tailscale HTTPS origin on port 443")
        self.base_path = base.path.rstrip("/")
        if self.base_path != "/skyrim-ios":
            raise ValueError("Unexpected private route")
        self.origin = f"https://{base.netloc}"
        self.route = f"/{self.token}/"
        self.public_path = self.base_path + self.route
        self.url = self.origin + self.public_path
        self.expires = int(config["expires_unix"])
        if self.expires <= time.time():
            raise ValueError("The private download link has expired")
        with zipfile.ZipFile(self.ipa) as archive:
            names = [n for n in archive.namelist()
                     if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
            if len(names) != 1:
                raise ValueError("Expected exactly one application")
            info = plistlib.loads(archive.read(names[0]))
            profile_raw = archive.read(names[0].rsplit("/", 1)[0] +
                                       "/embedded.mobileprovision")
        start = profile_raw.find(b"<?xml")
        end = profile_raw.find(b"</plist>", start)
        if start < 0 or end < 0:
            raise ValueError("Provisioning profile cannot be inspected")
        profile = plistlib.loads(profile_raw[start:end + len(b"</plist>")])
        self.bundle = info["CFBundleIdentifier"]
        self.build = str(info["CFBundleVersion"])
        if self.bundle != config["bundle_id"] or self.build != str(config["build"]):
            raise ValueError("Application identity/build differs from configuration")
        entitlements = profile["Entitlements"]
        if not entitlements["application-identifier"].endswith("." + self.bundle):
            raise ValueError("Application and provisioning profile do not match")
        for name in config["required_entitlements"]:
            if entitlements.get(name) is not True:
                raise ValueError("A required memory entitlement is missing")
        if not profile.get("ProvisionedDevices"):
            raise ValueError("Expected the existing registered-device profile")
        if profile["ExpirationDate"].replace(tzinfo=timezone.utc) <= datetime.now(timezone.utc):
            raise ValueError("Provisioning profile has expired")
        self.manifest = plistlib.dumps({"items": [{
            "assets": [{"kind": "software-package", "url": self.url + "SkyrimIOS.ipa"}],
            "metadata": {"bundle-identifier": self.bundle, "bundle-version": self.build,
                         "kind": "software", "title": "SkyrimIOS"},
        }]}, fmt=plistlib.FMT_XML)
        install_url = "itms-services://?action=download-manifest&url=" + quote(
            self.url + "manifest.plist", safe="")
        template = Path(__file__).with_name("private_ota.html.in").read_text(encoding="utf-8")
        replacements = {"@@BUILD@@": self.build,
                        "@@SIZE@@": f"{self.signature.st_size / 1_000_000:.1f}",
                        "@@INSTALL@@": install_url}
        for key, value in replacements.items():
            template = template.replace(key, html.escape(value, quote=True))
        self.page = template.encode("utf-8")
        self.status = json.dumps({"ready": True, "build": self.build,
                                  "bytes": self.signature.st_size,
                                  "sha256": self.sha256}).encode()

    def unchanged(self) -> bool:
        stat = self.ipa.stat()
        return (stat.st_size == self.signature.st_size and
                stat.st_mtime_ns == self.signature.st_mtime_ns)


def byte_range(value: str | None, size: int) -> tuple[int, int, bool]:
    if value is None:
        return 0, size - 1, False
    match = re.fullmatch(r"bytes=(\d*)-(\d*)", value)
    if not match or not any(match.groups()):
        raise ValueError("Only one valid byte range is supported")
    first, last = match.groups()
    if not first:
        length = int(last)
        if length <= 0:
            raise ValueError("Empty suffix")
        return max(0, size - length), size - 1, True
    start = int(first)
    end = min(int(last), size - 1) if last else size - 1
    if start >= size or end < start:
        raise ValueError("Unsatisfiable byte range")
    return start, end, True


def handler_for(package: Package):
    class Handler(BaseHTTPRequestHandler):
        server_version = "PrivateOTA"
        sys_version = ""
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            # Do not log token-bearing URLs, IPs, user identities, or headers.
            pass

        def headers_out(self, code, length, content_type, extra=()):
            self.send_response(code)
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "private, no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
            for name, value in extra:
                self.send_header(name, value)
            self.end_headers()

        def empty(self, code, extra=()):
            self.headers_out(code, 0, "text/plain", extra)

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def serve(self, body):
            # Tailscale Serve replaces this header. The listener is loopback-only.
            identity = self.headers.get("Tailscale-User-Login", "")
            if not hmac.compare_digest(identity.encode(), package.login.encode()):
                return self.empty(404)
            parsed = urlsplit(self.path)
            path = parsed.path
            # Serve proxies may strip --set-path. Accept exactly those two routes.
            route = next((p for p in (package.public_path, package.route)
                          if path.startswith(p)), None)
            if route is None or parsed.query or parsed.fragment:
                return self.empty(404)
            resource = path[len(route):]
            if resource not in ("", "manifest.plist", "SkyrimIOS.ipa", "status.json"):
                return self.empty(404)
            if time.time() >= package.expires:
                return self.empty(410)
            try:
                if not package.unchanged():
                    return self.empty(503)
                if resource == "SkyrimIOS.ipa":
                    try:
                        start, end, partial = byte_range(self.headers.get("Range"),
                                                         package.signature.st_size)
                    except ValueError:
                        return self.empty(416, [("Content-Range",
                                                f"bytes */{package.signature.st_size}")])
                    headers = [("Accept-Ranges", "bytes")]
                    if partial:
                        headers.append(("Content-Range",
                                        f"bytes {start}-{end}/{package.signature.st_size}"))
                    # Open before responding so missing/locked files cannot appear successful.
                    with package.ipa.open("rb") as stream:
                        self.headers_out(206 if partial else 200, end - start + 1,
                                         "application/octet-stream", headers)
                        if body:
                            stream.seek(start)
                            remaining = end - start + 1
                            while remaining:
                                chunk = stream.read(min(256 * 1024, remaining))
                                if not chunk:
                                    self.close_connection = True
                                    return
                                self.wfile.write(chunk)
                                remaining -= len(chunk)
                    print(json.dumps({"event": "ipa_transfer", "body": body,
                                      "bytes": end - start + 1, "partial": partial}), flush=True)
                    return
                payload, content_type = {
                    "": (package.page, "text/html; charset=utf-8"),
                    "manifest.plist": (package.manifest, "text/xml; charset=utf-8"),
                    "status.json": (package.status, "application/json"),
                }[resource]
                self.headers_out(200, len(payload), content_type)
                if body:
                    self.wfile.write(payload)
                print(json.dumps({"event": "request", "resource": resource or "page",
                                  "body": body}), flush=True)
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    package = Package(json.loads(args.config.read_text(encoding="utf-8")))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler_for(package))
    server.daemon_threads = True
    print(json.dumps({"event": "ready", "build": package.build,
                      "bytes": package.signature.st_size, "port": args.port}), flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
