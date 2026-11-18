"""Prepare/review/execute a private lifted-original-vs-native primitive diagnostic.

Default --prepare performs static inspection only. Execution requires the exact
reviewed audit hash and a fresh private output directory. Never approves a game
profile or runs the Windows executable. Uses the pinned upstream comparison.
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
import struct
import subprocess
import sys
import tarfile
import tempfile

# Keep the already tested process-group ownership, limits and path policy.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_private_strings import (ROOT, PIN, DiagnosticInterrupted, command,
                                   digest, private_output, scoped_interrupts)

INPUT_SHA = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605"
DISCOVERY_SHA = "1fc28ce90d668d635ace8fd641d95720782e24c0f64eaf08cbaf8bf170d15b6d"
ROOTS = ("f537f0", "f52e30", "a4a4e0", "a4a530", "a4a570")
HELPERS = ("f6029c", "f60323")
BACKWARD_TABLES = {
    0xf539de: (0xf53ab0, (0xf53ab4,0xf53abc,0xf53ac4,0xf53acc,0xf53ad4,0xf53adc,0xf53ae4,0xf53af7)),
    0xf5301e: (0xf530f0, (0xf530f4,0xf530fc,0xf53104,0xf5310c,0xf53114,0xf5311c,0xf53124,0xf53137)),
}
LANDING_PADS = tuple(f"{address:x}" for _,targets in BACKWARD_TABLES.values() for address in targets[:-1])
SELECTED = ROOTS + HELPERS + LANDING_PADS
SELECTION_FILE = "functions-selected.json"
PATCHES = {
    "0001-auxiliary-carry.patch": "110850aabfe70b26220557f528e54fe75a13ef5b38cb4ace7fbbd4ac9e5035ad",
    "0002-report-empty-jump-tables.patch": "6fe5ca6e3704184e865ec58a10d21628d53a267e6d098c3b33b16f6658c84a06",
    "0003-negative-index-candidate-roots.patch": "21f610a0af715992a6b16b2adcacd0924e4d020cc7c7cd6c88beabc9338668e3",
}
NEGATIVE = {0xf5386c: (0xf53964, [0xf53974, 0xf5397c, 0xf53988, 0xf5399c]),
            0xf52eac: (0xf52fa4, [0xf52fb4, 0xf52fbc, 0xf52fc8, 0xf52fdc])}
# Bounded discovery skips the non-code slot zero instead of retaining positions.
# The fixed input's TEST EDI,3 / JNE edge and MOV EAX,EDI / AND EAX,3
# restrict these four alignment dispatches to slots 1..3. Keep all raw slots.
COMPACTED = {0xf53865: (0x9000f538, 0xf53837, 0xf5383d, 0xf53854),
             0xf539f9: (0x9000f53b, 0xf539bc, 0xf539c2, 0xf539e8),
             0xf52ea5: (0x9000f52f, 0xf52e77, 0xf52e7d, 0xf52e94),
             0xf53039: (0x9000f531, 0xf52ffc, 0xf53002, 0xf53028)}
PASS = "PASS: 20000 differential cases, five TESV entry points; memory, returns and preserved ABI"


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def tree_hashes(root):
    return {str(p.relative_to(root)): digest(p) for p in sorted(root.rglob("*"))
            if p.is_file() and ".git" not in p.relative_to(root).parts and "__pycache__" not in p.parts}


def dependencies():
    result = {}
    for name, expected in (("capstone", "5.0.9"), ("pefile", "2024.8.26")):
        dist = importlib.metadata.distribution(name)
        if dist.version != expected:
            raise RuntimeError("Unexpected approved dependency version: " + name)
        files = {str(f): digest(dist.locate_file(f)) for f in dist.files or []
                 if str(f).endswith((".py", ".so", ".dll", "METADATA", "RECORD"))
                 and Path(dist.locate_file(f)).is_file()}
        result[name] = {"version": dist.version, "files_sha256": files}
    return result


def table_slots(original, targets, invalid_prefix=None):
    """Retain scanner-compacted slots without accepting altered target bytes."""
    expected = ([invalid_prefix] if invalid_prefix is not None else []) + targets
    if len(original) != len(expected)*4:
        raise RuntimeError("Truncated jump table span")
    actual = list(struct.unpack("<"+"I"*len(expected),original))
    return actual, actual == expected


def backward_bound_evidence(rows, site, tables):
    """Verify this fixed input's four CMP/JB edges into the NEG dispatch."""
    indexed={int(row["address"],16):row for row in rows}
    negative=site-2
    neg=indexed.get(negative,{})
    if (neg.get("mnemonic"),neg.get("operands"),neg.get("size")) != ("neg","ecx",2):
        raise RuntimeError("Backward dispatch lacks adjacent NEG ECX")
    incoming=[]
    for row in rows:
        address=int(row["address"],16)
        if row["operands"]==hex(negative):
            previous=next((x for x in rows if int(x["address"],16)+x["size"]==address),{})
            if row["mnemonic"]!="jb" or (previous.get("mnemonic"),previous.get("operands")) != ("cmp","ecx, 8"):
                raise RuntimeError("Unbounded incoming backward-dispatch edge")
            incoming.append({"compare":previous,"taken_branch":row})
        if address+row["size"]==negative and row["mnemonic"] not in ("jmp","ret"):
            raise RuntimeError("Unbounded fallthrough into backward dispatch")
    if len(incoming)!=4 or any(hex(negative) in targets for targets in tables.values()):
        raise RuntimeError("Unexpected backward-dispatch predecessor set")
    return {"incoming_edges":incoming,"negate":neg,"unsigned_count_before_negate":[0,7],
            "signed_indices_after_negate":[-7,0],"no_synthetic_guest_call_frame":True,
            "note":"Original discovery's nonempty zero-forward table omits seven negative slots; original evidence retained"}


