"""ENB-compatible post-processing: ini and effect scans, preset planning, quality control and variables.

Only authored files are used; no ENB binary, preset or effect from any real release.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"

MAIN_FX = (
    "texture2D TextureColor;\nfloat4 Timer;\nfloat4 ENBParams01;\n"
    "sampler2D S = sampler_state { Texture = <TextureColor>; };\n"
    "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(S, uv) * Timer.x; }\n"
    "technique Draw { pass P0 { PixelShader = compile ps_3_0 PS(); } }\n")
BLOOM_FX = ("texture2D TextureBloom;\nfloat4 PS(float2 uv : TEXCOORD0) : COLOR { return 0; }\n"
            "technique T { pass P0 { PixelShader = compile ps_3_0 PS(); } }\n")
LENS_FX = BLOOM_FX
DOF_FX = ("texture2D TextureDepth;\nfloat4 PS(float2 uv : TEXCOORD0) : COLOR { return TextureDepth.x; }\n"
          "technique T { pass P0 { PixelShader = compile ps_3_0 PS(); } }\n")
POST_FX = "Texture2D T; float4 PS(float2 uv : TEXCOORD0) : SV_Target { return T.Sample(S, uv); }\ntechnique11 X { pass P { } }\n"


@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "C compiler required")
class EnbTests(unittest.TestCase):
    def test_all(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "Data"
            (root / "ENBSeries").mkdir(parents=True)             # case-insensitive lookup
            (root / "EnbSeries.INI").write_text("[EFFECT]\nEnableBloom=true\nEnableLens=false\n")
            (root / "d3d9.dll").write_bytes(b"not loaded")
            for name, body in {"enbeffect.fx": MAIN_FX, "ENBBLOOM.FX": BLOOM_FX, "enblens.fx": LENS_FX,
                               "enbdepthoffield.fx": DOF_FX, "enbeffectpostpass.fx": POST_FX}.items():
                (root / "ENBSeries" / name).write_text(body)
            cc = os.environ.get("CC", "cc")
            flags = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-I", str(PORT)]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
            binary = Path(tmp) / "enb"
            subprocess.run([*flags, ROOT / "tests/native/test_enb.c", *(PORT / n for n in (
                "enb_ini.c", "enb_fx.c", "enb_preset.c", "mod_plugins.c")), "-lm", "-o", binary], check=True)
            subprocess.run([binary, root], check=True, timeout=60)

    def test_links_are_not_followed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "Data"
            root.mkdir()
            (Path(tmp) / "real.ini").write_text("[EFFECT]\n")
            os.symlink(Path(tmp) / "real.ini", root / "enbseries.ini")
            cc = os.environ.get("CC", "cc")
            probe = Path(tmp) / "probe.c"
            probe.write_text('#include "enb_preset.h"\n#include <assert.h>\n#include <errno.h>\n'
                             'int main(int c, char **v) { (void)c; const char *r[] = {v[1]}; EnbPreset p; assert(enb_preset_find(r, 1) == -1);'
                             ' assert(enb_preset_load(v[1], &p) == ENOENT); return 0; }\n')
            binary = Path(tmp) / "probe"
            subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(PORT), probe,
                            *(PORT / n for n in ("enb_ini.c", "enb_fx.c", "enb_preset.c", "mod_plugins.c")), "-lm", "-o", binary],
                           check=True)
            subprocess.run([binary, root], check=True)


if __name__ == "__main__":
    unittest.main()
