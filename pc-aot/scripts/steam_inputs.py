"""Read-only, bounded Steam discovery and separate original/derived PE manifests.

No Steam settings, game files, mods or saves are ever written. Standard library only.
Run through Find-SkyrimInputs.ps1 on Windows, or use --steam-root / --game-dir.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import sys
from datetime import datetime, timezone

GAMES = {
    "72850": {"name": "Skyrim 2011", "exe": "TESV.exe", "folder": "Skyrim"},
    "489830": {"name": "Skyrim Special Edition", "exe": "SkyrimSE.exe", "folder": "Skyrim Special Edition"},
}
CORE_DATA = ["Skyrim.esm", "Update.esm", "Skyrim - Animations.bsa", "Skyrim - Interface.bsa",
             "Skyrim - Meshes.bsa", "Skyrim - Misc.bsa", "Skyrim - Shaders.bsa",
             "Skyrim - Sounds.bsa", "Skyrim - Textures.bsa"]


def parse_vdf(text: str) -> dict:
    """Parse quoted/bare Valve KeyValues, // comments and legacy library entries."""
    tokens = []
    pos = 0
    while pos < len(text):
        if text[pos].isspace() or text[pos] == "\ufeff":
            pos += 1
        elif text.startswith("//", pos):
            end = text.find("\n", pos)
            pos = len(text) if end == -1 else end + 1
        elif text[pos] in "{}":
            tokens.append((text[pos], False))
            pos += 1
        elif text[pos] == '"':
            pos += 1
            value = []
            while pos < len(text) and text[pos] != '"':
                if text[pos] == "\\" and pos + 1 < len(text) and text[pos + 1] in '\\"':
                    pos += 1
                value.append(text[pos])
                pos += 1
            if pos == len(text):
                raise ValueError("unterminated VDF quoted string")
            tokens.append(("".join(value), True))
            pos += 1
        else:
            match = re.match(r'[^\s{}"]+', text[pos:])
            if not match:
                raise ValueError(f"invalid VDF at character {pos}")
            tokens.append((match.group(0), True))
            pos += len(match.group(0))
    index = 0

    def obj(nested=False):
        nonlocal index
        result = {}
        while index < len(tokens):
            key, scalar = tokens[index]
            index += 1
            if not scalar:
                if key == "}" and nested:
                    return result
                raise ValueError("unexpected VDF brace")
            if index == len(tokens):
                raise ValueError("missing VDF value")
            value, scalar = tokens[index]
            index += 1
            if not scalar:
                if value != "{":
                    raise ValueError("missing VDF value before closing brace")
                value = obj(True)
            result[key] = value
        if nested:
            raise ValueError("unclosed VDF object")
        return result

    return obj()


def hashes(path: Path) -> dict:
    sha1, sha256 = hashlib.sha1(), hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            sha1.update(chunk)
            sha256.update(chunk)
    return {"sha1": sha1.hexdigest(), "sha256": sha256.hexdigest()}


def pe_metadata(path: Path) -> dict:
    size = path.stat().st_size
    with path.open("rb") as stream:
        dos = stream.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError("not a DOS/PE executable")
        pe_offset = struct.unpack_from("<I", dos, 60)[0]
        if pe_offset < 64 or pe_offset + 26 > size:
            raise ValueError("PE header offset outside file")
        stream.seek(pe_offset)
        header = stream.read(26)
        if header[:4] != b"PE\0\0":
            raise ValueError("invalid PE signature")
        machine, sections, timestamp = struct.unpack_from("<HHI", header, 4)
        optional_size = struct.unpack_from("<H", header, 20)[0]
        magic = struct.unpack_from("<H", header, 24)[0]
        if magic not in (0x10B, 0x20B) or optional_size < (96 if magic == 0x10B else 112):
            raise ValueError("invalid/truncated PE optional header")
        if pe_offset + 24 + optional_size + sections * 40 > size:
            raise ValueError("truncated PE section table")
        stream.seek(pe_offset + 24)
        optional = stream.read(optional_size)
        entry_rva = struct.unpack_from("<I", optional, 16)[0]
        image_base = struct.unpack_from("<I" if magic == 0x10B else "<Q", optional, 28 if magic == 0x10B else 24)[0]
        return {"machine": f"0x{machine:04x}", "architecture": {0x14C: "x86", 0x8664: "x86_64", 0xAA64: "arm64"}.get(machine, "unknown"),
                "bits": 32 if magic == 0x10B else 64, "sections": sections,
                "image_base": f"0x{image_base:x}", "entry_rva": f"0x{entry_rva:x}",
                "timestamp": timestamp}


