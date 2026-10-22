#!/usr/bin/env bash
# Only hand-authored public synthetic entries and generic adapter code enter.
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 4
stage="$(cd "$1" && pwd)"
simde="$(cd "$2" && pwd)"
out="$3"
case "$4" in
  device) sdk=iphoneos; target=arm64-apple-ios26.0; platform=2 ;;
  simulator) sdk=iphonesimulator; target=arm64-apple-ios26.0-simulator; platform=7 ;;
  *) exit 2 ;;
esac
harness="$(cd "$(dirname "$0")/.." && pwd)"
source="$stage/skyrim-recomp-web"
generic="$stage/tools/windows-ios-guest-probe/owned-guest-abi"
host="$stage/tools/ios-owned-game-host"
backend="$stage/tools/ios-resident-guest-probe/apple-backend"
test ! -e "$out"
mkdir "$out"
out="$(cd "$out" && pwd)"
python3 "$backend/prepare_fixture.py" --source "$source" --generic "$generic" --output "$out/include"
sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
clang="$(xcrun --sdk "$sdk" --find clang++)"
c_clang="$(xcrun --sdk "$sdk" --find clang)"
printf '%s\n' 'int main(void) { return 0; }' > "$out/sdk-load-probe.c"
"$c_clang" -target "$target" -isysroot "$sdk_path" -O2 "$out/sdk-load-probe.c" -o "$out/sdk-load-probe"
python3 - "$out/sdk-load-probe" "$generic/system-load-metadata.tbd" "$out/system-load-metadata.tbd" <<'PY'
from pathlib import Path
import struct,sys
b=Path(sys.argv[1]).read_bytes()
assert len(b)>=32 and struct.unpack_from('<I',b)[0]==0xfeedfacf
n,size=struct.unpack_from('<II',b,16);assert n<=512 and 32+size<=len(b)
offset=32;system=[]
for _ in range(n):
    command,length=struct.unpack_from('<II',b,offset)
    assert length>=8 and length%8==0 and offset+length<=32+size
    if command==0xc:
        name,_,current,compat=struct.unpack_from('<IIII',b,offset+8)
        assert 24<=name<length
        value=b[offset+name:offset+length].split(b'\0',1)[0]
        if value==b'/usr/lib/libSystem.B.dylib':system.append((current,compat))
    offset+=length
assert offset==32+size and len(system)==1
assert all(value>=0x10000 for value in system[0])
def version(value):return f'{value>>16}.{value>>8&255}.{value&255}'
# Synthesize only public load metadata; no SDK file or symbol implementation is
# copied. The original eight-line implementation-free template stays unchanged.
text=Path(sys.argv[2]).read_text()
assert text.count('current-version: 1.0\n')==1 and text.count('compatibility-version: 1.0\n')==1
text=text.replace('current-version: 1.0\n','current-version: '+version(system[0][0])+'\n')
text=text.replace('compatibility-version: 1.0\n','compatibility-version: '+version(system[0][1])+'\n')
with Path(sys.argv[3]).open('x') as stream:stream.write(text)
PY
flags=(-target "$target" -isysroot "$sdk_path" -std=c++20 -O2 -fPIC
       -ffreestanding -fno-builtin -fno-stack-protector -fno-rtti
       -fexceptions -funwind-tables -ffp-model=strict -fvisibility=hidden
       -DSIMDE_ENABLE_NATIVE_ALIASES -I"$out/include" -I"$generic" -I"$simde")
"$clang" "${flags[@]}" -c "$generic/owned_abi.cpp" -o "$out/owned_abi.o"
"$clang" -target "$target" -isysroot "$sdk_path" -c "$generic/jump_tail.S" -o "$out/jump_tail.o"
"$clang" "${flags[@]}" -c "$host/native/PublicOwnedFixture.cpp" -o "$out/fixture.o"
"$clang" "${flags[@]}" -S -emit-llvm "$host/native/PublicOwnedFixture.cpp" -o "$out/fixture.ll"
framework="$out/PrivateGuest.framework"
mkdir "$framework"
printf '%s\n' _PG_GetRequirementsV1 _PG_BindHostV1 _PG_PublishMemoryV1 \
  _PG_InitContextV1 _PG_CallGuestV1 _skyrt_ppc_config _skyrt_func_addrs > "$out/exports.txt"