def subset(data):
    """Keep exact disjoint blocks, original tables, IAT and unresolved evidence."""
    result = dict(data)
    result["functions"] = {"0x"+a: data["functions"]["0x"+a] for a in SELECTED}
    if any(len(v) > 128 or any(b <= a or b-a > 4096 for a, b in v)
           for v in result["functions"].values()):
        raise RuntimeError("Unexpected discovered block bounds")
    return result


def audit_image(candidate, data, source):
    import capstone as cs
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM
    import pefile
    pe = pefile.PE(str(candidate), fast_load=True)
    if pe.OPTIONAL_HEADER.ImageBase != 0x400000 or pe.FILE_HEADER.Machine != 0x14c:
        raise RuntimeError("Unexpected PE architecture/base")
    if pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.SizeOfImage > 0x2000000:
        raise RuntimeError("Image exceeds the upstream private 32 MiB arena")
    raw = candidate.read_bytes()
    def bytes_at(address, count):
        rva = address - 0x400000
        section = pe.get_section_by_rva(rva)
        if section is None or rva-section.VirtualAddress+count > min(section.SizeOfRawData, section.Misc_VirtualSize or section.SizeOfRawData):
            raise RuntimeError(f"Not wholly file-backed: {address:x}+{count}")
        offset = section.PointerToRawData + rva-section.VirtualAddress
        if offset < 0 or offset+count > len(raw):
            raise RuntimeError("Truncated original bytes")
        return raw[offset:offset+count]
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32); md.detail = True
    functions = []; blockers = []; sites = set()
    selected = subset(data)
    for name, ranges in selected["functions"].items():
        blocks = []; instructions = []; starts = {a for a, _ in ranges}
        for start, end in ranges:
            chunk = bytes_at(start, end-start)
            decoded = list(md.disasm(chunk, start))
            if sum(i.size for i in decoded) != len(chunk):
                blockers.append(f"Incomplete decode {start:x}")
            blocks.append({"start": start, "end": end, "sha256": hashlib.sha256(chunk).hexdigest()})
            instructions.extend(decoded)
        boundaries = {i.address for i in instructions}
        if int(name,16) not in starts:
            blockers.append(f"Root is not a discovered block start: {name}")
        fallthrough = []
        for ins in instructions:
            if not ins.group(cs.CS_GRP_RET) and ins.mnemonic != "jmp" and ins.address+ins.size not in boundaries:
                fallthrough.append(hex(ins.address))
                blockers.append(f"Fallthrough outside exact blocks: {ins.address:x}")
        calls = []; jumps = []; globals_ = []; instruction_rows = []
        for ins in instructions:
            sites.add(ins.address)
            instruction_rows.append({"address": hex(ins.address), "size":ins.size,"mnemonic": ins.mnemonic, "operands": ins.op_str})
            if ins.group(cs.CS_GRP_CALL):
                target = ins.operands[0].imm if ins.operands[0].type == X86_OP_IMM else None
                calls.append({"site": hex(ins.address), "target": hex(target) if target else None})
                if target not in {int(a,16) for a in HELPERS}:
                    blockers.append(f"Unreviewed call {ins.address:x}")
            if ins.group(cs.CS_GRP_JUMP):
                target = ins.operands[0].imm if ins.operands[0].type == X86_OP_IMM else None
                table = data["jump_tables"].get(hex(ins.address))
                row = {"site": hex(ins.address), "direct_target": hex(target) if target else None,
                       "original_table": table, "original_unresolved_marker": hex(ins.address) in data.get("indirect_jumps", [])}
                if target is not None and target not in boundaries:
                    blockers.append(f"External branch {ins.address:x}->{target:x}")
                if target is None:
                    if ins.address in NEGATIVE:
                        address, expected = NEGATIVE[ins.address]
                        actual = list(struct.unpack("<4I", bytes_at(address, 16)))
                        row["bounded_negative_slots"] = {"address": hex(address), "targets": list(map(hex, actual)),
                            "index_values": [-4,-3,-2,-1], "original_table_not_rewritten": True}
                        if actual != expected or not set(actual) <= starts or table != []:
                            blockers.append("Negative-slot evidence changed")
                    elif ins.address in BACKWARD_TABLES:
                        address, expected = BACKWARD_TABLES[ins.address]
                        original = bytes_at(address-28,32)
                        actual = tuple(struct.unpack("<8I",original))
                        row["bounded_backward_slots"] = {"base_address":hex(address),"index_values":list(range(-7,1)),
                            "slots":[{"index":i,"address":hex(address+4*i),"target":hex(t)} for i,t in zip(range(-7,1),actual)],
                            "sha256":hashlib.sha256(original).hexdigest(),"original_table_not_rewritten":True,
                            "selected_original_landing_pads":[hex(t) for t in expected[:-1]],
                            "slot_zero_local_block":hex(expected[-1])}
                        if actual!=expected or table!=[hex(expected[-1])] or expected[-1] not in starts or not set(expected[:-1]) <= {int(a,16) for a in LANDING_PADS}:
                            blockers.append(f"Backward landing-pad evidence changed at {ins.address:x}")
                    elif not table or not {int(t,16) for t in table} <= starts:
                        blockers.append(f"Unreviewed indirect branch {ins.address:x}")
                jumps.append(row)
            if ins.group(cs.CS_GRP_INT) or ins.group(cs.CS_GRP_PRIVILEGE):
                blockers.append(f"System/trap instruction {ins.address:x}")
            for operand in ins.operands:
                if operand.type != X86_OP_MEM or operand.mem.base or operand.mem.disp < 0x400000:
                    continue
                mem = operand.mem
                row = {"site": hex(ins.address), "address": hex(mem.disp), "index": ins.reg_name(mem.index), "size": operand.size, "scale": mem.scale}
                globals_.append(row)
                if mem.disp == 0x110d240 and mem.index and mem.scale == 4:
                    continue
                if mem.disp == 0x1bc0468 and not mem.index and operand.size == 4:
                    continue
                if ins.group(cs.CS_GRP_JUMP):
                    continue
                blockers.append(f"Unreviewed global {ins.address:x}")
        functions.append({"address": name, "blocks": blocks, "instructions": instruction_rows,
                          "root_is_block_start":int(name,16) in starts,"fallthrough_outside_blocks":fallthrough,
                          "calls": calls, "jumps": jumps, "global_references": globals_})
        for jump in jumps:
            if "bounded_backward_slots" in jump:
                try:
                    jump["bounded_backward_slots"]["control_flow_bound"]=backward_bound_evidence(instruction_rows,int(jump["site"],16),data["jump_tables"])
                except RuntimeError as error:
                    blockers.append(str(error)+" at "+jump["site"])
    table = bytes_at(0x110d240, 1024)
    native = (source/"runtime/engine_primitives.c").read_text()
    match = re.search(r"crc_table\[256\] = \{(.*?)\};", native, re.S)
    values = [int(v,16) for v in re.findall(r"0x([0-9a-f]+)u", match.group(1))] if match else []
    if len(values) != 256 or table != struct.pack("<256I", *values):
        blockers.append("Original 256-entry CRC table differs from native constant")
    table_records = []
    for function in functions:
        for jump in function["jumps"]:
            if not jump["original_table"]:
                continue
            # Absolute indexed jump slots are original image data, never substituted.
            decoded = next(md.disasm(bytes_at(int(jump["site"],16), 7), int(jump["site"],16)))
            if decoded.operands[0].type != X86_OP_MEM:
                blockers.append("Unexpected jump table encoding"); continue
            address = decoded.operands[0].mem.disp
            targets = [int(t,16) for t in jump["original_table"]]
            site = int(jump["site"],16)
            compact = COMPACTED.get(site)
            slots = ([compact[0]]+targets) if compact else targets
            original = bytes_at(address, len(slots)*4)
            actual, matching = table_slots(original,targets,compact[0] if compact else None)
            if not matching:
                blockers.append(f"Original jump table bytes differ at {site:x}: expected {list(map(hex,slots))}, actual {list(map(hex,actual))}")
            evidence = None
            if compact:
                rows = {int(i["address"],16):i for i in function["instructions"]}
                expected = [(compact[1],"test","edi, 3"),(compact[2],"jne",hex(compact[3])),
                            (compact[3],"mov","eax, edi"),(site-5,"and","eax, 3")]
                if len(targets)!=3 or any(rows.get(a,{}).get("mnemonic")!=m or rows.get(a,{}).get("operands")!=o for a,m,o in expected):
                    blockers.append(f"Compacted table alignment path differs at {site:x}")
                evidence={"discovery_compacts_invalid_slots":True,"valid_index_values":[1,2,3],
                          "skipped_slot":0,"skipped_value":hex(compact[0]),
                          "controlling_instructions":[rows.get(a) for a,_,_ in expected],
                          "reason":"Taken TEST EDI,3 / JNE establishes nonzero low bits; EDI unchanged before MOV EAX,EDI and AND EAX,3"}
            table_records.append({"site": jump["site"], "address": hex(address), "length":len(original), "sha256":hashlib.sha256(original).hexdigest(),
                                  "raw_slots":[{"index":i,"address":hex(address+4*i),"value":hex(v)} for i,v in enumerate(actual)],
                                  "original_compact_targets":jump["original_table"],"compaction_evidence":evidence})
    capability = pe.get_section_by_rva(0x1bc0468-0x400000)
    if capability is None or not capability.Characteristics & 0x80000000:
        blockers.append("Capability fixture is not writable image storage")
    return {"functions": functions, "jump_table_provenance":table_records,
            "crc_table": {"address":"0x110d240", "entries":256, "sha256":hashlib.sha256(table).hexdigest(), "native_table_equal":len(values)==256 and table==struct.pack("<256I",*values)},
            "capability_fixture": {"address":"0x1bc0468", "values":[0,1], "owner":"unchanged upstream oracle", "section":capability.Name.rstrip(b"\0").decode("ascii") if capability else None},
            "blockers":sorted(set(blockers)), "original_indirect_jumps":data.get("indirect_jumps", [])}


