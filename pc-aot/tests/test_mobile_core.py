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
        start = runtime.index("int skyrim_ios_window_pixel_size(")
        end = runtime.index("void skyrim_ios_present_phase(", start)
        present = runtime[runtime.index("void skyrim_ios_present_result("):]
        ui = (ROOT / "ios/runtime-port/mobile_ui.m").read_text()
        render_size = ui.split("void skyrim_mobile_render_size(", 1)[1].split(
            "int skyrim_mobile_render_override(", 1)[0]
        self.assertIn("skyrim_ios_window_pixel_size(&w, &h)", render_size)
        self.assertIn("renderTrial720 ? trialHeight", render_size)
        self.assertIn("renderRecovery ||", render_size)
        self.assertNotIn("skyrim_ios_drawable_size(&w, &h);", render_size)
        self.assertIn("renderAttempt && skyrim_ios_render_size_submitted(renderWidth, renderHeight)", ui)
        with tempfile.TemporaryDirectory(prefix="startup-pixel-size-") as tmp:
            source = Path(tmp) / "startup.c"
            binary = Path(tmp) / "startup"
            source.write_text(r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "ios_runtime.h"
typedef struct { int width, height; } SDL_Window;
static SDL_Window *ui_window;
static uint64_t submitted_render_size;
static uint64_t interval_max, last_frame;
static unsigned frames;
static int metrics_active = 1;
static pthread_mutex_t frame_metrics_mutex = PTHREAD_MUTEX_INITIALIZER;
static int on_main = 1;
static int calls;
enum { SKYRIM_PERF_FRAME_GAP };
static void atomic_max(uint64_t *limit, uint64_t value) { if (*limit < value) *limit = value; }
static uint64_t monotonic_ns(void) { return 1; }
static void skyrim_perf_interval(int metric, uint64_t start, uint64_t end, int a, int b) {
    (void)metric; (void)start; (void)end; (void)a; (void)b;
}
static void skyrim_benchmark_frame(uint64_t now) { (void)now; }
static void skyrim_mobile_frame(void) { ++frames; }
void skyrim_ios_stage(const char *stage) { (void)stage; }
static int pthread_main_np(void) { return on_main; }
static void SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    ++calls;
    *w = window->width;
    *h = window->height;
}
''' + runtime[start:end] + present + r'''
int main(void) {
    SDL_Window window = {2868, 1320};
    int w = 956, h = 440;
    assert(!skyrim_ios_window_pixel_size(&w, &h));
    assert(w == 956 && h == 440 && calls == 0);
    ui_window = &window;
    on_main = 0;
    assert(!skyrim_ios_window_pixel_size(&w, &h));
    assert(w == 956 && h == 440 && calls == 0);
    on_main = 1;
    assert(skyrim_ios_window_pixel_size(&w, &h));
    assert(w == 2868 && h == 1320 && calls == 1);
    window.height = 0;
    assert(!skyrim_ios_window_pixel_size(&w, &h));
    assert(w == 2868 && h == 1320 && calls == 2);

    skyrim_ios_render_submitted(956, 440);
    skyrim_ios_present_result(0);
    assert(frames == 1 && skyrim_ios_render_size_submitted(956, 440));
    assert(!skyrim_ios_render_size_submitted(1564, 720));
    skyrim_ios_present_result(-1);
    assert(frames == 1 && !skyrim_ios_render_size_submitted(1564, 720));
    /* DXVK may return D3D_OK with no acquired image. */
    skyrim_ios_present_result(0);
    assert(frames == 2 && !skyrim_ios_render_size_submitted(1564, 720));
    skyrim_ios_render_submitted(1564, 720);
    assert(skyrim_ios_render_size_submitted(1564, 720));
    assert(!skyrim_ios_render_size_submitted(0, 0));
}
''')
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread"]
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
