"""Portable guest tool selection and fail-closed build orchestration.

Fixtures contain no game code. Apple commands are mocked here; a macOS run
with the real SDK remains necessary before claiming native guest validation.
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location("title_guest_builder",ROOT/"scripts/build_title_guest.py")
builder=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class ToolchainTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve()
        self.llvm=self.root/"LLVM bin"
        self.llvm.mkdir()
        for name in ("clang.exe","llvm-nm.exe","ld64.lld.exe","clang","nm"):
            (self.llvm/name).write_bytes(b"synthetic tool")
        self.resource=self.root/"resource"
        (self.resource/"include").mkdir(parents=True)
        (self.resource/"include/stddef.h").write_text("/* synthetic */")
        self.headers=self.root/"public headers"
        for name in builder.REQUIRED_DARWIN_HEADERS:
            p=self.headers/name
            p.parent.mkdir(parents=True,exist_ok=True)
            p.write_text("/* synthetic */")

    def windows_output(self,args):
        return str(self.resource) if "-print-resource-dir" in args else "LLD synthetic 1"

    def test_windows_explicit_paths_and_changed_header_invalidate_identity(self):
        with mock.patch.object(builder.sys,"platform","win32"),mock.patch.object(builder,"output",side_effect=self.windows_output):
            first=builder.toolchain(self.llvm,self.headers)
            self.assertFalse(first["official_apple_sdk_used"])
            self.assertEqual(first["nm_flags"],["--format=posix"])
            self.assertIn("-nostdlibinc",first["compile_flags"])
            self.assertEqual(first["compile_flags"][-1],str(self.headers))
            (self.headers/"math.h").write_text("/* different synthetic header */")
            second=builder.toolchain(self.llvm,self.headers)
            self.assertNotEqual(first["identity"],second["identity"])

    def test_windows_missing_public_headers_fails_with_actionable_message(self):
        with mock.patch.object(builder.sys,"platform","win32"),mock.patch.dict(builder.os.environ,{},clear=True):
            with self.assertRaisesRegex(ValueError,"--darwin-headers"):
                builder.toolchain(self.llvm)
            with self.assertRaisesRegex(ValueError,"complete public"):
                builder.toolchain(self.llvm,self.root/"absent")

    def test_macos_resolves_official_iphoneos_sdk_and_nm(self):
        sdk=self.root/"iPhoneOS.sdk"
        sdk.mkdir()
        calls=[]
        def answer(args):
            calls.append(args)
            if args[-1]=="--show-sdk-path": return str(sdk)
            if args[-1]=="--show-sdk-version": return "26.5"
            if args[-1]=="--show-sdk-build-version": return "23Fsynthetic"
            if args[-2:]==["--find","clang"]: return str(self.llvm/"clang")
            if args[-2:]==["--find","nm"]: return str(self.llvm/"nm")
            self.fail(str(args))
        with mock.patch.object(builder.sys,"platform","darwin"),mock.patch.object(builder.shutil,"which",return_value="xcrun"),mock.patch.object(builder,"output",side_effect=answer):
            tool=builder.toolchain()
        self.assertTrue(tool["official_apple_sdk_used"])
        self.assertEqual(tool["nm_flags"],["-P"])
        self.assertIn("-isysroot",tool["compile_flags"])
        self.assertNotIn("-nostdlibinc",tool["compile_flags"])
        self.assertEqual(tool["identity"]["sdk_build"],"23Fsynthetic")
        self.assertTrue(all(c[1:3]==["--sdk","iphoneos"] for c in calls))
        with mock.patch.object(builder.sys,"platform","darwin"):
            with self.assertRaisesRegex(ValueError,"Windows options"):
                builder.toolchain(self.llvm,self.headers)

    def test_link_uses_sdk_libsystem_and_preserves_path_arguments(self):
        build=self.root/"build with space,comma"
        build.mkdir()
        response,exports,binary=(build/n for n in ("objects.rsp","exports.txt","SkyrimGuest"))
        apple={"official_apple_sdk_used":True,"clang":self.llvm/"clang","sdk":self.root/"SDK"}
        cmd=builder.link_command(apple,build,response,exports,binary,["_memcpy"])
        self.assertIn("-lSystem",cmd)
        self.assertNotIn("-L",cmd)
        self.assertIn(str(response),cmd)
        self.assertEqual(cmd[cmd.index(str(response))-1],"-Xlinker")
        self.assertIn("-no_adhoc_codesign",cmd)
        self.assertFalse((build/"libSystem.tbd").exists())
        windows={"official_apple_sdk_used":False,"linker":self.llvm/"ld64.lld.exe"}
        cmd=builder.link_command(windows,build,response,exports,binary,["_memcpy"])
        self.assertEqual(cmd[0],windows["linker"])
        self.assertIn("_memcpy",(build/"libSystem.tbd").read_text())
        self.assertEqual(cmd[cmd.index("-undefined")+1],"error")

    def test_strict_imports_allow_cross_object_definitions_only(self):
        tool={"nm":"nm","nm_flags":["-P"]}
        with mock.patch.object(builder,"output",side_effect=["_helper U\n_memcpy U\n_accidental U\n", "_helper T 0 4\n"]):
            imports,unknown=builder.import_symbols(tool,["a.o","b.o"])
        self.assertEqual(imports,["_accidental","_memcpy"])
        self.assertEqual(unknown,["_accidental"])


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve()
        self.stage=self.root/"private/stage"
        runtime=self.stage/"source/runtime"
        runtime.mkdir(parents=True)
        gen=self.stage/"gen"
        gen.mkdir()
        (gen/"exe_0.c").write_text("/* synthetic, not game code */")
        self.abi=self.root/"abi"
        self.abi.mkdir()
        (self.abi/"PcGuestABI.h").write_text("/* synthetic ABI */")
        (self.abi/"pc_guest_library.c").write_text("/* synthetic */")
        headers={}
        for name in ("x86.h","lift_prelude.h","rt_table.h"):
            p=runtime/name
            p.write_text("/* synthetic */")
            headers[name]=builder.digest(p)
        self.state={"generation_finished":True,"inputs_unchanged":True,
                    "fixed_native_replacements":False,"source_headers_sha256":headers,
                    "candidate_sha256":"a"*64,"discovery_sha256":"b"*64,
                    "source_commit":"synthetic"}
        self.state_path=self.stage/"generation.json"
        self.state_path.write_text(json.dumps(self.state))
        self.tool={"clang":Path("synthetic-clang"),"nm":Path("synthetic-nm"),
                   "nm_flags":["-P"],"sdk":self.root/"SDK",
                   "compile_flags":["--target="+builder.TARGET],
                   "identity":{"kind":"apple-sdk","sdk_build":"A"},
                   "official_apple_sdk_used":True}
        self.calls=[]
        self.stack=contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(builder,"ROOT",self.root))
        self.stack.enter_context(mock.patch.object(builder,"ABI",self.abi))
        self.stack.enter_context(mock.patch.object(builder,"toolchain",return_value=self.tool))
        self.stack.enter_context(mock.patch.object(builder,"output",return_value="Synthetic clang 1"))
        self.import_mock=self.stack.enter_context(mock.patch.object(builder,"import_symbols",return_value=(["_memcpy"],[])))
        self.stack.enter_context(mock.patch.object(builder,"command",side_effect=self.fake_command))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def fake_command(self,args,log,timeout=900):
        self.calls.append(args)
        Path(args[-1]).write_bytes(b"synthetic object or framework")

    def build(self):
        builder.main([str(self.stage),"--jobs","1"])

    def test_build_and_reuse_objects_then_reject_changed_sdk(self):
        self.build()
        self.assertEqual(sum("-c" in c for c in self.calls),2)
        result=json.loads((self.stage/"arm64-ios/result.json").read_text())
        self.assertTrue(result["official_apple_sdk_used"])
        self.assertEqual(result["candidate_sha256"],self.state["candidate_sha256"])
        self.assertEqual(result["discovery_sha256"],self.state["discovery_sha256"])
        self.assertFalse(result["signed"])
        self.assertFalse(result["physical_iphone_verified"])
        self.calls.clear()
        self.build()
        self.assertEqual(sum("-c" in c for c in self.calls),0)
        self.calls.clear()
        self.tool["identity"]["sdk_build"]="B"
        with self.assertRaisesRegex(ValueError,"Compilation settings changed"):
            self.build()
        self.assertEqual(self.calls,[])

    def test_classic_variant_uses_separate_objects_bindings_and_framework_identity(self):
        import plistlib
        with mock.patch.object(sys, "path", [str(ROOT / "scripts"), *sys.path]):
            import classic_skse_guest as classic
            self.state.update(source_commit=classic.PIN, candidate_sha256=classic.CANDIDATE_SHA,
                              discovery_sha256=classic.DISCOVERY_SHA)
            self.state_path.write_text(json.dumps(self.state))
            gen = self.stage / "gen"
            declarations = gen / "funcs_exe.h"
            declarations.write_bytes(b"/* authored declarations */")
            (gen / "exe_0.c").write_bytes(b"\n".join(
                b"void f_" + name + b"(CPU *c) { return; }" for name in classic.ENTRIES))
            (gen / "exe_table.c").write_bytes(b"\n".join(
                b"{0x" + name + b"u, f_" + name + b"}," for name in classic.ENTRIES))
            (self.abi / "PcGuestSkseABI.h").write_bytes(b"authored experimental ABI")
            (self.abi / "pc_guest_skse_library.c").write_bytes(b"/* test */")
            self.build()
            self.calls.clear()
            builder.main([str(self.stage), "--jobs", "1", "--classic-skse"])
            self.calls.clear()
            builder.main([str(self.stage), "--jobs", "1", "--classic-skse"])
            self.assertFalse(any("-c" in cmd for cmd in self.calls))
            declarations.write_bytes(b"/* changed declarations */")
            self.calls.clear()
            builder.main([str(self.stage), "--jobs", "1", "--classic-skse"])
            self.assertEqual(sum("-c" in cmd for cmd in self.calls), 4)
        extra = self.stage / "arm64-ios-classic-skse"
        normal = self.stage / "arm64-ios"
        self.assertNotIn("classic_skse_experiment", json.loads((normal / "result.json").read_text()))
        self.assertIs(json.loads((extra / "result.json").read_text())["classic_skse_experiment"], True)
        self.assertIs(plistlib.loads((extra / "SkyrimGuest.framework/Info.plist").read_bytes())
                      ["PcGuestClassicSkseExperiment"], True)
        self.assertEqual(set((extra / "exports.txt").read_text().splitlines()),
                         {"_skyrim_guest_classic_api_v1", "_skyrim_guest_bind_classic_v1"})
        for cmd in (c for c in self.calls if "-c" in c):
            is_host_binding = Path(cmd[cmd.index("-c")+1]).parent == self.abi
            self.assertEqual("-DSKYRIM_IOS_CLASSIC_SKSE=1" in cmd, is_host_binding)

    def test_unknown_import_blocks_link_and_success_result(self):
        self.import_mock.return_value=(["_not_allowed"],["_not_allowed"])
        with self.assertRaisesRegex(RuntimeError,"Unexpected private framework imports"):
            self.build()
        self.assertTrue(all("-c" in c for c in self.calls))
        self.assertFalse((self.stage/"arm64-ios/result.json").exists())

    def test_missing_generated_sources_cannot_build_api_only_framework(self):
        (self.stage/"gen/exe_0.c").unlink()
        with self.assertRaisesRegex(ValueError,"Generated exe_\\*\\.c sources are missing"):
            self.build()
        self.assertEqual(self.calls,[])
        self.assertFalse((self.stage/"arm64-ios/result.json").exists())

    def test_generation_without_exact_input_binding_cannot_compile(self):
        del self.state["candidate_sha256"]
        self.state_path.write_text(json.dumps(self.state))
        with self.assertRaisesRegex(ValueError,"exact candidate_sha256"):
            self.build()
        self.assertEqual(self.calls,[])

    def test_incomplete_generation_or_changed_abi_cannot_compile(self):
        self.state["generation_finished"]=False
        self.state_path.write_text(json.dumps(self.state))
        with self.assertRaisesRegex(ValueError,"Completed original-body"):
            self.build()
        self.state["generation_finished"]=True
        self.state_path.write_text(json.dumps(self.state))
        (self.stage/"source/runtime/x86.h").write_text("/* changed */")
        with self.assertRaisesRegex(ValueError,"ABI header changed"):
            self.build()
        self.assertEqual(self.calls,[])

    def test_old_contract_cannot_silently_reuse_unbound_toolchain(self):
        self.build()
        contract_path=self.stage/"arm64-ios/compile-contract.json"
        old=json.loads(contract_path.read_text())
        del old["toolchain"]
        contract_path.write_text(json.dumps(old))
        self.calls.clear()
        with self.assertRaisesRegex(ValueError,"Compilation settings changed"):
            self.build()
        self.assertEqual(self.calls,[])


@unittest.skipUnless(sys.platform=="darwin" and shutil.which("xcrun"),
                     "Actual Apple SDK guest smoke requires macOS and Xcode")
class AppleSDKSmokeTests(unittest.TestCase):
    def test_synthetic_math_memory_guest_compiles_and_strictly_links_for_ios(self):
        # No game input, signing identity, network or additional package is used.
        try:
            builder.output(["xcrun","--sdk","iphoneos","--show-sdk-path"])
        except (OSError,subprocess.SubprocessError):
            self.skipTest("The selected Xcode does not provide the iPhoneOS SDK")
        tool=builder.toolchain()
        with tempfile.TemporaryDirectory(prefix="skyrim-guest-sdk-") as temporary:
            work=Path(temporary).resolve()
            source=work/"fixture.c"
            source.write_text('#include <math.h>\n#include <string.h>\n'
                '__attribute__((visibility("default"))) double skyrim_guest_api_v1('
                'double x, void *d, const void *s, unsigned long n) { '
                'memcpy(d, s, n); return cos(x); }\n')
            obj=work/"fixture.o"
            compile_log=work/"compile.log"
            try:
                builder.command([tool["clang"],*tool["compile_flags"],"-c",source,"-o",obj],compile_log)
            except subprocess.SubprocessError:
                self.fail("Apple SDK synthetic compile failed:\n"+compile_log.read_text(errors="replace"))
            imports,unknown=builder.import_symbols(tool,[obj])
            self.assertEqual(unknown,[])
            self.assertEqual(imports,["_cos","_memcpy"])
            response=work/"objects.rsp"
            response.write_text(str(obj)+"\n")
            exports=work/"exports.txt"
            exports.write_text("_skyrim_guest_api_v1\n")
            binary=work/"SkyrimGuest"
            link_log=work/"link.log"
            try:
                builder.command(builder.link_command(tool,work,response,exports,binary,imports),link_log)
            except subprocess.SubprocessError:
                self.fail("Apple SDK synthetic strict link failed:\n"+link_log.read_text(errors="replace"))
            builder.output(["xcrun","lipo",binary,"-verify_arch","arm64"])
            platform=builder.output(["xcrun","vtool","-show-build",binary])
            self.assertRegex(platform,r"platform\s+IOS\b")
            self.assertRegex(platform,r"minos\s+18\.0\b")
            symbols=builder.output([tool["nm"],"-g","-P",binary])
            self.assertRegex(symbols,r"(?m)^_skyrim_guest_api_v1\s+T\b")
            self.assertFalse((work/"libSystem.tbd").exists())


if __name__=="__main__":
    unittest.main()
