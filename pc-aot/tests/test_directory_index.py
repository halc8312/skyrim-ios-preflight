"""Real directory-cache lookup, invalidation and optional index allocation."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('directory_stage',ROOT/'ios/runtime-port/prepare_runtime.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)



def pinned_kernel():
    runtime = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream")) / "runtime"
    if not (runtime / "kernel32.c").is_file():
        raise unittest.SkipTest("Pinned Skydream checkout required")
    original = (runtime / "kernel32.c").read_text(encoding="utf-8")
    assert hashlib.sha256(original.encode()).hexdigest() == adapter.EXPECTED["kernel32.c"]
    return runtime, original


def kernel_file_fixture(folder, ece_witness=False):
    """Stage the normal iOS file shims with authored CPU/guest memory."""
    compiler = shutil.which(os.environ.get("CC", "cc"))
    if not compiler:
        raise unittest.SkipTest("C compiler required")
    runtime, original = pinned_kernel()
    port = ROOT / "ios/runtime-port"
    # The import registration table roots every shim under ASan. This fixture
    # invokes the three unchanged file entry points directly instead.
    source = adapter.patch_kernel(original).split("\nstatic const ShimDef k32[] = {", 1)
    assert len(source) == 2
    (folder / "kernel32.c").write_text(source[0] + "\n", encoding="utf-8")
    binary = folder / "kernel-file"
    flags = ["-std=gnu11", "-DGAME_SKYRIM", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-pthread"]
    flags += ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
    if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    sources = [ROOT / "tests/native/test_mod_kernel_file.c", port / "performance_metrics.c"]
    if ece_witness:
        ece = ROOT / "experiments/classic-racemenu-apply"
        flags += ["-DSKYRIM_IOS_MOD_HOOKS=1", "-I", ece, "-I", ROOT / "ios/private-abi",
                  "-I", ROOT / "experiments/fuz-voice"]
        sources += [port / "mod_ece_profile.c", port / "mod_ece_tri_host.c", ece / "ece_nose_witness.c",
                    ece / "ece_head_geometry.c",
                    port / "mod_highheel_pex_host.c", port / "classic_vm_epoch.c"]
    subprocess.run([compiler, *flags, "-I", folder, "-I", runtime, "-I", port,
                    *sources, "-lm", "-o", binary], check=True, timeout=90)
    return binary


def fixture(folder, measured=False):
    _, original = pinned_kernel()
    chunks=[]
    for label,text in [('old',original),('new',adapter.patch_directory_index(original))]:
        body=text[text.index('/* Directory listings for case-insensitive lookups.'):text.index('/* Resolve each path component case-insensitively')]
        for token in ('DirCache','g_dircache','g_dircache_next','g_dircache_mu','dircache_forget','dir_lookup_ci'):
            body=re.sub(r'\b'+token+r'\b',label+'_'+token,body)
        if measured:body=body.replace('strcasecmp(',label+'_compare(')
        body=body.replace('malloc(count * sizeof(*index))','index_malloc(count * sizeof(*index))')
        chunks.append(body)
    source=(ROOT/'tests/native/test_directory_index.c').read_text().replace('/* ACTUAL_DIRECTORY_CACHE */','\n'.join(chunks))
    path=folder/'directory.c';path.write_text(source)
    flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
    result=subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread',
                           f'-DMEASURED={int(measured)}',*flags,str(path),'-o',str(folder/'directory')],
                          capture_output=True,text=True,timeout=60)
    if result.returncode:raise AssertionError(result.stderr[-8000:])
    return folder/'directory'


@unittest.skipUnless(os.name=='posix' and shutil.which('cc'),'Existing POSIX C compiler required')
class DirectoryIndexTests(unittest.TestCase):
    def test_actual_case_lookup_invalidation_eviction_failure_and_parallel_readers(self):
        with tempfile.TemporaryDirectory(prefix='dir-index-') as tmp:
            folder=Path(tmp);data=folder/'data';data.mkdir()
            binary=fixture(folder,measured=True)
            result=subprocess.run([str(binary),str(data)],capture_output=True,text=True,timeout=40)
            self.assertEqual(result.returncode,0,result.stderr[-8000:])
            self.assertIn('DIRECTORY_INDEX passed',result.stdout)


if __name__=='__main__':unittest.main()