def adapt_oracle(text):
    """Preserve comparison/dispatch bodies; only defined scratch input and logs."""
    old = 'default:fprintf(stderr,"unexpected oracle call %x\\n",t);abort();'
    init = "unsigned args[4];"
    if text.count(old) != 1 or text.count(init) != 1 or text.count("uint8_t *MEM;int g_log_level=0;") != 1:
        raise RuntimeError("Pinned upstream oracle adapter fragments changed")
    text = text.replace(old, 'default:fprintf(stderr,"UNRESOLVED guest target=0x%08x\\n",t);abort();')
    text = text.replace(init, "unsigned args[4]={0};")
    return text.replace("uint8_t *MEM;int g_log_level=0;", r'''uint8_t *MEM;int g_log_level=0;
void rt_unimpl(CPU *c,uint32_t a,const char *op){(void)c;fprintf(stderr,"UNSUPPORTED addr=0x%08x op=%s\n",a,op);abort();}
void rt_int3(CPU *c,uint32_t a){(void)c;fprintf(stderr,"UNEXPECTED INT3 addr=0x%08x\n",a);abort();}''')


def oracle_worker(source, candidate, work):
    source, candidate, work = map(Path, (source,candidate,work))
    class RetainedTemporaryDirectory:
        def __init__(self,*a,**k): self.name = tempfile.mkdtemp(prefix="oracle-",dir=work)
        def __enter__(self): return self.name
        def __exit__(self,*a): return False
    original_write, original_run, original_temp = Path.write_text, subprocess.run, tempfile.TemporaryDirectory
    counter = 0
    def write(path,text,*a,**k):
        if path.name == "oracle.c":
            original_write(path.with_name("oracle-upstream.c"),text,*a,**k)
            text = adapt_oracle(text)
        return original_write(path,text,*a,**k)
    def bounded_run(args,**kwargs):
        nonlocal counter
        counter += 1
        compile_step = args[0] == "cc"
        if compile_step:
            args = [args[0],"-save-temps=obj","-Wall","-Wextra",*args[1:]]
        elif not Path(args[0]).resolve().is_relative_to(work.resolve()):
            raise RuntimeError("Oracle executable outside private evidence")
        return command(args,work,f"oracle-{counter}-"+("compile" if compile_step else "run"),seconds=120 if compile_step else 60,memory=1024 if compile_step else 128)
    try:
        Path.write_text, subprocess.run, tempfile.TemporaryDirectory = write,bounded_run,RetainedTemporaryDirectory
        sys.path.insert(0,str(source/"tools"))
        sys.argv = [str(source/"tools/check_engine_primitives.py"),str(candidate)]
        runpy.run_path(sys.argv[0],run_name="__main__")
    finally:
        Path.write_text, subprocess.run, tempfile.TemporaryDirectory = original_write,original_run,original_temp