def windows_version(path: Path) -> dict:
    if sys.platform != "win32":
        return {"status": "unavailable_without_Windows_version_API"}
    from ctypes import wintypes
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    api.GetFileVersionInfoW.restype = wintypes.BOOL
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    api.VerQueryValueW.restype = wintypes.BOOL
    length = api.GetFileVersionInfoSizeW(str(path), None)
    if not length:
        return {"status": "no_version_resource", "windows_error": ctypes.get_last_error()}
    buf = ctypes.create_string_buffer(length)
    if not api.GetFileVersionInfoW(str(path), 0, length, buf):
        raise OSError(ctypes.get_last_error(), "GetFileVersionInfoW failed")
    pointer, count = ctypes.c_void_p(), wintypes.UINT()
    if not api.VerQueryValueW(buf, "\\", ctypes.byref(pointer), ctypes.byref(count)) or count.value < 52:
        return {"status": "invalid_fixed_version_resource"}
    fields = struct.unpack("<13I", ctypes.string_at(pointer, 52))
    if fields[0] != 0xFEEF04BD:
        return {"status": "invalid_version_signature"}
    def version(ms, ls):
        return ".".join(map(str, (ms >> 16, ms & 65535, ls >> 16, ls & 65535)))
    result = {"status": "read", "file_version": version(fields[2], fields[3]), "product_version": version(fields[4], fields[5])}
    if api.VerQueryValueW(buf, "\\VarFileInfo\\Translation", ctypes.byref(pointer), ctypes.byref(count)):
        translations = ctypes.string_at(pointer, count.value)
        result["translations"] = []
        for off in range(0, len(translations) - 3, 4):
            lang, cp = struct.unpack_from("<HH", translations, off)
            values = {"language_id": f"0x{lang:04x}", "codepage": cp}
            for key in ("FileVersion", "ProductVersion", "ProductName"):
                query = f"\\StringFileInfo\\{lang:04x}{cp:04x}\\{key}"
                if api.VerQueryValueW(buf, query, ctypes.byref(pointer), ctypes.byref(count)) and count.value:
                    values[key] = ctypes.wstring_at(pointer, count.value).rstrip("\0")
            result["translations"].append(values)
    return result


def executable_record(path: Path, role="steam_original") -> dict:
    record = {"path": str(path.resolve()), "role": role, "size": path.stat().st_size, **hashes(path)}
    try:
        record["pe"] = pe_metadata(path)
        record["version"] = windows_version(path)
    except (OSError, ValueError) as error:
        record["inspection_error"] = str(error)
    return record


def path_on_host(value: str) -> Path:
    if sys.platform != "win32" and re.match(r"^[A-Za-z]:[\\/]", value):
        return Path("/mnt") / value[0].lower() / value[3:].replace("\\", "/")
    return Path(value)