# No compiler-default libc++/other load is admitted. The empty public TBD adds
# only libSystem load metadata, never an implementation for an unknown symbol.
"$clang" -target "$target" -isysroot "$sdk_path" -dynamiclib -nostdlib \
  "$out/owned_abi.o" "$out/jump_tail.o" "$out/fixture.o" \
  -Wl,-install_name,@rpath/PrivateGuest.framework/PrivateGuest \
  -Wl,-exported_symbols_list,"$out/exports.txt" -Wl,-undefined,error -Wl,-no_adhoc_codesign \
  "-Wl,-needed_library,$out/system-load-metadata.tbd" -o "$framework/PrivateGuest"
python3 - "$framework" "$4" <<'PY'
from pathlib import Path
import plistlib,sys
path=Path(sys.argv[1])
(path/'Info.plist').write_bytes(plistlib.dumps({
    'CFBundleExecutable':'PrivateGuest','CFBundleIdentifier':'local.public.OwnedFixture',
    'CFBundleName':'PrivateGuest','CFBundlePackageType':'FMWK',
    'CFBundleShortVersionString':'0.1','CFBundleVersion':'1','MinimumOSVersion':'26.0',
    'CFBundleSupportedPlatforms':['iPhoneOS' if sys.argv[2]=='device' else 'iPhoneSimulator']}))
PY
xcrun nm -gjU "$framework/PrivateGuest" | LC_ALL=C sort > "$out/actual-exports.txt"
LC_ALL=C sort "$out/exports.txt" > "$out/expected-exports.txt"
cmp "$out/actual-exports.txt" "$out/expected-exports.txt"
xcrun nm -u "$framework/PrivateGuest" > "$out/undefined.txt"
test ! -s "$out/undefined.txt"
xcrun nm -a "$framework/PrivateGuest" > "$out/symbols.txt"
xcrun otool -tvV "$framework/PrivateGuest" > "$out/disassembly.txt"
python3 "$harness/ci/validate-owned-host-fixture.py" --fixture "$framework/PrivateGuest" \
  --sdk-probe "$out/sdk-load-probe" --platform "$platform" > "$out/link-metadata.json"
python3 - "$out" <<'PY'
from pathlib import Path
import re,sys
out=Path(sys.argv[1]);ir=(out/'fixture.ll').read_text()
attrs=dict(re.findall(r'attributes #(\d+) = \{([^\n]*)',ir))
declaration=re.search(r'^declare[^\n]*@setjmp\([^\n]*#(\d+)',ir,re.M)
calls=re.findall(r'\b(?:call|invoke)[^\n]*@setjmp\([^\n]*#(\d+)',ir)
assert declaration and calls
assert 'returns_twice' in attrs.get(declaration[1],'')
assert all('returns_twice' in attrs.get(number,'') for number in calls)
symbols=(out/'symbols.txt').read_text();disassembly=(out/'disassembly.txt').read_text()
for name in ('_setjmp','_longjmp','__Unwind_Resume'):
    assert re.search(r'(?m)^[0-9a-fA-F]+\s+[tT]\s+'+re.escape(name)+r'$',symbols)
    block=re.search(r'(?m)^'+re.escape(name)+r':\n((?:[0-9a-fA-F]{16}\t[^\n]*\n)+)',disassembly)
    assert block
    lines=[line.split('\t')[1:] for line in block[1].splitlines()]
    operations=[line[0] for line in lines]
    assert 'br' in operations and not set(operations)&{'blr','ret','stp','ldp','sub'}
    assert not any(re.search(r'\b(?:sp|x29|x30)\b',' '.join(line)) for line in lines)
    assert re.search(r'\tbl\t'+re.escape(name)+r'(?:\n|$)',disassembly)
print('Public SDK fixture: seven exports, no imports, owned native tails and actual returns_twice callsite.')
PY