def prepare(args, work):
    candidate, discovery = args.candidate.resolve(),args.discovery.resolve()
    if candidate.stat().st_size > 256*1024**2 or discovery.stat().st_size > 64*1024**2:
        raise RuntimeError("Diagnostic input size bound exceeded")
    if digest(candidate) != INPUT_SHA or digest(discovery) != DISCOVERY_SHA:
        raise RuntimeError("Exact candidate/discovery identity mismatch")
    packages = dependencies()
    command(["git","--version"],work,"git-version")
    command(["git","-c","core.autocrlf=false","-C",str(ROOT/"vendor/skydream"),"archive","--format=tar","-o",str(work/"source.tar"),PIN],work,"archive")
    source = work/"source"; source.mkdir()
    with tarfile.open(work/"source.tar") as archive: archive.extractall(source,filter="data")
    command(["git","-C",str(source),"init","-q"],work,"isolate-source")
    for n,(name, expected) in enumerate(PATCHES.items()):
        patch = ROOT/"patches/skydream"/name
        if digest(patch) != expected: raise RuntimeError("Reviewed patch hash changed: "+name)
        shutil.copyfile(patch,work/name)
        command(["git","-c","core.autocrlf=false","-C",str(source),"apply","--check",str(work/name)],work,f"patch-{n}-check")
        command(["git","-c","core.autocrlf=false","-C",str(source),"apply",str(work/name)],work,f"patch-{n}-apply")
    inputs = work/"inputs"; inputs.mkdir()
    shutil.copyfile(candidate,inputs/"candidate.exe")
    shutil.copyfile(discovery,inputs/"original-functions.json")
    data = json.loads((inputs/"original-functions.json").read_text())
    write_json(inputs/SELECTION_FILE,subset(data))
    audit = audit_image(inputs/"candidate.exe",data,source)
    audit.update(schema=2,scope="lifted-original-vs-native, five roots, two SSE helpers and fourteen exact backward-copy landing-pad roots",
        selected_roots=list(SELECTED),landing_pad_roots=list(LANDING_PADS),
        source_revision=PIN,patch_sha256=PATCHES,input_sha256=INPUT_SHA,discovery_sha256=DISCOVERY_SHA,
        original_input=str(candidate),original_discovery=str(discovery),dependencies=packages,
        python={"executable":sys.executable,"version":sys.version,"sha256":digest(sys.executable)},
        source_sha256=tree_hashes(source),source_archive_sha256=digest(work/"source.tar"),
        selected_functions_sha256=digest(inputs/SELECTION_FILE),
        runner_sha256=digest(__file__),lifecycle_sha256=digest(ROOT/"scripts/check_private_strings.py"),
        game_executable_launched=False,translated_guest_functions_executed=False,full_game_generation=False,
        compatibility_profile_approved=False,compile_concurrency=1,
        bounds={"private_image_bytes":0x2000000,"payload_window":[0x30000,0x35000],"source":[0x31000,0x3101f],"max_length":4096,"translation_table":[0x40000,0x40100],"stack_top":0x20000,"oracle_seconds":60,"oracle_address_space_mib":128},
        limitations=["Finite 4000 x 5 upstream cases; not complete equivalence or guest ABI proof",
          "Reference executes lifted C, not original x86 instructions; shared lifter errors remain possible",
          "Arithmetic flags are function-local; not preserved across generated calls",
          "Two empty tables and two nonempty zero-forward tables omit negative slots; original discovery evidence is retained unchanged",
          "Fourteen original landing-pad roots cover the bounded backward-copy slots; unknown dynamic targets still abort",
          "No hardware memory-access sandbox inside the 32 MiB oracle arena; bad lifted addresses can crash the bounded process",
          "No iPhone execution, complete game identity or profile approval"],
        oracle_adaptations=["Retain original C, adapted C, memory image, compiler intermediates and logs",
          "Zero initialize args[4] so unused move argument is defined; used arguments and comparison unchanged",
          "Address-bearing fatal unknown-call/instruction/INT3 diagnostics; no helper replacement"])
    if digest(candidate)!=INPUT_SHA or digest(discovery)!=DISCOVERY_SHA or digest(inputs/"candidate.exe")!=INPUT_SHA:
        raise RuntimeError("Original input changed during preparation")
    write_json(work/"audit.json",audit)
    return {"status":"prepared_with_blockers" if audit["blockers"] else "prepared_for_review",
            "audit_sha256":digest(work/"audit.json"),"blockers":audit["blockers"]}