def registry_roots() -> list[Path]:
    roots = []
    if sys.platform == "win32":
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, key) as handle:
                    for value in ("SteamPath", "InstallPath"):
                        try:
                            roots.append(Path(winreg.QueryValueEx(handle, value)[0]))
                        except FileNotFoundError:
                            pass
            except FileNotFoundError:
                pass
        roots.extend(Path(os.environ.get(var, fallback)) / "Steam" for var, fallback in
                     (("ProgramFiles(x86)", r"C:\Program Files (x86)"), ("ProgramFiles", r"C:\Program Files")))
    else:
        roots.extend(path_on_host(p) for p in (r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"))
    return unique_paths(roots)


def unique_paths(paths):
    result, seen = [], set()
    for path in paths:
        key = str(path.absolute()).casefold() if sys.platform == "win32" else str(path.absolute())
        if key not in seen:
            result.append(path)
            seen.add(key)
    return result


def discover(steam_roots=None, game_dirs=None, fallback=True):
    errors, checked, libraries = [], [], []
    roots = unique_paths(list(steam_roots or []) + registry_roots())
    for root in roots:
        vdf = root / "steamapps" / "libraryfolders.vdf"
        libraries.append(root)
        checked.append({"kind": "libraryfolders", "path": str(vdf), "exists": vdf.is_file()})
        if vdf.is_file():
            try:
                entries = parse_vdf(vdf.read_text(encoding="utf-8-sig"))["libraryfolders"]
                for key, entry in entries.items():
                    if key.isdigit():
                        value = entry.get("path") if isinstance(entry, dict) else entry
                        if value:
                            libraries.append(path_on_host(value))
            except (ValueError, KeyError, OSError) as error:
                errors.append({"path": str(vdf), "error": str(error)})
    libraries = unique_paths(libraries)
    candidates = {key: [] for key in GAMES}
    metadata = {}
    for lib in libraries:
        for app_id in GAMES:
            acf = lib / "steamapps" / f"appmanifest_{app_id}.acf"
            checked.append({"kind": "appmanifest", "app_id": app_id, "path": str(acf), "exists": acf.is_file()})
            if acf.is_file():
                try:
                    state = parse_vdf(acf.read_text(encoding="utf-8-sig"))["AppState"]
                    folder = state["installdir"]
                    if state.get("appid") != app_id or not folder or any(c in folder for c in ("/", "\\", ":")) or folder in (".", ".."):
                        raise ValueError("invalid appid or non-local installdir")
                    candidate = lib / "steamapps" / "common" / folder
                    candidates[app_id].append(candidate)
                    userconfig = state.get("UserConfig", {})
                    metadata[str(candidate)] = {"manifest_path": str(acf), "appid": app_id, "buildid": state.get("buildid"),
                                                "state_flags": state.get("StateFlags"), "language": userconfig.get("language") if isinstance(userconfig, dict) else None}
                except (ValueError, KeyError, OSError) as error:
                    errors.append({"path": str(acf), "error": str(error)})
    fallback_roots = []
    if fallback and any(not any((p / GAMES[key]["exe"]).is_file() for p in candidates[key]) for key in GAMES):
        # Explicit bounded locations: never recurse across a drive or read arbitrary personal files.
        fallback_roots = [path_on_host(p) for p in (r"F:\SteamLibrary", r"F:\Steam", r"F:\Program Files\Steam",
                          r"F:\Program Files (x86)\Steam", r"F:\Games\Steam", r"F:\Games\SteamLibrary", r"F:\Applications\Steam")]
        froot = path_on_host("F:\\")
        if froot.is_dir():
            for child in froot.iterdir():
                if child.is_dir() and re.fullmatch(r"Steam(?:Library)?[ _-]?\d*", child.name, flags=re.I):
                    fallback_roots.append(child)
        for lib in unique_paths(libraries + fallback_roots):
            checked.append({"kind": "bounded_library", "path": str(lib), "exists": lib.is_dir()})
            common = lib / "steamapps" / "common"
            if common.is_dir():
                for folder in common.iterdir():
                    if folder.is_dir() and "skyrim" in folder.name.casefold():
                        for app_id in GAMES:
                            candidates[app_id].append(folder)
    for directory in game_dirs or []:
        for app_id in GAMES:
            candidates[app_id].append(directory)
    games = []
    for app_id, spec in GAMES.items():
        records = []
        for folder in unique_paths(candidates[app_id]):
            exe = folder / spec["exe"]
            checked.append({"kind": "executable", "app_id": app_id, "path": str(exe), "exists": exe.is_file()})
            if exe.is_file():
                data = folder / "Data"
                records.append({"directory": str(folder.resolve()), "steam": metadata.get(str(folder)),
                                "original_executable": executable_record(exe),
                                "derived_executable": None,
                                "data": [{"name": name, "exists": (data / name).is_file(),
                                          "size": (data / name).stat().st_size if (data / name).is_file() else None} for name in CORE_DATA],
                                "voice_archives": [{"name": p.name, "size": p.stat().st_size} for p in data.glob("Skyrim*Voices*.bsa") if p.is_file()],
                                "note": "Presence inventory only; no Data hashes, mod-free assertion or game launch."})
        games.append({"app_id": app_id, "name": spec["name"], "status": "found" if records else "not_found_in_checked_locations", "installations": records})
    visible_f = path_on_host("F:\\").is_dir()
    return {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
            "host": {"platform": platform.platform(), "execution": "Windows native" if sys.platform == "win32" else ("WSL" if "microsoft" in platform.release().lower() else "non-Windows"),
                     "f_drive_accessible": visible_f},
            "steam_roots": [str(p) for p in roots], "registered_libraries": [str(p) for p in libraries],
            "games": games, "checks": checked, "errors": errors,
            "scope": "Registry, standard roots, libraryfolders, exact app manifests, then bounded F-first Steam/common folders. No recursive drive search; absence is scoped, not proof of no installation."}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--steam-root", type=Path, action="append")
    parser.add_argument("--game-dir", type=Path, action="append")
    parser.add_argument("--no-fallback", action="store_true")
    args = parser.parse_args(argv)
    report = discover(args.steam_root, args.game_dir, not args.no_fallback)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8", newline="\n") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)
        file.write("\n")
    print(json.dumps({"manifest": str(args.output.resolve()), "games": [{"name": g["name"], "status": g["status"]} for g in report["games"]],
                      "f_drive_accessible": report["host"]["f_drive_accessible"], "inspection_errors": len(report["errors"])}, ensure_ascii=False))
    return 2 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
