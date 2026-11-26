"""Exercise touch timing, gamepad composition and frame limits without UIKit."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class MobileCoreTests(unittest.TestCase):
    def test_startup_uses_window_pixels_before_metal_drawable_exists(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        runtime = (ROOT / "ios/runtime-port/ios_runtime.c").read_text()
        start = runtime.index("void skyrim_ios_startup_pixel_size(")
        end = runtime.index("void skyrim_ios_present_phase(", start)
        ui = (ROOT / "ios/runtime-port/mobile_ui.m").read_text()
        render_size = ui.split("void skyrim_mobile_render_size(", 1)[1].split(
            "int skyrim_mobile_graphics_changed(", 1)[0]
        self.assertIn("skyrim_ios_startup_pixel_size(&w, &h);", render_size)
        with tempfile.TemporaryDirectory(prefix="startup-pixel-size-") as tmp:
            source = Path(tmp) / "startup.c"
            binary = Path(tmp) / "startup"
            source.write_text(r'''
#include <assert.h>
#include "ios_runtime.h"
typedef struct { int width, height; } SDL_Window;
static SDL_Window *ui_window;
static int on_main = 1;
static int calls;
static int pthread_main_np(void) { return on_main; }
static void SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    ++calls;
    *w = window->width;
    *h = window->height;
}
''' + runtime[start:end] + r'''
int main(void) {
    SDL_Window window = {2868, 1320};
    int w = 956, h = 440;
    skyrim_ios_startup_pixel_size(&w, &h);
    assert(w == 956 && h == 440 && calls == 0);
    ui_window = &window;
    on_main = 0;
    skyrim_ios_startup_pixel_size(&w, &h);
    assert(w == 956 && h == 440 && calls == 0);
    on_main = 1;
    skyrim_ios_startup_pixel_size(&w, &h);
    assert(w == 2868 && h == 1320 && calls == 1);
    window.height = 0;
    skyrim_ios_startup_pixel_size(&w, &h);
    assert(w == 2868 && h == 1320 && calls == 2);
}
''')
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(ROOT / "ios/runtime-port"), str(source),
                            "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_actual_input_expiry_pacing_and_concurrent_snapshots(self):
        cc=shutil.which(os.environ.get("CC","cc"))
        if not cc: self.skipTest("C compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="mobile-core-") as tmp:
            binary=Path(tmp)/"core"
            flags=["-std=c11","-D_POSIX_C_SOURCE=200809L","-Wall","-Wextra","-Werror","-O2","-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS")=="1":
                flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
            subprocess.run([cc,*flags,"-I",str(ROOT/"ios/runtime-port"),
                str(ROOT/"tests/native/test_mobile_core.c"),str(ROOT/"ios/runtime-port/mobile_core.c"),
                "-lm","-o",str(binary)],check=True,timeout=60)
            result=subprocess.run([str(binary)],check=True,text=True,capture_output=True,timeout=15)
            self.assertIn("concurrent snapshots PASS",result.stdout)


if __name__=="__main__": unittest.main()