def execute(args,work):
    prepared = private_output(args.prepared)
    if digest(prepared/"audit.json") != args.audit_sha256:
        raise RuntimeError("Reviewed audit hash mismatch")
    audit = json.loads((prepared/"audit.json").read_text())
    if audit["blockers"] or audit["source_revision"]!=PIN or audit["patch_sha256"]!=PATCHES:
        raise RuntimeError("Audit blocked or source identity differs")
    if audit["runner_sha256"]!=digest(__file__) or audit["lifecycle_sha256"]!=digest(ROOT/"scripts/check_private_strings.py") or audit["dependencies"]!=dependencies():
        raise RuntimeError("Runner, lifecycle or dependency identity differs from review")
    if audit["python"]!={"executable":sys.executable,"version":sys.version,"sha256":digest(sys.executable)}:
        raise RuntimeError("Python runtime differs from review")
    if tree_hashes(prepared/"source") != audit["source_sha256"]:
        raise RuntimeError("Prepared source changed")
    for path,sha in ((prepared/"inputs/candidate.exe",INPUT_SHA),(prepared/"inputs/original-functions.json",DISCOVERY_SHA),(prepared/"inputs"/SELECTION_FILE,audit["selected_functions_sha256"]),(Path(audit["original_input"]),INPUT_SHA),(Path(audit["original_discovery"]),DISCOVERY_SHA)):
        if digest(path)!=sha: raise RuntimeError("Reviewed input changed: "+str(path))
    shutil.copytree(prepared/"source",work/"source",ignore=shutil.ignore_patterns(".git","__pycache__"))
    shutil.copytree(prepared/"inputs",work/"inputs")
    shutil.copyfile(prepared/"audit.json",work/"audit.json")
    shutil.copyfile(__file__,work/"runner.py")
    shutil.copyfile(ROOT/"scripts/check_private_strings.py",work/"check_private_strings.py")
    source = work/"source"
    if tree_hashes(source)!=audit["source_sha256"] or digest(work/"inputs/candidate.exe")!=INPUT_SHA:
        raise RuntimeError("Private execution snapshot changed")
    environment = dict(os.environ,LIFT_FILES="1",LIFT_GAME="skyrim",LIFT_MEMORDER="1",LIFT_TSO="1",PYTHONDONTWRITEBYTECODE="1",SKYDREAM_NATIVE_PRIMITIVES="1")
    environment.pop("SKYDREAM_PROFILE_PRIMITIVES",None)
    compiler=shutil.which("cc")
    if not compiler: raise RuntimeError("Existing C compiler is unavailable")
    write_json(work/"compiler.json",{"path":compiler,"sha256":digest(compiler)})
    command([compiler,"--version"],work,"compiler-version",seconds=10,memory=128)
    command([sys.executable,"-B",str(source/"tools/lift.py"),str(work/"inputs/candidate.exe"),str(work/"inputs"/SELECTION_FILE),str(source/"gen")],work,"lift-selected-only",seconds=120,memory=2048,env=environment)
    if json.loads((source/"gen/exe_rejected.json").read_text()): raise RuntimeError("Lifter rejected an original body")
    generated = "\n".join(p.read_text() for p in (source/"gen").glob("exe_*.c"))
    definitions = re.findall(r"void f_([0-9A-F]+)(?:_guest)?\(CPU \*c\) \{",generated)
    if sorted(a.lower() for a in definitions)!=sorted(SELECTED): raise RuntimeError("Generated body closure differs from exact audited roots")
    if "rt_unimpl(" in generated or "rt_int3(" in generated:
        raise RuntimeError("Generated unsupported instruction/trap; evidence retained")
    temporary = work/"oracle-temporary"; temporary.mkdir()
    command([sys.executable,"-B",str(work/"runner.py"),"--oracle-worker",str(source),str(work/"inputs/candidate.exe"),str(temporary)],work,"oracle-adapter",seconds=240,memory=2048,env=environment)
    if PASS not in (temporary/"oracle-2-run.stdout").read_text(): raise RuntimeError("Upstream full case count missing")
    return {"status":"finite_lifted_original_vs_native_passed","audit_sha256":args.audit_sha256}


