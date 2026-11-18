"""Real public DXVK allocation/layout code; authored driver emulates pool overflow."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT / "vendor/skydream/third_party/dxvk"
spec=importlib.util.spec_from_file_location("descriptor_dxvk",ROOT / "ios/dependencies/prepare_dxvk.py")
adapter=importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class DescriptorCapacityTests(unittest.TestCase):
    def test_real_pool_layout_rollover_retirement_failures_and_warm_reuse(self):
        cxx=shutil.which(os.environ.get("CXX","c++"))
        if not cxx:
            self.skipTest("C++ compiler required; run on WSL/Mac")
        outcomes=[]
        with tempfile.TemporaryDirectory(prefix="descriptor-capacity-") as tmp:
            for new in (False,True):
                folder=Path(tmp)/str(new); folder.mkdir()
                for name in ("dxvk_descriptor.h","dxvk_descriptor.cpp","dxvk_pipelayout.h","dxvk_pipelayout.cpp","dxvk_context.cpp"):
                    key="src/dxvk/"+name
                    text=(SOURCE/key).read_text()
                    self.assertEqual(hashlib.sha256(text.encode()).hexdigest(),adapter.EXPECTED[key])
                    if new: text=adapter.adapt(key,text)
                    if name=="dxvk_context.cpp":
                        start=text.index("  VkDescriptorSet DxvkContext::allocateDescriptorSet(")
                        end=text.index("  void DxvkContext::trackDrawBuffer()",start)
                        text='#include "descriptor_device_fixture.h"\nnamespace dxvk {\n'+text[start:end]+"}\n"
                    (folder/name).write_text(text)
                (folder/"dxvk_limits.h").write_bytes((SOURCE/"src/dxvk/dxvk_limits.h").read_bytes())
                (folder/"dxvk_include.h").write_text('#include "descriptor_fixture.h"\n')
                (folder/"dxvk_device.h").write_text('#include "descriptor_device_fixture.h"\n')
                flags=["-O2","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-Wno-deprecated-declarations"]
                if os.environ.get("SKYRIM_TEST_SANITIZERS")=="1":
                    flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
                binary=folder/"test"
                subprocess.run([cxx,"-std=c++17",*flags,f"-DFIXTURE_NEW={int(new)}",
                    *[str(folder/(n+".cpp")) for n in ("dxvk_descriptor","dxvk_pipelayout","dxvk_context")],
                    str(ROOT/"tests/native/test_descriptor_capacity.cpp"),"-I",str(folder),
                    "-I",str(ROOT/"tests/native"),"-I",str(SOURCE/"src"),"-I",str(SOURCE/"include"),
                    "-o",str(binary)],check=True,timeout=90)
                result=subprocess.run([str(binary)],check=True,text=True,capture_output=True,timeout=20)
                self.assertIn("actual_pool_layout_rollover=PASS",result.stdout)
                print(result.stdout.strip())
                outcomes.append(dict(re.findall(r"(digest|pools|dynamic_objects|image_objects)=(\d+)",result.stdout)))
        self.assertEqual(outcomes[0]["digest"],outcomes[1]["digest"])
        self.assertGreater(int(outcomes[0]["dynamic_objects"]),0)
        self.assertGreater(int(outcomes[0]["image_objects"]),0)
        self.assertEqual(outcomes[1]["dynamic_objects"],"0")
        self.assertEqual(outcomes[1]["image_objects"],"0")

    def test_fixed_meta_layout_bound_requires_review_when_sources_change(self):
        for key,expected in adapter.META_DESCRIPTOR_BOUNDS.items():
            self.assertEqual(hashlib.sha256((SOURCE/key).read_text().encode()).hexdigest(),expected)
        # All upstream helper bodies are pinned, not a guessed driver name rule.
        self.assertEqual(set(adapter.META_DESCRIPTOR_BOUNDS),
            {p.relative_to(SOURCE).as_posix() for p in (SOURCE/"src/dxvk").glob("dxvk_meta*.cpp")})


if __name__=="__main__": unittest.main()
