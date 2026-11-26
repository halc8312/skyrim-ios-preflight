"""Guest lock policy patch: only the pthread policy of two lock families changes, and only where the macros exist."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runtime_policy_stage", ROOT / "ios/runtime-port/prepare_runtime.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
KERNEL = ROOT / "vendor/skydream/runtime/kernel32.c"


@unittest.skipUnless(KERNEL.is_file(), "pinned public runtime is unavailable; tests never download it")
class MutexPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = KERNEL.read_text()
        cls.patched = adapter.patch_mutex_policy(cls.original)

    def test_wait_lock_and_critical_sections_request_first_fit_behind_platform_guards(self):
        self.assertIn("#ifdef PTHREAD_FIRSTFIT_MUTEX_INITIALIZER\nstatic pthread_mutex_t g_wlock = "
                      "PTHREAD_FIRSTFIT_MUTEX_INITIALIZER;\n#else\nstatic pthread_mutex_t g_wlock = "
                      "PTHREAD_MUTEX_INITIALIZER;\n#endif", self.patched)
        self.assertIn("pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);\n#ifdef PTHREAD_MUTEX_POLICY_FIRSTFIT_NP\n"
                      "                pthread_mutexattr_setpolicy_np(&a, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP);\n#endif\n", self.patched)

    def test_nothing_else_changes(self):
        restored = self.patched.replace(
            "#ifdef PTHREAD_FIRSTFIT_MUTEX_INITIALIZER\nstatic pthread_mutex_t g_wlock = PTHREAD_FIRSTFIT_MUTEX_INITIALIZER;\n#else\n", "")
        restored = restored.replace("static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;\n#endif",
                                    "static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;")
        restored = restored.replace("#ifdef PTHREAD_MUTEX_POLICY_FIRSTFIT_NP\n"
                                    "                pthread_mutexattr_setpolicy_np(&a, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP);\n#endif\n", "")
        self.assertEqual(restored, self.original)

    def test_kernel_pipeline_still_stages_and_other_locks_are_not_touched(self):
        staged = adapter.patch_kernel(self.original)
        self.assertIn("PTHREAD_FIRSTFIT_MUTEX_INITIALIZER", staged)
        self.assertEqual(staged.count("PTHREAD_MUTEX_POLICY_FIRSTFIT_NP"), 2)  # guard and call, critical sections only
        self.assertIn("static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;", staged)
        self.assertIn("static pthread_mutex_t g_tlock = PTHREAD_MUTEX_INITIALIZER;", staged)

    def test_both_branches_compile(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        snippet = self.patched[self.patched.index("#ifdef PTHREAD_FIRSTFIT_MUTEX_INITIALIZER"):]
        snippet = snippet[:snippet.index("#endif") + 6]
        attr = self.patched[self.patched.index("                pthread_mutexattr_settype"):]
        attr = attr[:attr.index("#endif\n") + 7]
        stub = "static int pthread_mutexattr_setpolicy_np(pthread_mutexattr_t *a, int p) { (void)a; return p; }\n"
        for defs in ([], ["-DPTHREAD_FIRSTFIT_MUTEX_INITIALIZER=PTHREAD_MUTEX_INITIALIZER",
                          "-DPTHREAD_MUTEX_POLICY_FIRSTFIT_NP=3"]):
            source = ("#include <pthread.h>\n" + snippet + "\n" + (stub if defs else "") +
                      "void f(void) {\n    pthread_mutexattr_t a;\n" + attr + "}\n")
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "p.c"
                path.write_text(source)
                subprocess.run([compiler, "-fsyntax-only", "-Wall", "-Werror", *defs, str(path)], check=True)


if __name__ == "__main__":
    unittest.main()
