"""Stage the reviewed LE input observer in a native fixture."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation

INPUT_HASH = "e7545db20bf742f2981b3c73299ee8567cd3c2ddfe4e9f4c11ed55916094591d"
BUTTON_UNIT_HASH = "2465e7efd646217c9359f32419d3a7b523898206a76070d3e4bc919575c683c7"
VM_UNIT_HASH = "f550390f7023b2375da3746cfd6b99be32bfd4fecabff2ce5396b73e2e18bf3c"
VM_HOOK_HASHES = {
    "skse_compat.c": "ab9e96a3b330942c8f5623dd18af96889be243dcd3ae7fb9bbde711458bc96b4",
    "skse_compat.h": "306883571cc167ee22636905a45ce3af95e779d9f6cefbb414a394c4764c9eaa",
    "skse_system.c": "61ce22b226c98264382722d0bc54d5835b32ae4821466d1f6a5b91e7af04b07c",
}
GFX_CALL = b'        skse_gfx_input_event(c,key,control?control:"",down);\n'
CONTROL_CAPTURE = b"        skyrim_probe_control_edge(c,M32(event+0x10),down,MF32(event+0x1c));\n"
HOST_HEADER = b'''#include "lift_prelude.h"
#include <stddef.h>
#define ARG(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, nargs) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (nargs); return; } while (0)
static inline char *GSTR(uint32_t a) { return a ? (char *)(MEM + a) : NULL; }
void skse_gfx_input_event(CPU *, unsigned, const char *, int);
void skyrim_probe_control_edge(CPU *, uint32_t, int, float);
'''
RUNTIME_HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}


def verified(contents, digest):
    if hashlib.sha256(contents).hexdigest() != digest:
        raise ValueError("Input differs from the reviewed Classic source.")
    return contents


def observer_body(source):
    verified(source, INPUT_HASH)
    start = source.index(b"static unsigned macro_key(")
    end = source.index(b"void skse_input_install(", start)
    body = source[start:end]
    if body.count(GFX_CALL) != 1:
        raise ValueError("Expected one mapped-input callback in the reviewed observer.")
    return body


def staged_observer(body, wrapper, observe):
    if observe:
        body = body.replace(GFX_CALL, CONTROL_CAPTURE + GFX_CALL, 1)
    entry = f"void {wrapper}(CPU *c) {{ sink_event(c); }}\n".encode("ascii")
    return HOST_HEADER + body + entry


def control_accessor(source):
    return lifted_function(verified(source, BUTTON_UNIT_HASH), "f_667000")


def lifted_function(contents, name):
    marker = f"\nvoid {name}(CPU *c) {{\n".encode("ascii")
    if contents.count(marker) != 1:
        raise ValueError(f"Expected one original lifted {name} entry.")
    start = contents.index(marker)
    return contents[start:contents.index(b"\n}\n", start) + 3]


def staged_form_delete(contents):
    original = lifted_function(verified(contents, VM_UNIT_HASH), "f_8D6550")
    call = b"    esp -= 4; M32(esp) = 0x8D657Fu; SPILL(); f_8D4C40(c); RELOAD();"
    if original.count(call) != 1:
        raise ValueError("Expected one reviewed VM sleep-unregister call.")
    declarations = b"""
void f_8D1E00(CPU *);
void f_8D1EB0(CPU *);
void f_8D11A0(CPU *);
void f_8D4C40(CPU *);
void f_8D4D30(CPU *);
void f_8D64E0(CPU *);
void f_8AF310(CPU *);
void skyrim_probe_form_delete(CPU *, uint32_t, uint64_t);
"""
    observed = original.replace(b"void f_8D6550(", b"void skyrim_probe_delete_adapted(", 1)
    observed = observed.replace(call, b"""    esp -= 4; M32(esp) = 0x8D657Fu; SPILL();
    const uint32_t deletion_vm = c->r[1];
    const uint64_t deleted_handle = M64(c->r[4] + 4);
    f_8D4C40(c);
    skyrim_probe_form_delete(c, deletion_vm, deleted_handle);
    RELOAD();""", 1)
    previous = original.replace(b"void f_8D6550(", b"void skyrim_probe_delete_original(", 1)
    return b'#include "lift_prelude.h"\n' + declarations + previous + observed


def staged_registration(runtime):
    sources = {name: verified((runtime / name).read_bytes(), digest) for name, digest in VM_HOOK_HASHES.items()}
    abi = sources["skse_compat.c"]
    begin = abi.index(b"enum { VM_NATIVE_CTOR=")
    end = abi.index(b"uint32_t skse_member(", begin)
    constants = abi[begin:end]
    begin = abi.index(b"static void run_native(CPU *c)")
    end = abi.index(b"/* Optional integration probe:", begin)
    callback = abi[begin:end]
    system = sources["skse_system.c"]
    begin = system.index(b"static uint64_t base_handle(")
    end = system.index(b"static uint32_t resolve_form(", begin)
    methods = system[begin:end]
    declarations = b"""#include "skse_compat.h"
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
uint32_t skse_member(CPU *, uint32_t, uint32_t, int, const uint32_t *);
uint32_t skse_virtual(CPU *, uint32_t, unsigned, int, const uint32_t *);
int skse_system_run(CPU *, const SkseNative *, uint32_t, const uint32_t *, uint32_t, uint32_t);
int skse_skyui_enabled(void);
uint32_t gstrdup(const char *);
void gfree(uint32_t);
"""
    entries = b"""
