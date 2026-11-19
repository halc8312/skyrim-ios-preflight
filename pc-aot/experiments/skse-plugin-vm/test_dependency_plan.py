"""Resolve and run only our own i386 test DLLs. Native comparisons run on Windows."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from test_pe_image import BASE, DATA, OPT, RD, SECTIONS, add_named_clock, directory, find_tool, put, zero_string
from test_probe_directory import add_second_import, helper, primary

HERE = Path(__file__).resolve().parent


def branch(slot, child=None, symbol=b"OwnedCheck"):
    data = helper()
    put(data, OPT + 28, "I", BASE + slot * 0x100000)
    if child:
        add_named_clock(data)
        zero_string(data, DATA + 0x40, child.encode("ascii"))
        zero_string(data, DATA + 0xa2, symbol)
    return data


def run(*args, cwd=None):
    answer = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True, timeout=90)
    if answer.returncode:
        raise AssertionError(f"{Path(str(args[0])).name} exited {answer.returncode}: {answer.stderr}{answer.stdout}")
    return answer.stdout


class DependencyPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = find_tool(os.environ.get("CC", "cc"), "clang", "clang-18")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        cls.tools = tempfile.TemporaryDirectory(prefix="skse-graph-tools-")
        cls.addClassCleanup(cls.tools.cleanup)
        cls.binary = Path(cls.tools.name) / ("dependency-plan.exe" if os.name == "nt" else "dependency-plan")
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.name == "nt":
            flags.extend(["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        sources = ("test_dependency_plan.c", "plugin_trial.c", "dependency_plan.c", "pe_image.c", "x86_call.c", "x86_heap.c",
                   "x86_winthread.c", "x86_winfile.c", "x86_winevent.c")
        run(cc, *flags, *(HERE / source for source in sources), "-o", cls.binary)

    def setUp(self):
        self.files = tempfile.TemporaryDirectory(prefix="skse-package-")
        self.addCleanup(self.files.cleanup)
        self.root = Path(self.files.name)

    def place(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def inspect(self, main, *catalog, mode="report"):
        return dict(line.split("=", 1) for line in run(self.binary, mode, main, *catalog).splitlines())

    def refused(self, expected, main, *catalog, requester="", requested=""):
        report = self.inspect(main, *catalog)
        self.assertEqual((report["reason"], report["requester"], report["requested"]),
                         (expected, requester, requested), report)
        for counter in ("attached", "detached", "primary_attach", "query_steps"):
            self.assertEqual(report[counter], "0", report)
        mapped = self.inspect(main, *catalog, mode="map")
        self.assertEqual((mapped["count"], mapped["owned"]), ("0", "0"), mapped)
        return report

    def test_chain_is_independent_of_catalog_order_and_ignores_unreferenced_files(self):
        main = self.place("Main.dll", primary(b"Branch.dll"))
        root_child = self.place("BRANCH.dll", branch(1, "leaf.dll"))
        leaf = self.place("Leaf.dll", branch(2))
        unused = self.place("Broken.dll", b"invalid but unreachable")
        first = self.inspect(main, root_child, unused, leaf)
        self.assertEqual(first, self.inspect(main, leaf, root_child, unused))
        self.assertEqual((first["reason"], first["order"], first["phase"], first["direct"]),
                         ("ready", "Leaf.dll,BRANCH.dll", "0", "1"), first)
        self.assertEqual((first["name"], first["version"]), ("Owned test plugin", "7"))

    def test_shared_diamond_and_duplicate_import_descriptor_are_resolved_once(self):
        data = primary(b"A.dll")
        add_second_import(data, b"B.dll")
        main = self.place("Main.dll", data)
        a = self.place("A.dll", branch(1, "Leaf.dll"))
        b = self.place("B.dll", branch(2, "Leaf.dll"))
        leaf = self.place("Leaf.dll", branch(3))
        report = self.inspect(main, b, leaf, a)
        self.assertEqual((report["reason"], report["phase"], report["order"], report["direct"]),
                         ("ready", "0", "Leaf.dll,A.dll,B.dll", "2"), report)
        add_second_import(data, b"a.DLL")
        report = self.inspect(self.place("Main.dll", data), a, leaf)
        self.assertEqual((report["phase"], report["order"], report["direct"]), ("0", "Leaf.dll,A.dll", "1"), report)

    def test_missing_unreadable_ambiguous_and_forbidden_names(self):
        main = self.place("Main.dll", primary(b"A.dll"))
        a = self.place("A.dll", branch(1, "Leaf.dll"))
        self.refused("missing", main, requested="A.dll")
        self.refused("missing", main, a, requester="A.dll", requested="Leaf.dll")
        self.refused("unreadable", main, a, self.root / "Leaf.dll", requester="A.dll", requested="Leaf.dll")
        a2 = self.place("other/a.DLL", branch(2))
        self.refused("ambiguous", main, a, a2, requested="A.dll")
        for invalid in (b"../A.dll", b"sub\\A.dll", b"other.sys"):
            self.refused("name", self.place("Main.dll", primary(invalid)), requested=invalid.decode())

    def test_cycles_self_import_and_helper_limit_stop_before_attach(self):
        main = self.place("Main.dll", primary(b"A.dll"))
        a = self.place("A.dll", branch(1, "a.dll"))
        self.refused("self", main, a, requester="A.dll", requested="a.dll")
        a = self.place("A.dll", branch(1, "Main.dll"))
        self.refused("cycle", main, a, requester="A.dll", requested="Main.dll")
        a = self.place("A.dll", branch(1, "B.dll"))
        b = self.place("B.dll", branch(2, "A.dll"))
        self.refused("cycle", main, a, b, requester="B.dll", requested="A.dll")
        b = self.place("B.dll", branch(2, "C.dll"))
        c = self.place("C.dll", branch(3, "D.dll"))
        d = self.place("D.dll", branch(4))
        self.refused("limit", main, a, b, c, d, requester="C.dll", requested="D.dll")
        self.refused("self", self.place("Main.dll", primary(b"MAIN.dll")), requested="MAIN.dll")

    def test_all_exports_imports_permissions_and_addresses_checked_before_attach(self):
        root = primary(b"A.dll")
        add_second_import(root, b"B.dll")
        main = self.place("Main.dll", root)
        a = self.place("A.dll", branch(1))
        for change, requester, target in [
            (lambda d: zero_string(d, RD + 0x80, b"OtherExport"), "B.dll", "B.dll"),
            (lambda d: put(d, RD + 0x60, "I", 0x2040), "B.dll", "B.dll"),
            (lambda d: put(d, SECTIONS + 36, "I", 0xe0000020), "B.dll", "B.dll"),
        ]:
            broken = branch(2)
            change(broken)
            self.refused("image", main, a, self.place("B.dll", broken), requester=requester, requested=target)
        b = self.place("B.dll", branch(1))
        self.refused("address", main, a, b, requester="B.dll", requested="B.dll")
        bad_system = branch(2, "KERNEL32.dll", b"NotAnApprovedService")
        report = self.refused("image", main, a, self.place("B.dll", bad_system), requester="B.dll", requested="KERNEL32.dll")
        self.assertEqual(report["pe"], "import absent from the service list")
        by_ordinal = branch(2, "A.dll")
        for at in (0x60, 0x80):
            put(by_ordinal, DATA + at, "I", 0x80000001)
        report = self.refused("image", main, a, self.place("B.dll", by_ordinal), requester="B.dll", requested="A.dll")
        self.assertIn("ordinal", report["pe"].lower())

    def test_sum_of_virtual_image_sizes_is_bounded_before_mapping(self):
        main_data = primary(b"A.dll")
        a_data = branch(1, "B.dll")
        b_data = branch(2)
        for data in (main_data, a_data, b_data):
            put(data, OPT + 56, "I", 24 * 1024 * 1024)
        main = self.place("Main.dll", main_data)
        a, b = self.place("A.dll", a_data), self.place("B.dll", b_data)
        self.refused("size", main, a, b, requester="B.dll", requested="B.dll")

    def test_invalid_service_declarations_and_oversize_primary_leave_no_mappings(self):
        main = self.place("Main.dll", primary(b"A.dll"))
        self.assertEqual(self.inspect(main, mode="contracts"), {"input_contracts": "5"})

    def test_invalid_metadata_pointer_or_unterminated_name_never_escapes_the_selected_images(self):
        a = self.place("A.dll", branch(1))
        for address in (0xf20000, BASE + 0x4000, BASE + 0x2150):
            data = primary(b"A.dll")
            put(data, data.index(struct.pack("<I", BASE + 0x20c4)), "I", address)
            if address == BASE + 0x2150:
                data[RD + 0x150:RD + 0x1f0] = b"X" * 0xa0
            report = self.inspect(self.place("Main.dll", data), a)
            self.assertEqual(report["reason"], "ready")
            self.assertNotEqual(report["phase"], "0")
            self.assertEqual(report["version"], "0")
            self.assertGreater(int(report["query_steps"]), 0)

    def build_diamond(self, opt, *, declining=False, chain=False):
        cc, ld = find_tool("clang", "clang-18"), find_tool("lld-link", "lld-link-18")
        if not ld and sys.platform == "linux" and shutil.which("wslpath"):
            fallback = Path("/mnt/c/Program Files/LLVM/bin/lld-link.exe")
            if fallback.is_file():
                ld = str(fallback)
        if not cc or not ld:
            self.skipTest("LLVM Clang and COFF linker required for the four actual i386 images")
        stage = self.root / (opt + ("-decline" if declining else "") + ("-chain" if chain else ""))
        stage.mkdir()
        b_imports = ("PlanLeaf.lib", "PlanA.lib") if chain else ("PlanLeaf.lib",)
        units = (("PlanLeaf", ()), ("PlanA", ("PlanLeaf.lib",)), ("PlanB", b_imports),
                 ("fixture", ("PlanA.lib", "PlanB.lib")))
        for n, (name, dependencies) in enumerate(units, start=1):
            flags = ["-DDECLINE_GRAPH_BRANCH"] if declining and n == 3 else []
            if chain:
                flags.append("-DGRAPH_CHAIN")
            run(cc, "--target=i686-pc-windows-msvc", "-ffreestanding", "-fno-stack-protector", opt,
                f"-DGRAPH_MODULE={n}", *flags, "-c", HERE / "test_dependency_graph_fixture.c", "-o", f"{name}.obj", cwd=stage)
            run(ld, "/dll", "/nodefaultlib", "/machine:x86", "/base:0x10000000", "/entry:GraphEntry@12",
                f"/out:{name}.dll", f"/implib:{name}.lib", f"{name}.obj", *dependencies, cwd=stage)
        return cc, stage, [stage / f"{n}.dll" for n in ("fixture", "PlanB", "PlanLeaf", "PlanA")]

    def test_authored_windows_diamond_relocations_values_lifecycle_and_native_loader(self):
        for opt in ("-O0", "-O2"):
            with self.subTest(opt=opt):
                cc, stage, images = self.build_diamond(opt)
                report = self.inspect(*images)
                self.assertEqual(report, self.inspect(images[0], *reversed(images[1:])))
                self.assertEqual(report, self.inspect(*images, mode="moved"))
                self.assertEqual((report["reason"], report["phase"], report["name"], report["version"]),
                                 ("ready", "0", "Package-local diamond", "1329"), report)
                self.assertEqual((report["direct"], report["attached"], report["detached"]), ("2", "3", "3"))
                self.assertEqual(report["order"].split(",")[0], "PlanLeaf.dll")
                sequence = int(report["sequence"])
                self.assertIn(sequence, (12345768, 13245678))
                table = run(self.binary, "table", *images)
                self.assertEqual(len(table.splitlines()), 8)
                self.assertIn("0,1,1,1329,Package-local diamond,1,1", table)
                if sys.platform == "win32":
                    for source, destination in (("test_native_windows.c", "native.exe"),
                                                ("test_dependency_order_windows.c", "order.exe")):
                        run(cc, "--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-std=c11", HERE / source,
                            "-o", destination, cwd=stage)
                    self.assertEqual(table, run(stage / "native.exe", cwd=stage))
                    self.assertEqual(sequence, int(run(stage / "order.exe", cwd=stage).strip()))

    def test_authored_declining_helper_unwinds_and_never_starts_root(self):
        _, _, images = self.build_diamond("-O2", declining=True)
        rep = self.inspect(*images)
        self.assertEqual((rep["reason"], rep["failing"], rep["query_steps"], rep["primary_attach"]),
                         ("ready", "PlanB.dll", "0", "0"), rep)
        self.assertNotEqual(rep["phase"], "0")
        self.assertEqual(int(rep["detached"]), int(rep["attached"]) + 1)
        self.assertGreater(int(rep["attached"]), 0)

    def test_authored_multiply_referenced_chain_detaches_consumers_before_providers(self):
        cc, stage, images = self.build_diamond("-O2", chain=True)
        report = self.inspect(*images)
        self.assertEqual((report["phase"], report["order"], report["version"], report["sequence"]),
                         ("0", "PlanLeaf.dll,PlanA.dll,PlanB.dll", "1329", "12345678"), report)
        if sys.platform == "win32":
            run(cc, "--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-std=c11", HERE / "test_dependency_order_windows.c",
                "-o", "order.exe", cwd=stage)
            self.assertEqual(int(report["sequence"]), int(run(stage / "order.exe", cwd=stage).strip()))


if __name__ == "__main__":
    unittest.main()
