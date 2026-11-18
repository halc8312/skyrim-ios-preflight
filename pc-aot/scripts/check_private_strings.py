"""Bounded, private, four-body string diagnostic; never full game regeneration.

Requires an explicit --execute and a hash-bound independent discovery audit.
Uses the pinned upstream oracle with persistent temporary files and address-bearing
abort diagnostics. Comparisons/inputs/native wrappers are otherwise unchanged.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
from contextlib import contextmanager
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
PATCH_SHA = "110850aabfe70b26220557f528e54fe75a13ef5b38cb4ace7fbbd4ac9e5035ad"
TARGETS = ("F5B670", "F520F0", "F55A90", "F59140")

class DiagnosticInterrupted(BaseException):
    pass

@contextmanager
def scoped_interrupts():
    previous = {}
    def interrupted(signum, frame):
        raise DiagnosticInterrupted(f"Interrupted by signal {signum}")
    if os.name == "posix":
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.getsignal(sig)
            signal.signal(sig, interrupted)
    try:
        yield
    finally:
        for sig, handler in previous.items(): signal.signal(sig, handler)

def private_output(path):
    private_root = ROOT.resolve()/"private"
    resolved = Path(path).resolve()
    if not resolved.is_relative_to(private_root):
        raise RuntimeError("Diagnostic output resolves outside this workspace's private directory")
    return resolved

def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda:f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def limits(memory_mib, cpu_seconds):
    def apply():
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (memory_mib * 1024**2,) * 2)
        resource.setrlimit(resource.RLIMIT_CPU, (cpu_seconds, cpu_seconds + 1))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_FSIZE, (256 * 1024**2,) * 2)
    return apply

def command(args, work, name, *, seconds=120, memory=1024, env=None):
    work = Path(work)
    record = {"argv":list(map(str,args)), "deadline_seconds":seconds, "address_space_mib":memory,
              "cpu_seconds":seconds, "compile_concurrency":1, "status":"started"}
    record_path = work / (name + ".command.json")
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    with (work/(name+".stdout")).open("wb") as out, (work/(name+".stderr")).open("wb") as err:
        child = subprocess.Popen(args, stdout=out, stderr=err, stdin=subprocess.DEVNULL,
            env=env, start_new_session=True, preexec_fn=limits(memory, seconds))
        record["spawned_pid"] = child.pid
        record_path.write_text(json.dumps(record, indent=2) + "\n")
        try:
            code = child.wait(timeout=seconds)
        except BaseException:
            # Give a nested adapter time to terminate its own compiler session.
            try: os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError: pass
            try: child.wait(timeout=10 if name == "oracle-adapter" else 3)
            except subprocess.TimeoutExpired: pass
            try: os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            child.wait(timeout=5)
            record["status"] = "interrupted_or_deadline"
            record_path.write_text(json.dumps(record, indent=2) + "\n")
            raise
    record.update(status="passed" if code == 0 else "failed", returncode=code)
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    if code:
        raise RuntimeError(f"{name} exited {code}; raw command/stdout/stderr preserved")
    return subprocess.CompletedProcess(args, code)

def oracle_worker(source, generated, work):
    """Adapt filesystem lifetime/logging only in an isolated process, not vendor source."""
    source, generated, work = map(Path, (source, generated, work))
    class PersistentTemporaryDirectory:
        def __init__(self, *args, **kwargs):
            self.name = tempfile.mkdtemp(prefix="oracle-", dir=work)
        def __enter__(self): return self.name
        def __exit__(self, *_): return False  # Deliberately never remove evidence.
    original_write = Path.write_text
    original_run = subprocess.run
    original_temp = tempfile.TemporaryDirectory
    counter = 0
    def write(path, text, *args, **kwargs):
        if path.name == "oracle.c":
            original_write(path.with_name("oracle-upstream.c"), text, *args, **kwargs)
            old = "void rt_call(CPU *c, uint32_t target) { (void)c; (void)target; abort(); }"
            if text.count(old) != 1:
                raise RuntimeError("Pinned oracle abort hook changed")
            text = text.replace(old, r'''void rt_call(CPU *c, uint32_t target) {
 (void)c; fprintf(stderr,"UNRESOLVED guest call/jump target=0x%08x\n",target); abort(); }
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
 (void)c; fprintf(stderr,"UNSUPPORTED guest instruction addr=0x%08x op=%s\n",addr,what); abort(); }
void rt_int3(CPU *c, uint32_t addr) {
 (void)c; fprintf(stderr,"UNEXPECTED guest INT3 addr=0x%08x\n",addr); abort(); }''')
        return original_write(path, text, *args, **kwargs)
    def bounded_run(args, **kwargs):
        nonlocal counter
        counter += 1
        compile_step = args[0] == "cc"
        if compile_step:
            args = list(args)
            args.insert(1, "-save-temps=obj")
        if not compile_step and not Path(args[0]).is_relative_to(work):
            raise RuntimeError("Unexpected oracle subprocess outside private evidence directory")
        return command(args, work, f"oracle-{counter}-" + ("compile" if compile_step else "run"),
                       seconds=120 if compile_step else 60, memory=1024 if compile_step else 128)
    try:
        tempfile.TemporaryDirectory = PersistentTemporaryDirectory
        subprocess.run = bounded_run
        Path.write_text = write
        sys.argv = [str(source/"tools/check_engine_strings.py"), "--gen", str(generated)]
        runpy.run_path(sys.argv[0], run_name="__main__")
    finally:
        tempfile.TemporaryDirectory = original_temp
        subprocess.run = original_run
        Path.write_text = original_write

def checked_inputs(candidate, discovery, audit_path, audit_sha, work):
    if digest(audit_path) != audit_sha:
        raise RuntimeError("Audit hash mismatch")
    audit = json.loads(audit_path.read_text())
    if audit["source_revision"] != PIN or digest(candidate) != audit["input_sha256"] or digest(discovery) != audit["discovery_sha256"]:
        raise RuntimeError("Input/discovery/pin differs from independent audit")
    if candidate.stat().st_size > 256 * 1024**2 or discovery.stat().st_size > 64 * 1024**2:
        raise RuntimeError("Input exceeds diagnostic size bound")
    selected = {f["address"].lower():f for f in audit["functions"] if f["address"].upper().removeprefix("0X") in TARGETS}
    if set(selected) != {"0x"+a.lower() for a in TARGETS}:
        raise RuntimeError("Audit lacks exact four string targets")
    data = json.loads(discovery.read_text())
    functions = {}
    import pefile
    pe = pefile.PE(str(candidate), fast_load=True)
    if pe.FILE_HEADER.Machine != 0x14c or pe.OPTIONAL_HEADER.Magic != 0x10b:
        raise RuntimeError("Expected x86-32 PE")
    counts = {}
    with candidate.open("rb") as original:
        for key, function in selected.items():
            failures = ("decode_errors", "calls", "absolute_memory_references", "indexed_global_references",
                        "external_direct_jumps", "unresolved_or_external_indirect_jumps", "fallthrough_outside_blocks",
                        "system_or_trap_instructions", "self_contained_blockers")
            if function.get("self_contained_candidate") is not True or any(function.get(name) for name in failures):
                raise RuntimeError(f"Audit does not establish self-contained candidate at {key}")
            blocks = [[int(b["start"],16),int(b["end"],16)] for b in function["blocks"]]
            discovered = data["functions"].get(key)
            if discovered is None or sorted(discovered) != sorted(blocks) or len(blocks) > 128 or sum(b-a for a,b in blocks) > 4096:
                raise RuntimeError(f"Discovery/audited block set mismatch or bound exceeded at {key}")
            # Preserve shared tails below the entry; never replace CFG with an entry..max slice.
            for block in function["blocks"]:
                start, end = int(block["start"],16),int(block["end"],16)
                if not block["decode_complete"] or end <= start:
                    raise RuntimeError(f"Incomplete decode at {start:#x}")
                offset = pe.get_offset_from_rva(start - pe.OPTIONAL_HEADER.ImageBase)
                original.seek(offset); body = original.read(end-start)
                if len(body) != end-start or hashlib.sha256(body).hexdigest() != block["sha256"]:
                    raise RuntimeError(f"Original byte identity mismatch at {start:#x}")
            functions[key] = discovered
            counts[key] = {name:function[name] for name in ("block_count", "instruction_count", "decoded_bytes", "discovered_block_identity_sha256")}
    pe.close()
    inputs = work / "inputs"; inputs.mkdir()
    shutil.copyfile(candidate, inputs/"candidate.exe")
    shutil.copyfile(audit_path, inputs/"audit.json")
    (inputs/"functions-four.json").write_text(json.dumps({"functions":functions, "iat":{}, "jump_tables":{}}, indent=2) + "\n")
    if digest(inputs/"candidate.exe") != audit["input_sha256"]:
        raise RuntimeError("Private candidate snapshot differs")
    return audit, counts

def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--oracle-worker":
        oracle_worker(*sys.argv[2:]); return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--discovery", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--audit-sha256", required=True)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    if not args.execute:
        print("Prepared four-body diagnostic only. No input lifting/execution without explicit --execute."); return 2
    if os.name != "posix":
        raise RuntimeError("Use approved existing POSIX tooling environment")
    work = private_output(ROOT/"private/diagnostics"/("four-strings-"+datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")+f"-{os.getpid()}"))
    work.mkdir(parents=True, exist_ok=False)
    private_output(work)
    report = {"scope":"four isolated original string bodies vs pinned native wrappers", "full_game_generation":False,
              "game_launched":False, "game_executable_launched":False, "translated_guest_functions_executed":False,
              "compatibility_profile_approved":False, "device_proof":False,
              "source_revision":PIN, "patch_sha256":PATCH_SHA, "status":"started", "compile_concurrency":1,
              "limitations":["Upstream deterministic 32000-case contract oracle, not exhaustive equivalence",
                             "Lifted C is the oracle; shared lifter errors can remain", "Host execution only, no full runtime or iPhone evidence"]}
    audit = None
    code = 1
    try:
        versions = {name:importlib.metadata.version(name) for name in ("capstone","pefile")}
        if versions != {"capstone":"5.0.9","pefile":"2024.8.26"}: raise RuntimeError("Unexpected approved dependency versions")
        report["dependencies"] = versions
        audit, counts = checked_inputs(args.candidate,args.discovery,args.audit,args.audit_sha256,work)
        report.update(input_sha256=audit["input_sha256"], discovery_sha256=audit["discovery_sha256"], audit_sha256=args.audit_sha256, targets=counts)
        patch = ROOT/"patches/skydream/0001-auxiliary-carry.patch"
        if digest(patch) != PATCH_SHA: raise RuntimeError("AF patch hash mismatch")
        shutil.copyfile(patch, work/"0001-auxiliary-carry.patch")
        shutil.copyfile(__file__, work/"runner.py")
        command(["git","-c","core.autocrlf=false","-C",str(ROOT/"vendor/skydream"),"archive","--format=tar","-o",str(work/"source.tar"),PIN],work,"archive")
        source = work/"source"; source.mkdir()
        with tarfile.open(work/"source.tar") as tar: tar.extractall(source,filter="data")
        command(["git","-C",str(source),"init","-q"],work,"isolate-source")
        command(["git","-C",str(source),"config","core.autocrlf","false"],work,"line-endings")
        command(["git","-C",str(source),"apply","--check",str(work/"0001-auxiliary-carry.patch")],work,"patch-check")
        command(["git","-C",str(source),"apply",str(work/"0001-auxiliary-carry.patch")],work,"patch-apply")
        report["source_hashes"] = {name:digest(source/name) for name in ("tools/lift.py","tools/peimg.py","runtime/x86.h","runtime/lift_prelude.h","tools/check_engine_strings.py","runtime/engine_strings.c","runtime/engine_strings_bridge.c")}
        if report["source_hashes"]["runtime/x86.h"] != "13878e1c48d65e3b7b0162570f8df2c4576560060c3eda1f52cdd66869004302" or report["source_hashes"]["tools/lift.py"] != "c0f31459bb311cc4c12de668f8c4ffe44a973dc59c5ecbc9b2d3f38b1610fd3b":
            raise RuntimeError("Patched private source identity mismatch")
        environment = dict(os.environ,LIFT_FILES="1",LIFT_GAME="skyrim",LIFT_MEMORDER="1",LIFT_TSO="1",PYTHONDONTWRITEBYTECODE="1",SKYDREAM_NATIVE_STRINGS="1")
        environment.pop("SKYDREAM_PROFILE_STRINGS",None)
        generated = work/"generated-four"
        command([sys.executable,"-B",str(source/"tools/lift.py"),str(work/"inputs/candidate.exe"),str(work/"inputs/functions-four.json"),str(generated)],work,"lift-four-only",seconds=120,memory=2048,env=environment)
        if json.loads((generated/"exe_rejected.json").read_text()): raise RuntimeError("Lifter rejected original body; see private rejection file")
        text = (generated/"exe_00.c").read_text()
        names = re.findall(r"void f_([0-9A-F]+)_guest\(CPU \*c\) \{",text)
        if sorted(names) != sorted(TARGETS): raise RuntimeError("Generated definitions are not exactly the four original guest bodies")
        if "rt_unimpl(" in text or "rt_int3(" in text or "rt_call(" in text or re.search(r"\bf_[0-9A-F]+\(c\)",text):
            lines = [line.strip() for line in text.splitlines() if any(s in line for s in ("rt_unimpl(","rt_int3(","rt_call("))]
            raise RuntimeError("Unknown instruction/call or non-self-contained generated body: "+repr(lines))
        report["generated_sha256"] = digest(generated/"exe_00.c")
        temporary = work/"oracle-temporary"; temporary.mkdir()
        command([sys.executable,"-B",str(work/"runner.py"),"--oracle-worker",str(source),str(generated),str(temporary)],work,"oracle-adapter",seconds=240,memory=2048,env=environment)
        runtime_output = (temporary/"oracle-2-run.stdout").read_text()
        if "PASS: 32000 differential cases, four string entries; results, writes, preserved ABI" not in runtime_output:
            raise RuntimeError("Pinned oracle did not report its full expected case count")
        report["status"] = "partial_contract_diagnostic_passed"
        code = 0
    except BaseException as error:
        interrupted = isinstance(error,(DiagnosticInterrupted,KeyboardInterrupt,SystemExit,subprocess.TimeoutExpired))
        report.update(status="interrupted" if interrupted else "failed",error=str(error)); code=1
    finally:
        oracle_command = work/"oracle-temporary/oracle-2-run.command.json"
        if oracle_command.is_file():
            report["translated_guest_functions_executed"] = bool(json.loads(oracle_command.read_text()).get("spawned_pid"))
        if audit:
            report["original_input_sha256_after"] = digest(args.candidate)
            report["original_discovery_sha256_after"] = digest(args.discovery)
            report["snapshot_sha256_after"] = digest(work/"inputs/candidate.exe")
            if report["original_input_sha256_after"] != audit["input_sha256"] or report["original_discovery_sha256_after"] != audit["discovery_sha256"] or report["snapshot_sha256_after"] != audit["input_sha256"]:
                report.update(status="failed",integrity_failure=True); code=1
        report["retained_files"] = {str(p.relative_to(work)):digest(p) for p in work.rglob("*") if p.is_file() and ".git" not in p.parts and "source" not in p.relative_to(work).parts and p.name != "report.json"}
        (work/"report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"status":report["status"],"report":str(work/"report.json"),"error":report.get("error")},indent=2))
    return code

if __name__ == "__main__":
    with scoped_interrupts():
        raise SystemExit(main())