void skyrim_probe_control_native(CPU *cpu) { run_native(cpu); }
uint64_t skyrim_probe_base_handle(uint32_t base) { return base_handle(base); }
void skyrim_probe_handle_ref(CPU *cpu, uint64_t handle, int add) { handle_ref(cpu,handle,add); }
"""
    return HOST_HEADER + declarations + constants + callback + methods + entries


class ClassicControlEventTests(unittest.TestCase):
    def test_unreviewed_input_observer_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "reviewed Classic source"):
            observer_body(b"authored input event\n" + GFX_CALL)

    def reviewed_generation(self):
        setting = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not setting:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation.")
        generation = Path(setting)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        if (state.get("schema") != 1 or state.get("mode") != "original-guest-bodies"
                or state.get("fixed_native_replacements") is not False
                or state.get("generation_finished") is not True or state.get("inputs_unchanged") is not True):
            raise ValueError("Requires a completed, unchanged original-body generation.")
        runtime = generation / "source" / "runtime"
        for name, digest in RUNTIME_HEADERS.items():
            verified((runtime / name).read_bytes(), digest)
        return generation, runtime

    def run_fixture(self, runtime, scratch, sources):
        windows = sys.platform == "win32"
        compiler = shutil.which(os.environ.get("CC", "cc"))
        llvm = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin")) / "clang.exe"
        if windows and not compiler and llvm.is_file():
            compiler = str(llvm)
        if not compiler:
            self.skipTest("A C compiler is required.")
        native = ROOT / "native"
        args = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-Wno-unused-label", "-isystem", str(runtime), f"-I{native}"]
        if windows:
            args.append("-fuse-ld=lld")
        if sys.platform.startswith("linux"):
            args.append("-D_DEFAULT_SOURCE")
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                args.extend(["-fno-pie", "-no-pie"])
        binary = scratch / ("controls.exe" if windows else "controls")
        sources = [*sources, native / "guest_memory.c"]
        subprocess.run([*args, *map(str, sources), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)

    def test_reviewed_button_events_preserve_original_observer(self):
        generation, runtime = self.reviewed_generation()
        source = runtime / "skse_input.c"
        original = source.read_bytes()
        body = observer_body(original)
        accessor = control_accessor((generation / "gen" / "exe_98.c").read_bytes())
        with tempfile.TemporaryDirectory(prefix="classic-control-events-") as scratch:
            temporary = Path(scratch)
            previous = temporary / "original_observer.c"
            adapted = temporary / "control_observer.c"
            previous.write_bytes(staged_observer(body, "skyrim_probe_original_input", False))
            adapted.write_bytes(staged_observer(body, "skyrim_probe_control_input", True) + accessor)
            self.run_fixture(runtime, temporary, [HERE / "test_control_events.c", previous, adapted])
        self.assertEqual(source.read_bytes(), original)

    def test_registration_vm_delivery_and_original_form_delete(self):
        generation, runtime = self.reviewed_generation()
        body = observer_body((runtime / "skse_input.c").read_bytes())
        vm_unit = generation / "gen" / "exe_199.c"
        delete = vm_unit.read_bytes()
        with tempfile.TemporaryDirectory(prefix="classic-control-registration-") as scratch:
            temporary = Path(scratch)
            native = temporary / "control_native.c"
            adapted = temporary / "control_observer.c"
            form_delete = temporary / "form_delete.c"
            native.write_bytes(staged_registration(runtime))
            adapted.write_bytes(staged_observer(body, "skyrim_probe_control_input", True))
            form_delete.write_bytes(staged_form_delete(delete))
            self.run_fixture(runtime, temporary, [
                HERE / "test_control_registration.c", HERE / "control_bridge.c", native, adapted, form_delete,
            ])
        self.assertEqual(vm_unit.read_bytes(), delete)


if __name__ == "__main__":
    unittest.main()
