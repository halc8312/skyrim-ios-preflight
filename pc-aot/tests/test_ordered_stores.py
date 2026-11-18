"""Actual TSO helpers: native byte oracle and real iOS ARM64 code generation.

The native test selects the ARM helper body on x86, testing byte semantics, not
ARM hardware ordering. The Windows test separately checks real ARM instructions.
No game input is used and neither test proves physical iPhone performance.
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"


def prepare(folder):
    for label in ("baseline", "patched"):
        stage = folder / label
        for name in ("x86.h", "rt_table.h", "lift_prelude.h"):
            path = stage / "runtime" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(subprocess.check_output(["git", "-C", str(ROOT / "vendor/skydream"),
                "show", PIN + ":runtime/" + name]))
        if label == "patched":
            subprocess.run(["git", "init", "-q", str(stage)], check=True)
            subprocess.run(["git", "-C", str(stage), "-c", "core.autocrlf=false", "apply",
                str(ROOT / "patches/skydream/0006-inline-ordered-stores.patch")], check=True)
    (folder / "helpers.c").write_text('''#include "x86.h"
#include "lift_prelude.h"
uint32_t ordered_load16(uint32_t a) { return TL16(a); }
uint32_t ordered_load32(uint32_t a) { return TL32(a); }
void ordered_store16(uint32_t a, uint16_t x) { TS16(a, x); }
void ordered_store32(uint32_t a, uint32_t x) { TS32(a, x); }
''')
    (folder / "driver.c").write_text(DRIVER)


DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t bytes[4096]; uint8_t *MEM=bytes;
uint32_t ordered_load16(uint32_t); uint32_t ordered_load32(uint32_t);
void ordered_store16(uint32_t,uint16_t); void ordered_store32(uint32_t,uint32_t);
int main(void) {
    uint8_t expected[4096]; unsigned state=1, cases=0;
    const unsigned bases[]={0,31,63,4076};
    memset(bytes,0xa5,sizeof bytes); memset(expected,0xa5,sizeof expected);
    for(unsigned n=0;n<4096;n++) for(unsigned j=0;j<4;j++) for(unsigned k=0;k<16;k++) {
        unsigned p=bases[j]+k; state=state*1664525u+1013904223u;
        ordered_store32(p,state);
        for(unsigned b=0;b<4;b++) expected[p+b]=(uint8_t)(state>>(b*8));
        if(memcmp(bytes,expected,sizeof bytes)||ordered_load32(p)!=state) return 1;
        unsigned short v=(unsigned short)(state>>8);
        ordered_store16(p,v); expected[p]=(uint8_t)v; expected[p+1]=(uint8_t)(v>>8);
        if(memcmp(bytes,expected,sizeof bytes)||ordered_load16(p)!=v) return 2;
        unsigned mixed=expected[p]|(unsigned)expected[p+1]<<8|(unsigned)expected[p+2]<<16|(unsigned)expected[p+3]<<24;
        if(ordered_load32(p)!=mixed) return 3;
        cases+=2;
    }
    printf("ordered_store_byte_cases=%u\n",cases); return 0;
}
'''


class OrderedStoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ordered-stores-")
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        prepare(self.folder)

    @unittest.skipUnless(os.name == "posix" and shutil.which("cc"), "Existing POSIX C compiler required")
    def test_real_helpers_aligned_unaligned_edges_and_mixed_widths(self):
        for label in ("baseline", "patched"):
            binary = self.folder / label / "check"
            subprocess.run(["cc", "-O1", "-fno-builtin", "-fno-strict-aliasing", "-fwrapv",
                "-D__aarch64__", "-DLB_STRICT_ORDER", "-I", str(self.folder/label/"runtime"),
                str(self.folder/"helpers.c"), str(self.folder/"driver.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True, text=True, check=True, timeout=30)
            self.assertEqual(result.stdout, "ordered_store_byte_cases=524288\n")
        print("524288 byte-oracle cases per baseline/patched helper")

    @unittest.skipUnless(os.name == "nt", "Existing Windows iOS cross-compiler route required")
    def test_actual_arm64_compiler_keeps_ordering_and_removes_stack_spills(self):
        spec=importlib.util.spec_from_file_location("guest_build",ROOT/"scripts/build_title_guest.py")
        build=importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
        clang=build.LLVM/"clang.exe"
        resource=Path(subprocess.check_output([clang,"-print-resource-dir"],text=True).strip())/"include"
        flags=["--target=arm64-apple-ios18.0","-std=gnu11","-O1","-g0","-fno-strict-aliasing",
            "-fwrapv","-fsigned-char","-fvisibility=hidden","-fno-stack-protector","-fno-builtin",
            "-DLB_STRICT_ORDER","-DGAME_SKYRIM","-w","-nostdlibinc","-isystem",str(resource),"-isystem",str(build.DARWIN)]
        bodies={}
        for label in ("baseline","patched"):
            asm=self.folder/label/"helpers.s"
            subprocess.run([clang,*flags,"-I",str(self.folder/label/"runtime"),"-S",self.folder/"helpers.c","-o",asm],
                           capture_output=True,check=True,timeout=60)
            bodies[label]=asm.read_text()
        for op in (r"\bldarh?\b",r"\bstlrh?\b",r"\bdmb\s+\w+"):
            self.assertEqual(re.findall(op,bodies["baseline"]),re.findall(op,bodies["patched"]))
        self.assertEqual(re.findall(r"\bdmb\s+(\w+)",bodies["patched"]),["ishld","ishld","ish","ish"])
        self.assertEqual(len(re.findall(r"\bstlrh?\b",bodies["patched"])),2)
        self.assertIn("[sp,",bodies["baseline"])
        self.assertNotRegex(bodies["patched"],r"\bsp\b|\bbl\b")
        # Each unaligned store follows its release barrier in the real body.
        for name,operation in (("ordered_store16","strh"),("ordered_store32","str")):
            body=bodies["patched"].split("_"+name+":",1)[1].split(".cfi_endproc",1)[0]
            self.assertRegex(body,rf"dmb\s+ish[\s\S]*\b{operation}\s+w1")
        print("ARM64: aligned stlr/stlrh and all dmb retained; stack spill/reload removed")


if __name__ == "__main__": unittest.main()