def final_integrity(args, work):
    """Record original, prepared and execution identities even after failures."""
    checks = {}
    def check(label,path,expected):
        try:
            actual=digest(path)
            checks[label]={"path":str(path),"sha256":actual,"expected_sha256":expected,"matches":actual==expected}
        except OSError as error:
            checks[label]={"path":str(path),"error":str(error),"matches":False}
    if args.prepare:
        check("original_candidate",args.candidate,INPUT_SHA)
        check("original_discovery",args.discovery,DISCOVERY_SHA)
    try:
        # The execute preflight may already have rejected this path. Never read
        # it in cleanup without independently enforcing the same private guard.
        prepared = private_output(args.prepared if args.execute else work)
    except (RuntimeError,OSError,ValueError) as error:
        checks["prepared_path"]={"error":str(error),"matches":False}
        return checks
    audit_path = prepared/"audit.json"
    if audit_path.is_file():
        try:
            audit=json.loads(audit_path.read_text())
            if args.execute:
                check("reviewed_audit",audit_path,args.audit_sha256)
                check("original_candidate",Path(audit["original_input"]),INPUT_SHA)
                check("original_discovery",Path(audit["original_discovery"]),DISCOVERY_SHA)
            for prefix,base in (("prepared",prepared),("run",work)) if args.execute else (("prepared",prepared),):
                for name,sha in (("candidate.exe",INPUT_SHA),("original-functions.json",DISCOVERY_SHA),(SELECTION_FILE,audit["selected_functions_sha256"])):
                    check(prefix+"/inputs/"+name,base/"inputs"/name,sha)
                for name,sha in audit["source_sha256"].items():
                    check(prefix+"/source/"+name,base/"source"/name,sha)
        except (KeyError,ValueError,TypeError) as error:
            checks["audit_parse"]={"error":str(error),"matches":False}
    return checks


