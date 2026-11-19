"""Check the pinned class, shader placements, input protection and SPIR-V resource use."""
from collections import Counter
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import prepare


class PresentationSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = os.environ.get("GRADE_DXVK")
        if not src:
            raise unittest.SkipTest("Set GRADE_DXVK to Skydream's pinned third_party/dxvk")
        cls.dxvk = Path(src).resolve()
        cls.grade = (prepare.HERE / "warm_grade.glsl").read_text(encoding="utf-8")

    def test_presentation_state_drawing_and_msaa_resolve_exactly_match_original_methods(self):
        _, trial = prepare.test_class(self.dxvk)
        host = prepare.staged_host(self.dxvk)
        original = prepare.pinned(self.dxvk / "src/dxvk/dxvk_swapchain_blitter.cpp", prepare.CPP_SHA)
        corrected = trial.replace(prepare.TRIAL, prepare.UPSTREAM)
        def method(code, signature):
            start = code.index("  void " + prepare.UPSTREAM + "::" + signature)
            end = code.index("\n  }", start) + len("\n  }")
            return code[start:end]
        for name in ("presentImage(", "draw(", "resolve(", "createResolveImage(", "updateGammaTexture("):
            self.assertEqual(method(original, name), method(corrected, name))
            self.assertEqual(method(original, name), method(host, name))
        self.assertEqual(corrected.count("grade ? SpirvCodeBuffer"), len(prepare.SHADERS))
        self.assertEqual(host.count("std::getenv("), 1)
        self.assertIn('std::getenv("SKYRIM_TRIAL_PRESENT_GRADE")', method(host, "createShaders("))

    def test_grade_is_between_source_sampling_or_resolve_and_output_gamma(self):
        for shader, (_, checksum) in prepare.SHADERS.items():
            original = prepare.pinned(self.dxvk / "src/dxvk/shaders" / (shader + ".frag"), checksum)
            result = prepare.present_shader(original, self.grade)
            call = "o_color.rgb = skyrimWarmDisplay(o_color.rgb);"
            self.assertEqual(result.count(call), 1)
            self.assertLess(result.index(call), result.index("if (s_gamma_bound)"))
            for line in ("o_color = texelFetch", "o_color = textureLod", "o_color /= float(c_samples)"):
                if line in original:
                    self.assertLess(result.index(line), result.index(call))
            self.assertIn("o_color.a);", result)

    def test_failed_generation_does_not_change_existing_directory_or_publish_a_partial_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            existing = base / "existing"
            existing.mkdir()
            (existing / "owner-file").write_bytes(b"keep")
            with self.assertRaises(ValueError):
                prepare.generate(self.dxvk, existing, "unused", "unused")
            self.assertEqual((existing / "owner-file").read_bytes(), b"keep")
            new = base / "new"
            with self.assertRaises(OSError):
                prepare.generate(self.dxvk, new, str(base / "missing-compiler"), "unused")
            self.assertFalse(new.exists())
            with self.assertRaises(ValueError):
                prepare.generate(self.dxvk, self.dxvk / "unused-presentation-output", "unused", "unused")

    def test_compiled_grade_has_the_same_image_reads_descriptors_and_specialization_constants(self):
        compiler = shutil.which(os.environ.get("GRADE_GLSLANG", "glslangValidator"))
        dis = shutil.which(os.environ.get("GRADE_SPIRV_DIS", "spirv-dis"))
        if not compiler or not dis:
            self.skipTest("glslangValidator and spirv-dis required")
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp)
            for name, (_, checksum) in prepare.SHADERS.items():
                stock = prepare.pinned(self.dxvk / "src/dxvk/shaders" / (name + ".frag"), checksum)
                modules = []
                for shader in (stock, prepare.present_shader(stock, self.grade)):
                    fragment, binary = stage / "source.frag", stage / "compiled.spv"
                    fragment.write_text(shader, encoding="utf-8")
                    subprocess.run([compiler, "-V", "--target-env", "vulkan1.0", "-o", binary, fragment],
                                   check=True, capture_output=True)
                    modules.append(subprocess.check_output([dis, binary], text=True))
                def resource_signature(module):
                    image_reads = Counter(re.findall(r"\b(OpImage\w+|OpFragment(?:Mask)?FetchAMD)\b", module))
                    decorations = Counter(re.findall(r"\b(DescriptorSet|Binding|SpecId|Location|Offset) (\d+)", module))
                    return image_reads, decorations
                self.assertEqual(resource_signature(modules[0]), resource_signature(modules[1]), name)
                self.assertIn("OpDot", modules[1])


if __name__ == "__main__":
    unittest.main()
