"""Compile actual converter comparisons and public headers, no Metal stubs."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mvk_patch", ROOT/"ios/dependencies/patch_moltenvk.py")
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)


class MoltenCacheKeyTests(unittest.TestCase):
    def test_real_configuration_matches_and_backport_refusal(self):
        source = os.environ.get("SKYRIM_MVK_CONVERTER_SOURCE")
        cross = os.environ.get("SKYRIM_SPIRV_CROSS_SOURCE")
        cxx = shutil.which(os.environ.get("CXX", "c++"))
        if not (source and cross and cxx):
            self.skipTest("Pinned MoltenVK converter/Cross headers and host C++ required")
        source = Path(source)
        raw = (source/"SPIRVToMSLConverter.cpp").read_bytes()
        if hashlib.sha256(raw).hexdigest() != patch.SHA256:
            raw = patch.original_from_patched(raw)
        new = patch.patched(raw)
        self.assertEqual(patch.original_from_patched(new), raw)
        with self.assertRaises(ValueError): patch.patched(raw+b"\n")
        with self.assertRaises(ValueError): patch.original_from_patched(new+b"\n")
        header = (source/"SPIRVToMSLConverter.h").read_bytes()
        # Original complete public header; no imitation of the cache key types.
        self.assertEqual(hashlib.sha256(header).hexdigest(), "a36806cf4caf7e3d72ffc04885f58fa6a07af07a0f37416e7ceb1cc63a02b6be")
        with tempfile.TemporaryDirectory(prefix="mvk-cache-key-") as tmp:
            folder = Path(tmp)
            (folder/"SPIRVToMSLConverter.h").write_bytes(header)
            for mode, data in ((0,raw),(1,new)):
                text = data.decode()
                start=text.index("// Returns whether the container")
                end=text.index("MVK_PUBLIC_SYMBOL void SPIRVToMSLConversionConfiguration::alignWith")
                unit=folder/f"comparison-{mode}.cpp"
                unit.write_text('#include <cstring>\n#include "SPIRVToMSLConverter.h"\n'
                    '#define MVK_PUBLIC_SYMBOL\nusing namespace mvk; using namespace std; '
                    'using namespace spv; using namespace SPIRV_CROSS_NAMESPACE;\n'+text[start:end])
                flags=["-O2","-Wall","-Wextra","-Wno-unknown-pragmas"]
                if os.environ.get("SKYRIM_TEST_SANITIZERS")=="1":
                    flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
                binary=folder/f"test-{mode}"
                subprocess.run([cxx,"-std=c++17",*flags,f"-DFIXTURE_NEW={mode}",str(unit),
                    str(ROOT/"tests/native/test_mvk_cache_key.cpp"),"-I",str(folder),"-I",cross,
                    "-o",str(binary)],check=True,timeout=60)
                result=subprocess.run([str(binary)],check=True,text=True,capture_output=True,timeout=15)
                self.assertIn("real_mvk_cache_key=PASS cases=12288",result.stdout)
                print(result.stdout.strip())


if __name__ == "__main__": unittest.main()
