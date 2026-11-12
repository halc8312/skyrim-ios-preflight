"""Compile private generated C on Windows to a signed-later arm64 iOS dylib.

Uses existing Clang and public Darwin headers, not an Apple SDK. The actual
Apple-compiled host checks the complete shared CPU layout when it loads this
framework. No signing credential or private input leaves the local machine.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
LLVM=Path(r"C:\Program Files\LLVM\bin")
DARWIN=Path(r"F:\Theft4\skyrim-ios-lab\tools\windows-ios-guest-probe\zig-public-headers\lib\libc\include\any-darwin-any")
ABI=ROOT / "ios/private-abi"
SYSTEM_SYMBOLS=set("acos asin atan atan2 ceil cos cosh exp exp2 fabs floor fmod frexp ldexp log log10 log2 nearbyint nearbyintf pow remainder round sin sinh sqrt sqrtf tan tanh trunc memcpy memmove memset memcmp setjmp _setjmp abort __stack_chk_fail __stack_chk_guard".split())

def digest(p):
    with Path(p).open("rb") as f:
        return hashlib.file_digest(f,"sha256").hexdigest()

def command(cmd, log, timeout=900):
    with log.open("wb") as f:
        subprocess.run([str(x) for x in cmd],stdout=f,stderr=subprocess.STDOUT,
                       timeout=timeout,check=True)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("stage",type=Path)
    p.add_argument("--jobs",type=int,choices=[1,2],default=2)
    a=p.parse_args()
    stage=a.stage.resolve()
    if not stage.is_relative_to(ROOT / "private"):
        raise ValueError("Private stage required")
    state=json.loads((stage/"generation.json").read_text())
    if not state.get("generation_finished") or not state.get("inputs_unchanged") or state.get("fixed_native_replacements"):
        raise ValueError("Completed original-body generation required")
    build=stage/"arm64-ios"
    objects=build/"objects"
    logs=build/"logs"
    objects.mkdir(parents=True,exist_ok=True)
    logs.mkdir(exist_ok=True)
    runtime=stage/"source/runtime"
    for name, expected in state["source_headers_sha256"].items():
        if digest(runtime/name)!=expected:
            raise ValueError("Generated-code ABI header changed: "+name)
    resource=Path(subprocess.check_output([str(LLVM/"clang.exe"),"-print-resource-dir"],text=True).strip())/"include"
    flags=["--target=arm64-apple-ios18.0","-std=gnu11","-O1","-g0","-fno-strict-aliasing",
           "-fwrapv","-fsigned-char","-fvisibility=hidden","-fno-stack-protector","-fno-builtin",
           "-DLB_STRICT_ORDER","-DGAME_SKYRIM","-w","-nostdlibinc","-isystem",str(resource),
           "-isystem",str(DARWIN),"-I",str(runtime),"-I",str(stage/"gen"),"-I",str(ABI)]
    sources=sorted((stage/"gen").glob("exe_*.c"))+[ABI/"pc_guest_library.c"]
    contract={"compiler":subprocess.check_output([str(LLVM/"clang.exe"),"--version"],text=True),
              "flags":flags,"headers":state["source_headers_sha256"],"abi_header_sha256":digest(ABI/"PcGuestABI.h")}
    contract_file=build/"compile-contract.json"
    if contract_file.exists() and json.loads(contract_file.read_text())!=contract:
        raise ValueError("Compilation settings changed; preserve the old stage")
    contract_file.write_text(json.dumps(contract,indent=2)+"\n")
    def compile_one(source):
        obj=objects/(source.stem+".o")
        stamp=obj.with_suffix(".json")
        source_hash=digest(source)
        if obj.is_file() and stamp.is_file():
            old=json.loads(stamp.read_text())
            if old.get("source_sha256")==source_hash and old.get("object_sha256")==digest(obj):
                return source.name,True
        started=time.monotonic()
        command([LLVM/"clang.exe",*flags,"-c",source,"-o",obj],logs/(source.stem+".log"))
        stamp.write_text(json.dumps({"source_sha256":source_hash,"object_sha256":digest(obj),
                                    "seconds":round(time.monotonic()-started,2)})+"\n")
        return source.name,False
    print(f"Compiling {len(sources)} private translation units, jobs={a.jobs}",flush=True)
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures=[pool.submit(compile_one,s) for s in sources]
        try:
            for n,f in enumerate(as_completed(futures),1):
                name,reused=f.result()
                if n%16==0 or name=="pc_guest_library.c" or n==len(sources):
                    print(f"{n}/{len(sources)} {name} {'reused' if reused else 'compiled'}",flush=True)
        except BaseException:
            for f in futures: f.cancel()
            raise
    object_paths=[objects/(s.stem+".o") for s in sources]
    response=build/"objects.rsp"
    # ld64 -filelist reads literal newline-separated names, not shell quoting.
    response.write_text("\n".join(str(p) for p in object_paths)+"\n")
    undefined=set()
    defined=set()
    for obj in object_paths:
        raw=subprocess.check_output([str(LLVM/"llvm-nm.exe"),"--format=posix",str(obj)],text=True)
        for line in raw.splitlines():
            bits=line.split()
            if len(bits)<2: continue
            (undefined if bits[1]=="U" else defined).add(bits[0])
    imports=sorted(undefined-defined)
    unknown=[s for s in imports if not s.startswith("_") or s[1:] not in SYSTEM_SYMBOLS]
    (build/"imports.json").write_text(json.dumps({"system_imports":imports,"unexpected":unknown},indent=2)+"\n")
    if unknown:
        raise RuntimeError("Unexpected private framework imports: "+", ".join(unknown))
    tbd=build/"libSystem.tbd"
    tbd.write_text("--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-ios ]\ninstall-name: '/usr/lib/libSystem.B.dylib'\nexports:\n  - targets: [ arm64-ios ]\n    symbols: [ "+", ".join(imports)+" ]\n...\n")
    framework=build/"SkyrimGuest.framework"
    framework.mkdir(exist_ok=True)
    binary=framework/"SkyrimGuest"
    exports=build/"exports.txt"
    exports.write_text("_skyrim_guest_api_v1\n")
    command([LLVM/"ld64.lld.exe","-dylib","-arch","arm64","-platform_version","ios","18.0","18.0",
             "-undefined","error","-no_adhoc_codesign","-dead_strip","-install_name",
             "@rpath/SkyrimGuest.framework/SkyrimGuest","-exported_symbols_list",exports,
             "-filelist",response,"-L",build,"-lSystem","-o",binary],logs/"link.log",timeout=600)
    info={"CFBundleDevelopmentRegion":"en","CFBundleExecutable":"SkyrimGuest",
          "CFBundleIdentifier":"org.skyrimios.pcguest","CFBundleInfoDictionaryVersion":"6.0",
          "CFBundleName":"SkyrimGuest","CFBundlePackageType":"FMWK",
          "CFBundleShortVersionString":"0.1.0","CFBundleVersion":"1","MinimumOSVersion":"18.0"}
    (framework/"Info.plist").write_bytes(plistlib.dumps(info))
    result={"schema":1,"framework":str(framework),"bytes":binary.stat().st_size,
            "sha256":digest(binary),"translation_units":len(sources),"target":"arm64-apple-ios18.0",
            "strict_link_succeeded":True,"system_imports":imports,"source_commit":state["source_commit"],
            "public_headers_only":True,"official_apple_sdk_used":False,"signed":False,
            "game_executed":False,"physical_iphone_verified":False}
    (build/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)

if __name__=="__main__": main()
