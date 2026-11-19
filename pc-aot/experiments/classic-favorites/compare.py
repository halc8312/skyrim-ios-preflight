"""Compare one Classic Papyrus favorite query against a locally supplied SKSE 1.7.3 SDK."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
HASHES = {
    "PapyrusGame.cpp": "5ad56d75721494a5eda0b2351747760bddde98e59cd3a3e2b88257ae9ed85dbb",
    "GameData.cpp": "9b3ba290104d7c10452eceb1fafebaae4c26f5c4bc451b0e40be37e60a09a918",
    "GameTypes.h": "cdae2ca2369fd0a42e1643ca0319cb0b04cfb2b8a524419dffed58fb2c6ef554",
    "GameExtraData.cpp": "7859eec6530de8a08b258cc596245333d1eee737edaad2b5f7b95cc868fcc7e8",
    "GameBSExtraData.cpp": "465576afef35bf252b48e152e2863d90d2c171d7b5107a77db59d6e973bb4e27",
    "GameBSExtraData.h": "c42e806614507e60502534eabebbca2fd8fe4391f7df62bef8e9b67bb4dcad85",
}


def excerpt(source, first, following):
    start = source.index(first)
    return source[start:source.index(following, start)]


def prepare_reference(root, scratch):
    sdk = root if (root / "PapyrusGame.cpp").is_file() else root / "src/skse/skse"
    contents = {}
    for name, expected in HASHES.items():
        data = (sdk / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"{name} does not match the unmodified Classic 1.7.3 archive")
        contents[name] = data.decode("utf-8").replace("\r\n", "\n")

    # Compile the original member search, visitors, extra lookup, and Papyrus
    # function. Their code remains only in the private temporary directory.
    (scratch / "sdk_collections.inc").write_text(
        excerpt(contents["GameTypes.h"], "template <class T>\nclass tArray",
                "\n\ttemplate <class Op>\n\tT * Find(Op& op) const") + "\n};\n")
    (scratch / "sdk_presence.inc").write_text(
        excerpt(contents["GameBSExtraData.h"], "struct PresenceBitfield", "\n\tvoid MarkType("))
    extra = contents["GameExtraData.cpp"]
    game = contents["PapyrusGame.cpp"]
    if 'bool, TESForm*>("IsObjectFavorited", "Game", papyrusGame::IsObjectFavorited' not in game:
        raise ValueError("Missing Classic native registration")
    (scratch / "sdk_favorites.inc").write_text("\n".join((
        excerpt(contents["GameBSExtraData.cpp"], "BSExtraData* BaseExtraList::GetByType(",
                "\nbool BaseExtraList::Remove("),
        excerpt(contents["GameData.cpp"], "bool MagicFavorites::IsFavorited(",
                "\nGarbageCollector * GarbageCollector::GetSingleton("),
        excerpt(extra, "class HotkeyDataFinder", "HotkeyData ExtraContainerChanges::FindHotkey(SInt32"),
        excerpt(extra, "HotkeyData ExtraContainerChanges::FindHotkey(TESForm * form)",
                "\nInventoryEntryData::InventoryEntryData("),
        "namespace papyrusGame {",
        excerpt(game, "bool IsObjectFavorited(", "\n\tTESForm* GetFormEx("),
        "}")))


def find_vs_command():
    if os.name != "nt":
        raise RuntimeError("Run on genuine Windows with the x86 MSVC compiler")
    installer = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    version = subprocess.check_output(
        [installer, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"], text=True).strip()
    command = Path(version) / "Common7/Tools/VsDevCmd.bat"
    if not command.is_file():
        raise RuntimeError("An existing MSVC x86 C++ toolchain is required")
    return command


def run(sdk, developer_cmd):
    if os.name != "nt":
        raise RuntimeError("Run on genuine Windows with the x86 MSVC compiler")
    with tempfile.TemporaryDirectory(prefix="skyrim-classic-favorites-") as folder:
        scratch = Path(folder)
        prepare_reference(sdk, scratch)
        batch = scratch / "compare.cmd"
        batch.write_text(
            '@echo off\n'
            'call "%FAVORITES_VS_CMD%" -arch=x86 -host_arch=x64 > nul\n'
            'if errorlevel 1 exit /b 1\n'
            'cl /nologo /std:c11 /W4 /WX %1 /c "%FAVORITES_SOURCE%\\favorite.c" /Fo:query.obj\n'
            'if errorlevel 1 exit /b 1\n'
            'cl /nologo /std:c++17 /EHsc /Zc:twoPhase- /W3 %1 '
            '/I. "%FAVORITES_SOURCE%\\compare_windows.cpp" query.obj /Fe:compare.exe\n'
            'if errorlevel 1 exit /b 1\n'
            'compare.exe\nexit /b %errorlevel%\n')
        env = {**os.environ, "FAVORITES_SOURCE": str(HERE), "FAVORITES_VS_CMD": str(developer_cmd)}
        reports = []
        for optimize in ("/Od", "/O2"):
            proc = subprocess.run(["cmd.exe", "/d", "/c", "compare.cmd", optimize], cwd=scratch,
                                  env=env, capture_output=True, text=True, errors="replace")
            if proc.returncode:
                raise RuntimeError(f"Win32 {optimize} failed\n{proc.stdout}{proc.stderr}")
            report, = [line for line in proc.stdout.splitlines() if line.startswith("favorites ")]
            print(f"Win32 {optimize}: {report}")
            reports.append(report)
        if reports[0] != reports[1]:
            raise RuntimeError("The two Windows builds produced different results")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--official", type=Path, required=True, help="Private extracted Classic 1.7.3 source")
    parser.add_argument("--vsdevcmd", type=Path, help="Optional path to an installed VsDevCmd.bat")
    args = parser.parse_args()
    try:
        run(args.official, args.vsdevcmd or find_vs_command())
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"{exc}\n")
