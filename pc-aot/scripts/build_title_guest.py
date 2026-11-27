"""Compile private generated C to a signed-later arm64 iOS framework.

macOS uses the selected Xcode iPhoneOS SDK. Windows uses explicit LLVM and
public Darwin headers; that cross-build is not an Apple SDK validation. The
host checks the complete shared CPU layout when it loads this framework.
No signing credential or private input leaves the local machine.
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
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1].resolve()
ABI=ROOT / "ios/private-abi"
SYSTEM_SYMBOLS=set("acos asin atan atan2 ceil cos cosh exp exp2 fabs floor fmod frexp ldexp log log10 log2 nearbyint nearbyintf pow remainder round sin sinh sqrt sqrtf tan tanh trunc memcpy memmove memset memcmp setjmp _setjmp abort __stack_chk_fail __stack_chk_guard".split())
# Darwin binds a thread-local variable through dyld's TLV bootstrap. Only the hook
# library keeps per-thread state (the HighHeels frame receipt), so only its object may
# import the symbol. Any other object that needs it still blocks the link.
THREAD_LOCAL_BOOTSTRAP="__tlv_bootstrap"
THREAD_LOCAL_OBJECTS=frozenset({"pc_guest_mod_hooks_library.o"})
TARGET="arm64-apple-ios18.0"
REQUIRED_DARWIN_HEADERS=("stdint.h", "math.h", "setjmp.h", "string.h", "sys/types.h")

def digest(p):
    with Path(p).open("rb") as f:
        return hashlib.file_digest(f,"sha256").hexdigest()

def command(cmd, log, timeout=900):
    with log.open("wb") as f:
        subprocess.run([str(x) for x in cmd],stdout=f,stderr=subprocess.STDOUT,
                       timeout=timeout,check=True)

def output(command):
    return subprocess.check_output([str(x) for x in command],text=True).strip()


def header_fingerprint(directory):
    """Bind a user-supplied header tree so changed headers cannot reuse objects."""
    hashes=hashlib.sha256()
    count=0
    for path in sorted(directory.rglob("*")):
        if path.is_file():
            hashes.update(path.relative_to(directory).as_posix().encode()+b"\0")
            hashes.update(bytes.fromhex(digest(path)))
            count+=1
    if not count:
        raise ValueError("Public Darwin header directory is empty")
    return {"files":count,"sha256":hashes.hexdigest()}


def require_tool(path, label):
    path=Path(path).resolve()
    if not path.is_file():
        raise ValueError(f"Missing {label}; supply a complete toolchain directory")
    return path


def toolchain(llvm_bin=None, darwin_headers=None):
    common=["--target="+TARGET,"-std=gnu11","-O1","-g0","-fno-strict-aliasing",
            "-fwrapv","-fsigned-char","-fvisibility=hidden","-fno-stack-protector",
            "-fno-builtin","-DLB_STRICT_ORDER","-DGAME_SKYRIM","-w"]
    if sys.platform=="darwin":
        if llvm_bin or darwin_headers:
            raise ValueError("macOS uses Xcode; --llvm-bin and --darwin-headers are Windows options")
        xcrun=shutil.which("xcrun")
        if not xcrun:
            raise ValueError("Full Xcode with the iPhoneOS SDK is required; xcrun was not found")
        sdk=Path(output([xcrun,"--sdk","iphoneos","--show-sdk-path"])).resolve()
        if not sdk.is_dir():
            raise ValueError("Selected iPhoneOS SDK directory does not exist")
        clang=require_tool(output([xcrun,"--sdk","iphoneos","--find","clang"]),"Xcode clang")
        nm=require_tool(output([xcrun,"--sdk","iphoneos","--find","nm"]),"Xcode nm")
        identity={"kind":"apple-sdk", "sdk_path":str(sdk),
                  "sdk_version":output([xcrun,"--sdk","iphoneos","--show-sdk-version"]),
                  "sdk_build":output([xcrun,"--sdk","iphoneos","--show-sdk-build-version"])}
        return {"clang":clang,"nm":nm,"nm_flags":["-P"],"sdk":sdk,
                "compile_flags":common+["-isysroot",str(sdk)],"identity":identity,
                "official_apple_sdk_used":True}
    if sys.platform!="win32":
        raise ValueError("Use macOS with Xcode or Windows with LLVM; Linux guest compilation is not supported")
    llvm_bin=llvm_bin or os.environ.get("SKYRIM_LLVM_BIN")
    if not llvm_bin:
        on_path=shutil.which("clang.exe")
        if on_path:
            llvm_bin=Path(on_path).parent
        else:
            llvm_bin=Path(os.environ.get("ProgramFiles",r"C:\Program Files"))/"LLVM/bin"
    llvm=Path(llvm_bin).resolve()
    clang=require_tool(llvm/"clang.exe","LLVM clang.exe (--llvm-bin)")
    nm=require_tool(llvm/"llvm-nm.exe","LLVM llvm-nm.exe (--llvm-bin)")
    linker=require_tool(llvm/"ld64.lld.exe","LLVM ld64.lld.exe (--llvm-bin)")
    darwin_headers=darwin_headers or os.environ.get("SKYRIM_DARWIN_HEADERS")
    if not darwin_headers:
        raise ValueError("Windows requires --darwin-headers pointing to public any-darwin-any headers (or SKYRIM_DARWIN_HEADERS)")
    headers=Path(darwin_headers).resolve()
    if not headers.is_dir() or any(not (headers/name).is_file() for name in REQUIRED_DARWIN_HEADERS):
        raise ValueError("--darwin-headers must contain the complete public any-darwin-any headers, including sys/types.h")
    resource=Path(output([clang,"-print-resource-dir"])).resolve()/"include"
    if not resource.is_dir():
        raise ValueError("LLVM builtin include directory is missing")
    identity={"kind":"windows-public-headers","headers_path":str(headers),
              "darwin_headers":header_fingerprint(headers),
              "resource_headers":header_fingerprint(resource),
              "linker_version":output([linker,"--version"])}
    return {"clang":clang,"nm":nm,"nm_flags":["--format=posix"],"linker":linker,
            "compile_flags":common+["-nostdlibinc","-isystem",str(resource),"-isystem",str(headers)],
            "identity":identity,"official_apple_sdk_used":False}


def import_symbols(tool, objects):
    undefined=set()
    defined=set()
    thread_local_users=set()
    for obj in objects:
        raw=output([tool["nm"],*tool["nm_flags"],obj])
        for line in raw.splitlines():
            bits=line.split()
            if len(bits)<2: continue
            if bits[1]=="U":
                undefined.add(bits[0])
                if bits[0]==THREAD_LOCAL_BOOTSTRAP: thread_local_users.add(Path(obj).name)
            else:
                defined.add(bits[0])
    imports=sorted(undefined-defined)
    thread_local_allowed=thread_local_users<=THREAD_LOCAL_OBJECTS
    unknown=[s for s in imports
             if not (s.startswith("_") and s[1:] in SYSTEM_SYMBOLS)
             and not (s==THREAD_LOCAL_BOOTSTRAP and thread_local_allowed)]
    return imports,unknown


def link_command(tool, build, response, exports, binary, imports):
    if tool["official_apple_sdk_used"]:
        # Pass each linker argument separately: paths may contain spaces or commas.
        options=["-undefined","error","-no_adhoc_codesign","-dead_strip",
                 "-install_name","@rpath/SkyrimGuest.framework/SkyrimGuest",
                 "-exported_symbols_list",str(exports),"-filelist",str(response)]
        linker_args=[arg for option in options for arg in ("-Xlinker",option)]
        return [tool["clang"],"--target="+TARGET,"-isysroot",tool["sdk"],
                "-dynamiclib","-nostdlib",*linker_args,"-lSystem","-o",binary]
    tbd=build/"libSystem.tbd"
    tbd.write_text("--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-ios ]\ninstall-name: '/usr/lib/libSystem.B.dylib'\nexports:\n  - targets: [ arm64-ios ]\n    symbols: [ "+", ".join(imports)+" ]\n...\n")
    return [tool["linker"],"-dylib","-arch","arm64","-platform_version","ios","18.0","18.0",
            "-undefined","error","-no_adhoc_codesign","-dead_strip","-install_name",
            "@rpath/SkyrimGuest.framework/SkyrimGuest","-exported_symbols_list",exports,
            "-filelist",response,"-L",build,"-lSystem","-o",binary]


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("stage",type=Path)
    p.add_argument("--jobs",type=int,choices=[1,2],default=2)
    p.add_argument("--classic-skse", action="store_true",
                   help="Build the opt-in experimental Classic VM/menu guest in its own directory")
    p.add_argument("--mod-hooks", metavar="GROUPS",
                   help="With --classic-skse: comma-separated reviewed MOD hook groups to stage "
                        "(fuz_menu, fuz_world, vm_lifecycle, chargen_precache, chargen_slider)")
    p.add_argument("--llvm-bin",type=Path,help="Windows LLVM bin directory; defaults to SKYRIM_LLVM_BIN, PATH, or Program Files/LLVM/bin")
    p.add_argument("--darwin-headers",type=Path,help="Windows public any-darwin-any include directory; alternatively set SKYRIM_DARWIN_HEADERS")
    a=p.parse_args(argv)
    if sys.version_info<(3,12):
        p.error("Python 3.12 or newer is required")
    stage=a.stage.resolve()
    if not stage.is_relative_to(ROOT / "private"):
        raise ValueError("Private stage required")
    state=json.loads((stage/"generation.json").read_text())
    if not state.get("generation_finished") or not state.get("inputs_unchanged") or state.get("fixed_native_replacements"):
        raise ValueError("Completed original-body generation required")
    hook_groups = 0
    if a.mod_hooks is not None and not a.classic_skse:
        p.error("--mod-hooks is combined with --classic-skse")
    if a.classic_skse:
        from classic_skse_guest import prepare_sources, validate_generation
        validate_generation(state)
    if a.mod_hooks is not None:
        import mod_hooks_guest
        try:
            hook_groups = mod_hooks_guest.parse_groups(a.mod_hooks)
            mod_hooks_guest.validate_generation(state, stage)
        except ValueError as error:
            p.error(str(error))
    inputs={key:state.get(key) for key in ("candidate_sha256","discovery_sha256")}
    if any(not isinstance(value,str) or not re.fullmatch(r"[0-9a-f]{64}",value) for value in inputs.values()):
        raise ValueError("Generation must bind exact candidate_sha256 and discovery_sha256")
    generated_sources=sorted((stage/"gen").glob("exe_*.c"))
    if not generated_sources:
        raise ValueError("Generated exe_*.c sources are missing; complete guest generation first")
    try:
        tool=toolchain(a.llvm_bin,a.darwin_headers)
    except (ValueError,OSError,subprocess.SubprocessError) as error:
        p.error(str(error))
    build=stage/("arm64-ios-classic-skse" if a.classic_skse else "arm64-ios")
    if hook_groups:
        build=build.with_name(f"{build.name}-hooks-{hook_groups}")
    objects=build/"objects"
    logs=build/"logs"
    objects.mkdir(parents=True,exist_ok=True)
    logs.mkdir(exist_ok=True)
    runtime=stage/"source/runtime"
    for name, expected in state["source_headers_sha256"].items():
        if digest(runtime/name)!=expected:
            raise ValueError("Generated-code ABI header changed: "+name)
    flags=[*tool["compile_flags"],"-I",str(runtime),"-I",str(stage/"gen"),"-I",str(ABI)]
    declarations_hash = None
    if a.classic_skse:
        declarations_hash = digest(stage/"gen/funcs_exe.h")
        generated_sources=prepare_sources(generated_sources, build/"classic-sources")
    staged_units = {}
    if hook_groups:
        generated_sources, staged_units = mod_hooks_guest.prepare_sources(
            generated_sources, build/"mod-hook-sources", hook_groups, state, stage)
    sources=generated_sources+[ABI/"pc_guest_library.c"]
    contract={"compiler":output([tool["clang"],"--version"]),"toolchain":tool["identity"],
              "inputs":inputs,"source_commit":state["source_commit"],
              "flags":flags,"headers":state["source_headers_sha256"],"abi_header_sha256":digest(ABI/"PcGuestABI.h")}
    if a.classic_skse:
        sources.append(ABI/"pc_guest_skse_library.c")
        contract.update(classic_skse_experiment=True,
                        classic_abi_header_sha256=digest(ABI/"PcGuestSkseABI.h"))
    if hook_groups:
        sources.append(ABI/"pc_guest_mod_hooks_library.c")
        contract.update(mod_hook_groups=hook_groups, mod_hook_group_names=mod_hooks_guest.group_names(hook_groups),
                        mod_hooks_header_sha256=mod_hooks_guest.header_hashes(),
                        mod_hook_staged_units=staged_units)
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
            if (old.get("source_sha256")==source_hash and old.get("object_sha256")==digest(obj)
                    and (not a.classic_skse or old.get("generated_declarations_sha256")==declarations_hash)):
                return source.name,True
        started=time.monotonic()
        options = [*flags, "-DSKYRIM_IOS_CLASSIC_SKSE=1"] if a.classic_skse and source.parent == ABI else flags
        if hook_groups and (source.name in staged_units or source.name == "pc_guest_mod_hooks_library.c"):
            # Only the staged units and the hook library see the reviewed seam headers.
            options = [*options, "-I", str(mod_hooks_guest.SEAMS), "-I", str(mod_hooks_guest.SAVE_SEAMS),
                       f"-DPC_GUEST_MOD_HOOKS_COMPILED={hook_groups}u"]
        command([tool["clang"],*options,"-c",source,"-o",obj],logs/(source.stem+".log"))
        evidence = {"source_sha256":source_hash, "object_sha256":digest(obj),
                    "seconds":round(time.monotonic()-started,2)}
        if a.classic_skse:
            evidence["generated_declarations_sha256"] = declarations_hash
        stamp.write_text(json.dumps(evidence)+"\n")
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
    imports,unknown=import_symbols(tool,object_paths)
    (build/"imports.json").write_text(json.dumps({"system_imports":imports,"unexpected":unknown},indent=2)+"\n")
    if unknown:
        raise RuntimeError("Unexpected private framework imports: "+", ".join(unknown))
    framework=build/"SkyrimGuest.framework"
    framework.mkdir(exist_ok=True)
    binary=framework/"SkyrimGuest"
    exports=build/"exports.txt"
    exports.write_text(("_skyrim_guest_classic_api_v1\n_skyrim_guest_bind_classic_v1\n"
                        if a.classic_skse else "_skyrim_guest_api_v1\n")
                       + ("_skyrim_guest_bind_mod_hooks_v1\n" if hook_groups else "")
                       + ("_skyrim_guest_bind_menu_focus_v1\n"
                          if hook_groups and hook_groups & mod_hooks_guest.GROUPS["loading_menu_observe"] else ""))
    command(link_command(tool,build,response,exports,binary,imports),logs/"link.log",timeout=600)
    info={"CFBundleDevelopmentRegion":"en","CFBundleExecutable":"SkyrimGuest",
          "CFBundleIdentifier":"org.skyrimios.pcguest","CFBundleInfoDictionaryVersion":"6.0",
          "CFBundleName":"SkyrimGuest","CFBundlePackageType":"FMWK",
          "CFBundleShortVersionString":"0.1.0","CFBundleVersion":"1","MinimumOSVersion":"18.0"}
    if a.classic_skse:
        info["PcGuestClassicSkseExperiment"] = True
    if hook_groups:
        info["PcGuestModHooks"] = hook_groups
    (framework/"Info.plist").write_bytes(plistlib.dumps(info))
    result={"schema":1,"framework":str(framework),"bytes":binary.stat().st_size,
            "sha256":digest(binary),"translation_units":len(sources),"target":"arm64-apple-ios18.0",
            "strict_link_succeeded":True,"system_imports":imports,"source_commit":state["source_commit"],
            **inputs,
            "public_headers_only":not tool["official_apple_sdk_used"],
            "official_apple_sdk_used":tool["official_apple_sdk_used"],"signed":False,
            "game_executed":False,"physical_iphone_verified":False}
    if a.classic_skse:
        result["classic_skse_experiment"] = True
    if hook_groups:
        result["mod_hook_groups"] = hook_groups
    (build/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)

if __name__=="__main__": main()