def main():
    if len(sys.argv)==5 and sys.argv[1]=="--oracle-worker":
        oracle_worker(*sys.argv[2:]); return 0
    parser=argparse.ArgumentParser(description=__doc__)
    mode=parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare",action="store_true");mode.add_argument("--execute",action="store_true")
    parser.add_argument("--candidate",type=Path);parser.add_argument("--discovery",type=Path)
    parser.add_argument("--prepared",type=Path);parser.add_argument("--audit-sha256")
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    if args.prepare and (not args.candidate or not args.discovery): parser.error("--prepare requires --candidate and --discovery")
    if args.execute and (not args.prepared or not args.audit_sha256): parser.error("--execute requires --prepared and reviewed --audit-sha256")
    if os.name!="posix": parser.error("Use approved existing POSIX tooling environment")
    work=private_output(args.output);work.mkdir(parents=True,exist_ok=False)
    report={"status":"started","full_game_generation":False,"game_executable_launched":False,"compatibility_profile_approved":False,"translated_guest_functions_executed":False}
    code=1
    try:
        report.update(prepare(args,work) if args.prepare else execute(args,work));code=0
    except BaseException as error:
        report.update(status="interrupted" if isinstance(error,(DiagnosticInterrupted,KeyboardInterrupt,subprocess.TimeoutExpired)) else "failed",error=str(error))
    finally:
        oracle=work/"oracle-temporary/oracle-2-run.command.json"
        if oracle.is_file(): report["translated_guest_functions_executed"]=bool(json.loads(oracle.read_text()).get("spawned_pid"))
        report["final_integrity"]=final_integrity(args,work)
        if any(not row["matches"] for row in report["final_integrity"].values()):
            report.update(status="failed",integrity_failure=True);code=1
        report["retained_files_sha256"]=tree_hashes(work)
        write_json(work/"report.json",report)
    print(json.dumps({k:v for k,v in report.items() if k not in ("retained_files_sha256","final_integrity")},indent=2))
    print(str(work/"report.json"))
    return code


if __name__=="__main__":
    with scoped_interrupts(): raise SystemExit(main())
