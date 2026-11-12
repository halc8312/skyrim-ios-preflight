"""Game-free CMake source-property regression; no desktop/iOS compile or link.

Evaluate two verbatim command spans from the pinned upstream CMakeLists using
synthetic source files. Unrelated dependency discovery and game generation are
outside this fixture. C/C++ languages stay disabled; Ninja generates no builds.
"""
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
PATCH = ROOT / "patches/skydream/0004-preserve-native-audio-source.patch"
SOURCES = ("runtime/host.c", "runtime/x3daudio_native.c",
           "runtime/skse_compat_values.c", "runtime/keep.c", "runtime/keep.cpp",
           "third_party/f3daudio/F3DAudio.c")


def command(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(f"Command failed: {args!r}\n{result.stdout}\n{result.stderr}")
    return result


def prepare_sources(folder):
    """Canonical pinned content, isolated patch application; vendor is read-only."""
    result = subprocess.run(
        ["git", "-c", "core.autocrlf=false", "-C", str(ROOT / "vendor/skydream"),
         "archive", PIN, "desktop/CMakeLists.txt"], capture_output=True, timeout=30)
    if result.returncode:
        raise unittest.SkipTest("Pinned source unavailable; this test never downloads it")
    paths = []
    for name in ("baseline", "patched"):
        path = folder / name
        path.mkdir()
        with tarfile.open(fileobj=io.BytesIO(result.stdout)) as archive:
            archive.extractall(path, filter="data")
        paths.append(path)
    changed = paths[1]
    command(["git", "-c", "core.autocrlf=false", "init", "--quiet"], changed)
    for flags in (["--check"], []):
        command(["git", "-c", "core.autocrlf=false", "apply", *flags, "--", str(PATCH)], changed)
    return paths


def extracted_commands(source):
    text = source.read_text(encoding="utf-8")
    # Exact upstream boundaries: fail if upstream layout changes unexpectedly.
    boundaries = ('file(GLOB RT_SRC ', 'add_library(skydream_host ',
                  'add_library(lb_runtime ', 'target_compile_options(lb_runtime ')
    for boundary in boundaries:
        if text.count(boundary) != 1:
            raise AssertionError(f"Ambiguous or missing upstream boundary: {boundary}")
    return (text[text.index(boundaries[0]):text.index(boundaries[1])]
            + text[text.index(boundaries[2]):text.index(boundaries[3])])


def evaluate(source, folder, skse, skyui=False):
    """Retain fixture, raw configure log and evaluated target properties in folder."""
    folder.mkdir()
    port = folder / "port"
    for name in SOURCES:
        path = port / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("/* Synthetic placeholder; never compiled. */\n")
    content = ('cmake_minimum_required(VERSION 3.18)\n'
               'project(AudioSourceProperties NONE)\n'
               'set(PORT "${CMAKE_CURRENT_SOURCE_DIR}/port")\n'
               + extracted_commands(source)
               + '\nset_target_properties(lb_runtime PROPERTIES LINKER_LANGUAGE C)\n'
               'get_target_property(actual_sources lb_runtime SOURCES)\n'
               'get_target_property(actual_definitions lb_runtime COMPILE_DEFINITIONS)\n'
               'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sources.txt" "${actual_sources}")\n'
               'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/definitions.txt" "${actual_definitions}")\n'
               'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rt_sources.txt" "${RT_SRC}")\n')
    (folder / "CMakeLists.txt").write_text(content, encoding="utf-8")
    args = [shutil.which("cmake"), "-S", str(folder), "-B", str(folder / "build"),
            "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={Path(shutil.which('ninja')).as_posix()}",
            f"-DSKYDREAM_SKSE={'ON' if skse else 'OFF'}",
            f"-DSKYDREAM_SKYUI={'ON' if skyui else 'OFF'}"]
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    (folder / "configure.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise AssertionError(f"CMake fixture failed: {result.stdout}\n{result.stderr}")
    def paths(name):
        return [Path(item).relative_to(port).as_posix()
                for item in (folder / "build" / name).read_text().split(";")]
    return {"sources": paths("sources.txt"), "runtime": paths("rt_sources.txt"),
            "definitions": (folder / "build/definitions.txt").read_text().split(";")}


@unittest.skipUnless(all(shutil.which(tool) for tool in ("git", "cmake", "ninja")),
                     "Existing Git, CMake and Ninja required; no installation")
class DesktopAudioSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.folder = Path(cls.temp.name)
        cls.baseline, cls.patched = prepare_sources(cls.folder)

    def check_sources(self, result, skse, audio, skyui=False):
        expected_runtime = {name for name in SOURCES if name.startswith("runtime/")}
        expected_runtime.remove("runtime/host.c")
        if not skse:
            expected_runtime.remove("runtime/skse_compat_values.c")
        expected = expected_runtime | ({SOURCES[-1]} if audio else set())
        self.assertCountEqual(result["runtime"], expected_runtime)
        self.assertCountEqual(result["sources"], expected)
        definitions = {"GAME_SKYRIM", "DXVK_NATIVE", "_FILE_OFFSET_BITS=64", "SDL_MAIN_HANDLED"}
        if skse:
            definitions.add("SKYDREAM_SKSE=1")
        if skyui:
            definitions.add("SKYDREAM_SKYUI=1")
        self.assertCountEqual(result["definitions"], definitions)

    def test_baseline_reproduces_audio_omission_only_when_skse_off(self):
        for skse in (False, True):
            with self.subTest(skse=skse):
                result = evaluate(self.baseline / "desktop/CMakeLists.txt",
                                  self.folder / f"baseline-{skse}", skse)
                self.check_sources(result, skse, audio=skse)

    def test_patch_preserves_audio_and_filters_with_both_skse_modes(self):
        for skse, skyui in ((False, False), (True, False), (True, True)):
            with self.subTest(skse=skse, skyui=skyui):
                result = evaluate(self.patched / "desktop/CMakeLists.txt",
                                  self.folder / f"patched-{skse}-{skyui}", skse, skyui)
                self.check_sources(result, skse, audio=True, skyui=skyui)


if __name__ == "__main__":
    unittest.main()
