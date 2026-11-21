"""Check that selected local DLLs are read, diagnosed and left intact."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

from test_pe_image import BASE, DATA, RD, SECTIONS, add_named_clock, directory, make_image, put, zero_string

HERE = Path(__file__).resolve().parent


def supported_fixture():
    dll = make_image()
    struct.pack_into("<HH", dll, RD + 0x70, 0, 1)
    dll[0x200:0x206] = bytes.fromhex("b8 01 00 00 00 c3")
    # SKSEPlugin_Query fills PluginInfo in the scratch pointer at [esp+8].
    query = bytes.fromhex("8b 54 24 08 c7 02 01 00 00 00 c7 42 04")
    query += struct.pack("<I", BASE + 0x20c4)
    query += bytes.fromhex("c7 42 08 07 00 00 00 b8 01 00 00 00 c3")
    dll[0x210:0x210 + len(query)] = query
    zero_string(dll, RD + 0xc4, b"Owned test plugin")
    return dll


def serializing_fixture():
    dll = supported_fixture()
    put(dll, SECTIONS + 8, "I", 0x200)
    put(dll, RD + 0x60, "I", 0x1080)
    zero_string(dll, RD + 0x120, b"trial record")
    # The authored load thunk asks for v4 serialization, then registers a
    # UID plus save and load callbacks. EBX and ESI retain the interface
    # table and the owned handle across cdecl function calls.
    code = bytearray.fromhex(
        "53 56 8b 44 24 0c 6a 03 ff 50 10 83 c4 04 85 c0 74 00 "
        "89 c3 8b 44 24 0c ff 50 14 89 c6"
    )
    rejected = 17
    for field, number in ((4, 0x191d77), (12, BASE + 0x1100), (16, BASE + 0x1140)):
        code.extend(b"\x68" + struct.pack("<I", number))
        code.extend(bytes((0x56, 0xff, 0x53, field, 0x83, 0xc4, 8)))
    code.extend(bytes.fromhex("b8 01 00 00 00 5e 5b c3"))
    code[rejected] = len(code) - rejected - 1
    code.extend(bytes.fromhex("31 c0 5e 5b c3"))
    dll[0x280:0x280 + len(code)] = code

    # Save copies an authored byte into one record. Load checks that its
    # metadata was delivered before consuming the first byte.
    save = bytes.fromhex("8b 44 24 04 6a 01") + b"\x68" + struct.pack("<I", BASE + 0x2120)
    save += bytes.fromhex("6a 03 6a 17 ff 50 18 83 c4 10 c3")
    dll[0x300:0x300 + len(save)] = save
    load = bytearray.fromhex("53 8b 5c 24 08")
    for address in (BASE + 0x3128, BASE + 0x3124, BASE + 0x3120):
        load.extend(b"\x68" + struct.pack("<I", address))
    load.extend(bytes.fromhex("ff 53 24 83 c4 0c 85 c0 75 02 0f 0b"))
    for address, expected in ((BASE + 0x3120, 0x17), (BASE + 0x3124, 3), (BASE + 0x3128, 1)):
        load.extend(b"\x83\x3d" + struct.pack("<I", address) + bytes((expected, 0x74, 2, 0x0f, 0x0b)))
    load.extend(bytes.fromhex("6a 01"))
    load.extend(b"\x68" + struct.pack("<I", BASE + 0x312c))
    load.extend(bytes.fromhex("ff 53 28 83 c4 08 83 f8 01 74 02 0f 0b"))
    load.extend(b"\x80\x3d" + struct.pack("<I", BASE + 0x312c) + b"t")
    load.extend(bytes.fromhex("74 02 0f 0b 5b c3"))
    dll[0x340:0x340 + len(load)] = load
    return dll


def worker_fixture(close_before_join=False, worker_runs_forever=False):
    dll = supported_fixture()
    add_named_clock(dll)
    directory(dll, 12, 0x3060, 16)
    for table in (0x60, 0x80):
        put(dll, DATA + table, "4I", 0x30a0, 0x30c0, 0x30e0, 0)
    for offset, name in ((0xa0, b"CreateThread"), (0xc0, b"WaitForSingleObject"), (0xe0, b"CloseHandle")):
        zero_string(dll, DATA + offset + 2, name)
    put(dll, SECTIONS + 8, "I", 0x200)
    put(dll, RD + 0x60, "I", 0x1080)
    output = BASE + 0x3140
    code = bytearray(b"\x53")  # Keep the worker handle in EBX.

    def call(slot):
        code.extend(b"\xff\x15" + struct.pack("<I", BASE + 0x3060 + slot * 4))

    for value in (0, 0, output, BASE + 0x1100, 0, 0):
        code.extend(b"\x68" + struct.pack("<I", value))
    call(0)
    code.extend(bytes.fromhex("85 c0 75 02 0f 0b 89 c3"))
    if not close_before_join:
        code.extend(bytes.fromhex("6a ff 53"))
        call(1)
        code.extend(bytes.fromhex("85 c0 74 02 0f 0b"))
        code.extend(bytes.fromhex("83 3d") + struct.pack("<I", output) + bytes.fromhex("49 74 02 0f 0b"))
    code.append(0x53)
    call(2)
    code.extend(bytes.fromhex("5b c3"))
    dll[0x280:0x280 + len(code)] = code
    # The worker receives a pointer and leaves an observable value before returning.
    worker = bytes.fromhex("8b 44 24 04 c7 00 49 00 00 00 31 c0 c2 04 00")
    if worker_runs_forever:
        worker = bytes.fromhex("eb fe")
    dll[0x300:0x300 + len(worker)] = worker
    return dll


def task_fixture(bad_vtable=False):
    dll = supported_fixture()
    put(dll, SECTIONS + 8, "I", 0x200)
    put(dll, RD + 0x60, "I", 0x1080)
    put(dll, RD + 0x140, "2I", BASE + 0x1100, BASE + 0x1180)
    put(dll, RD + 0x150, "2I", BASE + 0x1100, BASE + 0x1140)
    for i, expected in enumerate((0, 2, 6, 4)):
        table = 0xfffffffe if bad_vtable and i == 1 else BASE + 0x2140
        put(dll, DATA + 0x140 + i * 8, "2I", table, expected)

    # Keep the checked Classic interface in EBX, then enqueue two game tasks
    # and one UI task whose delegates share methods but carry distinct state.
    code = bytearray.fromhex("53 8b 44 24 08 6a 04 ff 50 10 83 c4 04 85 c0 75 02 0f 0b")
    code.extend(bytes.fromhex("83 38 02 74 02 0f 0b 89 c3"))
    interface = struct.pack("<I", BASE + 0x31f4)
    code.extend(b"\xa3" + interface)
    for i in range(3):
        code.extend(b"\x68" + struct.pack("<I", BASE + 0x3140 + i * 8))
        code.extend(bytes((0xff, 0x53, 8 if i == 2 else 4, 0x83, 0xc4, 4)))
    code.extend(bytes.fromhex("b8 01 00 00 00 5b c3"))
    dll[0x280:0x280 + len(code)] = code

    stage = struct.pack("<I", BASE + 0x31f0)
    check_and_advance = bytes.fromhex("39 05") + stage + bytes.fromhex("74 02 0f 0b ff 05") + stage
    nested = b"\x68" + struct.pack("<I", BASE + 0x3158)
    nested += b"\x8b\x15" + interface + bytes.fromhex("ff 52 04 83 c4 04")
    for i in range(2):
        # Run changes the vptr. Dispose must read the new slot, as Classic does.
        action = b"\x40" if i else b"\xc7\x01" + struct.pack("<I", BASE + 0x2150)
        method = bytes.fromhex("8b 41 04") + action + check_and_advance
        if not i:
            # The first Run appends a game task behind the existing second task.
            method += bytes((0x85, 0xc0, 0x75, len(nested))) + nested
        method += b"\xc3"
        at = 0x300 + 0x40 * i
        dll[at:at + len(method)] = method
    dll[0x380:0x382] = b"\x0f\x0b"
    return dll


def compile_local_probe(cc, binary, *, json_trial=False, settings_backend=False, extra_flags=(), extra_sources=()):
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
    if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if sys.platform.startswith("linux"):
            flags += ["-fno-pie", "-no-pie"]
    sources = ("pe_image.c", "x86_call.c", "x86_heap.c", "x86_winthread.c", "x86_wintext.c", "x86_winfile.c",
               "x86_winsync.c", "x86_winenv.c", "papyrus_trial.c", "x86_skse_records.c", "dependency_plan.c",
               "probe_local_file.c", "fixtures/owned_storage.c")
    libraries = []
    if json_trial or settings_backend:
        flags.append("-pthread")
        sources += ("classic_pex.c", "classic_scalar.c", "../skse-profile-json/profile_json.c",
                    "../skse-profile-json/profile_overlay.c", "../skse-cosave/records.c")
        if settings_backend:
            port = HERE.parents[1] / "ios/runtime-port"
            flags += ["-D_POSIX_C_SOURCE=200809L", "-DSKYRIM_CLASSIC_SETTINGS_BACKEND=1"]
            flags += ["-I", str(port)]
            sources += ("plugin_trial.c", "probe_asset_file.c", "fixtures/settings_session.c", port / "mod_papyrus_inputs.c")
            if sys.platform != "darwin":
                libraries.append("-lcrypto")
        else:
            sources += ("fixtures/owned_json.c",)
    else:
        sources += ("plugin_trial.c", "test_probe_local_file.c")
    subprocess.run([cc, *flags, *extra_flags, "-I", HERE, *(HERE / source for source in sources),
                    *extra_sources, *libraries, "-o", binary], check=True, timeout=60)


def owner_bootstrap_words(executable):
    # Verify the same exact supported PE as the ordinary title generator.
    pin_file = HERE.parents[1] / "scripts/prepare_title_guest.py"
    pin = next(line for line in pin_file.read_text(encoding="utf-8").splitlines()
               if line.startswith("CANDIDATE_SHA = ")).partition("=")[2].strip().strip('"')
    game = executable.read_bytes()
    if hashlib.sha256(game).hexdigest() != pin:
        raise ValueError("The executable does not match the pinned Skyrim LE input")
    nt = struct.unpack_from("<I", game, 0x3c)[0]
    if game[nt:nt + 4] != b"PE\0\0":
        raise ValueError("No PE header")
    machine, section_count = struct.unpack_from("<HH", game, nt + 4)
    optional_size = struct.unpack_from("<H", game, nt + 20)[0]
    magic = struct.unpack_from("<H", game, nt + 24)[0]
    image_base = struct.unpack_from("<I", game, nt + 52)[0]
    if (machine, magic, image_base) != (0x14c, 0x10b, 0x400000):
        raise ValueError("Unexpected owner PE address model")
    addresses = (0x0124166c, 0x012b5b80)
    result, verified_hooks = {}, set()
    call_sites = {0x006a9e90: 0x0040e4f0, 0x008cab54: 0x00a51280}
    animation_site = 0x006f02ff
    for i in range(section_count):
        section = nt + 24 + optional_size + i * 40
        name = game[section:section + 8].rstrip(b"\0")
        flags = struct.unpack_from("<I", game, section + 36)[0]
        virtual_size, rva, file_size, raw_offset = struct.unpack_from("<IIII", game, section + 8)
        if name == b".text" and flags & 0x20000000:
            for address, target in call_sites.items():
                offset = address - image_base - rva
                if 0 <= offset and offset + 5 <= min(virtual_size, file_size):
                    raw = raw_offset + offset
                    if game[raw] == 0xe8 and address + 5 + struct.unpack_from("<i", game, raw + 1)[0] == target:
                        verified_hooks.add(address)
            offset = animation_site - image_base - rva
            if 0 <= offset and offset + 6 <= min(virtual_size, file_size):
                raw = raw_offset + offset
                test, modrm, field, mask, branch, displacement = game[raw:raw + 6]
                if (test, modrm, field, mask, branch) == (0xf6, 0x46, 0x2c, 1, 0x74) and displacement == 0x0d:
                    verified_hooks.add(animation_site)
        if name != b".data" or flags & 0x20000000 or not flags & 0x40000000:
            continue
        for address in addresses:
            offset = address - image_base - rva
            if 0 <= offset and offset + 4 <= min(virtual_size, file_size) and raw_offset + offset + 4 <= len(game):
                result[address] = game[raw_offset + offset:raw_offset + offset + 4]
    if len(result) != len(addresses):
        raise ValueError("The approved initializers are missing from the file-backed PE data")
    if verified_hooks != {*call_sites, animation_site}:
        raise ValueError("The experimental hook sites do not match the pinned input")
    return b"".join(result[address] for address in addresses)


def owned_register_integer_method(library, callback):
    nt = struct.unpack_from("<I", library, 0x3c)[0]
    section_count = struct.unpack_from("<H", library, nt + 6)[0]
    optional_size = struct.unpack_from("<H", library, nt + 20)[0]
    base = struct.unpack_from("<I", library, nt + 52)[0]

    def backed(address, length, executable):
        rva = address - base
        for i in range(section_count):
            header = nt + 24 + optional_size + 40 * i
            virtual_size, start, file_size, offset = struct.unpack_from("<IIII", library, header + 8)
            flags = struct.unpack_from("<I", library, header + 36)[0]
            delta = rva - start
            if bool(flags & 0x20000000) == executable and flags & 0x40000000 and \
               0 <= delta and delta + length <= min(virtual_size, file_size) and offset + delta + length <= len(library):
                return library[offset + delta:offset + delta + length]
        raise ValueError("Owned declaration used an unexpected PE section")

    # First declaration inside the observed Classic RegisterFunctions callback.
    # Verify the NativeFunction ctor call and the static no-argument int record.
    constructor = re.compile(
        rb"\x68(.{4})\xc6\x45\xfc\x01\xb8\x00\x6c\xc4\x00\x68(.{4})"
        rb"\x8b\xcf\xc7\x07.{4}\xff\xd0\xc7\x07(.{4})\xc7\x47\x2c(.{4})"
        rb"\xc6\x47\x30\x00\xc7\x47\x14\x03\x00\x00\x00", re.DOTALL)
    records = constructor.findall(backed(callback, 0xb0, True))
    if len(records) != 1:
        raise ValueError("Could not resolve a unique owned integer declaration")
    class_name, function_name, vtable, entry = (struct.unpack("<I", item)[0] for item in records[0])
    if backed(class_name, 12, False) != b"PapyrusUtil\0" or backed(function_name, 11, False) != b"GetVersion\0":
        raise ValueError("Unexpected class or function in the first owned registration")
    run = struct.unpack("<I", backed(vtable + 22 * 4, 4, False))[0]
    method = backed(run, 0x50, True)
    if not method.endswith(b"\xc2\x14\x00") or method.count(b"\xb8\x20\xa6\xc3\x00") != 1 or \
       method.count(b"\xb8\xe0\x28\xc3\x00") != 1:
        raise ValueError("Unexpected owned IFunction call shape")
    return base, (entry - base, backed(entry, 6, True)), (run - base, method)


def owned_register_integer_leaf(library, callback):
    base, (_, code), _ = owned_register_integer_method(library, callback)
    return base, code


def prepare_owned_json_inputs(archive_path, executable_path, root):
    dll, words = root / "StorageUtil.dll", root / "boot.bin"
    scripts = root / "scripts"
    with zipfile.ZipFile(archive_path) as archive:
        member = archive.getinfo("SKSE/Plugins/StorageUtil.dll")
        if member.file_size != 978944:
            raise ValueError("Unexpected owned PapyrusUtil archive size")
        library = archive.read(member)
        scripts.mkdir()
        for name in ("ActorUtil", "JsonUtil", "MiscUtil", "ObjectUtil", "PapyrusUtil", "StorageUtil"):
            filename = name + ".pex"
            member = archive.getinfo(f"scripts/{filename}")
            if not 0 < member.file_size <= 8 << 20:
                raise ValueError("Unexpected owned PapyrusUtil PEX size")
            (scripts / filename).write_bytes(archive.read(member))
    # The controlled fixture verifies the exact owner inputs in both images.
    if hashlib.sha256(library).hexdigest() != "51e40d27bc635664d747a781179673d9cbda8c22d176f56fa6598fcce4a0b5e9":
        raise ValueError("The owned PapyrusUtil DLL does not match the reviewed build")
    dll.write_bytes(library)
    words.write_bytes(owner_bootstrap_words(Path(executable_path)))
    return dll, words, scripts


def check_owned_pex_contract(test, binary, dll, words, store, scripts):
    result = subprocess.run([binary, dll, words, store, "contract", scripts], capture_output=True, text=True, timeout=10)
    test.assertEqual(result.returncode, 0, result.stderr)
    test.assertEqual(result.stderr, "", result.stdout)
    test.assertEqual(result.stdout.splitlines(), [
        "pex mismatch=1 MiscUtil.ScanCellNPCsByFaction",
        "dll only MiscUtil.TESTScanCellNPCsByFaction",
        "native signatures matched=394 dll_declarations=395",
    ])


def check_owned_settings_sequence(test, binary, dll, bootstrap, root, scripts, executable):
    game, store = root / "game", root / "settings"
    game.mkdir()
    store.mkdir(mode=0o700)
    owner_digest = hashlib.sha256(Path(executable).read_bytes()).hexdigest()
    base_file = game / "Data/SKSE/Plugins/WearableLanternsData/profile1.json"
    base_bytes = b'{"int":{"brightness":208}}'
    for phase in ("write", "records", "startup-list", "read", "keyword", "base"):
        if phase == "base":
            test.assertEqual(list(game.iterdir()), [])
            base_file.parent.mkdir(parents=True)
            base_file.write_bytes(base_bytes)
        extra = [scripts / "StorageUtil.pex"] if phase == "keyword" else []
        result = subprocess.run([binary, dll, scripts / "JsonUtil.pex", bootstrap, game, store,
                                 phase, executable, owner_digest, *extra],
                                capture_output=True, text=True, timeout=30)
        test.assertEqual(result.returncode, 0, result.stderr)
    test.assertEqual(base_file.read_bytes(), base_bytes)
    paths = list(store.rglob("profile1.json"))
    test.assertEqual(len(paths), 1)
    test.assertEqual(json.loads(paths[0].read_text(encoding="utf-8")), {
        "int": {"brightness": 39}, "float": {"duration": 1.625}, "string": {"profile_name": "ランタンの明かり"},
    })
    support = root / "selection"
    view = support / "Views/10000000-0000-4000-8000-000000000000"
    package = support / "Mods/20000000-0000-4000-8000-000000000000"
    for directory in (view, package):
        for src, relative in ((dll, "Data/SKSE/Plugins/StorageUtil.dll"), (scripts / "JsonUtil.pex", "Data/Scripts/JsonUtil.pex")):
            destination = directory / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, destination)
    common = view / "Data/SKSE/Plugins/WearableLanternsData/common.json"
    common.parent.mkdir()
    common_bytes = b'{"int":{"auto_load":2}}'
    common.write_bytes(common_bytes)
    acquired = subprocess.run([binary, dll, scripts / "JsonUtil.pex", bootstrap, support, store,
                               "view", executable, owner_digest], capture_output=True, text=True, timeout=30)
    test.assertEqual(acquired.returncode, 0, acquired.stderr)
    test.assertEqual(common.read_bytes(), common_bytes)
    test.assertEqual(list((support / "PapyrusSettings").iterdir()), [])


def check_owned_json_sequence(test, binary, dll, words, root, scripts):
    private_store = root / "app-private"
    private_store.mkdir(mode=0o700)
    for phase, value in (("write", -731), ("update", -382), ("read", -382), ("isolation", None)):
        with test.subTest(phase=phase):
            result = subprocess.run([binary, dll, words, private_store, phase, scripts], capture_output=True, timeout=10)
            test.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            if value is None:
                test.assertEqual(result.stdout, b"")
            else:
                test.assertEqual(json.loads(result.stdout),
                                 {"intList": {"skyrimios-probe": [0, 0, 0, 0, value, 0, 0]}})
    with test.subTest(phase="restore-real-records-in-fresh-session"):
        companion = root / "AuthoredSlot.skse"
        arguments = [binary, dll, words, private_store]
        write = subprocess.run([*arguments, "write-records", scripts, companion], capture_output=True, timeout=10)
        test.assertEqual(write.returncode, 0, write.stderr.decode(errors="replace"))
        test.assertEqual(write.stdout, b"")
        committed = companion.read_bytes()
        test.assertEqual(committed[:4], b"SKSE")
        restore = subprocess.run([*arguments, "read-records", scripts, companion], capture_output=True, timeout=10)
        test.assertEqual(restore.returncode, 0, restore.stderr.decode(errors="replace"))
        test.assertEqual(restore.stdout, b"")
        test.assertEqual(companion.read_bytes(), committed)
        wrong_owner = committed[:20] + struct.pack("<I", 1) + committed[24:]
        for name, contents in (("truncated", committed[:-1]), ("other-owner", wrong_owner)):
            altered = root / (name + ".skse")
            altered.write_bytes(contents)
            refused = subprocess.run([*arguments, "read-records", scripts, altered], capture_output=True, timeout=10)
            test.assertEqual(refused.returncode, 2, refused.stderr.decode(errors="replace"))
            test.assertEqual(refused.stdout, b"")
            test.assertEqual(altered.read_bytes(), contents)
        test.assertEqual(companion.read_bytes(), committed)
    lantern_store = root / "app-lanterns"
    lantern_store.mkdir(mode=0o700)
    for phase in ("write-profile", "read-profile"):
        with test.subTest(phase=phase):
            result = subprocess.run([binary, dll, words, lantern_store, phase, scripts], capture_output=True, timeout=10)
            test.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            test.assertEqual(json.loads(result.stdout),
                             {"int": {"brightness": 3}, "float": {"hold_activate_toggle_duration": 1.625},
                              "string": {"profile_name": "ランタンの明かり"}})
    paths = list(lantern_store.rglob("profile1.json"))
    common_paths = list(lantern_store.rglob("common.json"))
    test.assertEqual(len(paths), 1)
    test.assertEqual(len(common_paths), 1)
    target, common = paths[0], common_paths[0]
    common_source = common.read_bytes()
    test.assertEqual(json.loads(common_source), {"int": {"auto_load": 2, "last_profile": 1}})
    # Authored profile at the size of a real MCM configuration. These
    # inputs exercise the original DLL's decimal parsing, not only 1.625.
    contents = json.loads(target.read_bytes())
    for i in range(22):
        contents["int"][f"synthetic_integer_{i}"] = i + 1
    values = (0.01, -0.00004, 19.875, 289.50, 123.88, 40.1001, 2.1, 120, 10.3, 720.15)
    for i, value in enumerate(values):
        contents["float"][f"synthetic_float_{i}"] = struct.unpack("<f", struct.pack("<f", value))[0]
    source = json.dumps(contents, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    target.write_bytes(source)
    with test.subTest(phase="prepare-readable-profile"):
        result = subprocess.run([binary, dll, words, lantern_store, "prepare-profile", scripts], capture_output=True, timeout=10)
        test.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        test.assertEqual(json.loads(result.stdout), contents)
        test.assertEqual(target.read_bytes(), source)
        test.assertEqual(common.read_bytes(), common_source)
    with test.subTest(phase="honor-disabled-profile-autoload"):
        disabled = b'{"int":{"auto_load":1,"last_profile":1}}'
        common.write_bytes(disabled)
        result = subprocess.run([binary, dll, words, lantern_store, "prepare-disabled", scripts],
                                capture_output=True, timeout=10)
        test.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        test.assertEqual(result.stdout, disabled)
        test.assertEqual(common.read_bytes(), disabled)
        test.assertEqual(target.read_bytes(), source)
    with test.subTest(phase="reject-wrong-selected-pex"):
        wrong_scripts = root / "wrong-scripts"
        wrong_scripts.mkdir()
        shutil.copyfile(scripts / "StorageUtil.pex", wrong_scripts / "JsonUtil.pex")
        before = {path: path.read_bytes() for path in lantern_store.rglob("*") if path.is_file()}
        result = subprocess.run([binary, dll, words, lantern_store, "write-profile", wrong_scripts],
                                capture_output=True, timeout=10)
        test.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
        test.assertEqual(result.stdout, b"")
        test.assertIn(b"Selected JsonUtil PEX does not match the captured native methods.", result.stderr)
        after = {path: path.read_bytes() for path in lantern_store.rglob("*") if path.is_file()}
        test.assertEqual(after, before)


@unittest.skipIf(sys.platform == "win32", "the selected-file wrapper uses Darwin/POSIX descriptors")
class LocalFileProbeTest(unittest.TestCase):
    def test_regular_file_boundaries_symlinks_and_dll_results(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required")
        with tempfile.TemporaryDirectory(prefix="dll-local-file-") as temporary:
            # Darwin's system temporary directory may begin with /var, which
            # is a symlink. Resolve the test directory before creating the
            # separate symlink case that the actual reader must reject.
            root = Path(temporary).resolve()
            binary = root / "probe"
            compile_local_probe(cc, binary)
            good, unknown, serialized = root / "検査.dll", root / "unknown.dll", root / "serialized.dll"
            fixture = supported_fixture()
            good.write_bytes(fixture)
            with_import = supported_fixture()
            add_named_clock(with_import)
            zero_string(with_import, DATA + 0xa2, b"OwnedUnsupported")
            unknown.write_bytes(with_import)
            uncalled_path, called_path, mixed_path = (root / name for name in ("uncalled.dll", "called.dll", "mixed.dll"))
            uncalled = with_import.copy()
            zero_string(uncalled, DATA + 0xa2, b"RtlUnwind")
            uncalled_path.write_bytes(uncalled)
            called = uncalled.copy()
            code = bytes.fromhex("ff 15") + struct.pack("<I", BASE + 0x3060) + bytes.fromhex("b8 01 00 00 00 c3")
            called[0x200:0x200 + len(code)] = code
            called_path.write_bytes(called)
            shell_path, wrong_library_path = root / "shell.dll", root / "wrong-library.dll"
            shell = called.copy()
            zero_string(shell, DATA + 0x40, b"shell32.DLL")
            zero_string(shell, DATA + 0xa2, b"SHGetFolderPathA")
            shell_path.write_bytes(shell)
            wrong_library = shell.copy()
            zero_string(wrong_library, DATA + 0x40, b"kernel32.DLL")
            wrong_library_path.write_bytes(wrong_library)
            mixed = uncalled.copy()
            directory(mixed, 12, 0x3060, 12)
            for table in (0x60, 0x80):
                put(mixed, DATA + table, "3I", 0x30a0, 0x30c0, 0)
            zero_string(mixed, DATA + 0xc2, b"OwnedUnsupported")
            mixed_path.write_bytes(mixed)
            serialized.write_bytes(serializing_fixture())
            worker, closed, bounded = root / "worker.dll", root / "closed.dll", root / "bounded.dll"
            worker.write_bytes(worker_fixture())
            closed.write_bytes(worker_fixture(close_before_join=True))
            bounded.write_bytes(worker_fixture(worker_runs_forever=True))
            tasks, bad_task = root / "tasks.dll", root / "invalid-task.dll"
            tasks.write_bytes(task_fixture())
            bad_task.write_bytes(task_fixture(bad_vtable=True))
            invalid, empty, big = root / "invalid.dll", root / "empty.dll", root / "large.dll"
            invalid.write_bytes(b"not a PE file")
            empty.touch()
            with big.open("wb") as file:
                file.truncate(64 * 1024 * 1024 + 1)
            link, fifo = root / "link.dll", root / "fifo.dll"
            link.symlink_to(good)
            os.mkfifo(fifo, mode=0o600)
            result = subprocess.check_output([binary, good, unknown, uncalled_path, called_path, mixed_path, serialized,
                                              worker, closed, bounded, tasks, bad_task, invalid, empty, big,
                                              link, fifo, root / "absent.dll", root],
                                             text=True, timeout=3).splitlines()
            self.assertEqual(result, ["ready,7,Owned test plugin",
                                      "stopped,1,import absent from the service list,1,kernel32.DLL,OwnedUnsupported",
                                      "ready,7,Owned test plugin,partial",
                                      "stopped,8,ok,0,load=6,partial,RtlUnwind",
                                      "stopped,1,import absent from the service list,2,kernel32.DLL,OwnedUnsupported",
                                      "ready,7,Owned test plugin,2,1",
                                      "ready,7,Owned test plugin,workers=1",
                                      "ready,7,Owned test plugin,workers=1",
                                      "stopped,8,ok,0,load=4",
                                      "ready,7,Owned test plugin,tasks=3/1",
                                      "stopped,20,ok,0,task=2",
                                      "stopped,1,truncated headers,0", "2", "3", "1", "2", "1", "2"])
            self.assertEqual(good.read_bytes(), fixture)
            self.assertEqual(unknown.read_bytes(), with_import)
            result = subprocess.check_output([binary, shell_path, wrong_library_path], text=True, timeout=3).splitlines()
            self.assertEqual(result, ["stopped,8,ok,0,load=6,partial,SHGetFolderPathA",
                                      "stopped,1,import absent from the service list,1,kernel32.DLL,SHGetFolderPathA"])
            for name in ("GetStartupInfoA", "GetStartupInfoW"):
                with self.subTest(startup=name):
                    startup = with_import.copy()
                    zero_string(startup, DATA + 0xa2, name.encode("ascii"))
                    startup[DATA + 0x100:DATA + 0x148] = b"\xa5" * 0x48
                    output = BASE + 0x3100
                    code = b"\x68" + struct.pack("<I", output)
                    code += b"\xff\x15" + struct.pack("<I", BASE + 0x3060)
                    for address, value in ((output, 68), (output + 52, 0),
                                           (output + 56, 0xa5a5a5a5), (output + 68, 0xa5a5a5a5)):
                        # Compare the written size and cleared pointer, then check
                        # that the unused handles and adjacent storage are intact.
                        code += b"\x81\x3d" + struct.pack("<II", address, value) + b"\x74\x02\x0f\x0b"
                    code += b"\xb8\x01\0\0\0\xc3"
                    put(startup, SECTIONS + 8, "I", 0x200)
                    put(startup, RD + 0x60, "I", 0x1080)
                    startup[0x280:0x280 + len(code)] = code
                    path = root / f"{name}.dll"
                    path.write_bytes(startup)
                    self.assertEqual(subprocess.check_output([binary, path], text=True, timeout=3).strip(),
                                     "ready,7,Owned test plugin")
            resident = subprocess.check_output([binary, "--session", tasks], text=True, timeout=3)
            self.assertEqual(resident.strip(), "resident tasks=3/1 independent-lifetime=ok budget-refusal=ok")
            resident_worker = subprocess.check_output([binary, "--session-worker", closed], text=True, timeout=3)
            self.assertEqual(resident_worker.strip(), "resident worker completed once on the explicit step")

    def test_owned_settings_backend_stages_and_reopens_profile(self):
        archive = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        executable = os.environ.get("SKYRIM_TEST_OWNED_LE_EXE")
        if not archive or not executable:
            self.skipTest("Optional locally owned LE executable and PapyrusUtil 3.3 ZIP required")
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required")
        with tempfile.TemporaryDirectory(prefix="skse-classic-settings-") as temporary:
            root = Path(temporary)
            dll, bootstrap, scripts = prepare_owned_json_inputs(archive, executable, root)
            binary = root / "settings-fixture"
            compile_local_probe(cc, binary, settings_backend=True)
            check_owned_settings_sequence(self, binary, dll, bootstrap, root, scripts, executable)

    def test_owned_papyrus_enters_classic_load_with_pinned_data(self):
        archive_path = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        executable_path = os.environ.get("SKYRIM_TEST_OWNED_LE_EXE")
        if not archive_path or not executable_path:
            self.skipTest("Optional locally owned LE executable and PapyrusUtil 3.3 ZIP required")
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required")
        with tempfile.TemporaryDirectory(prefix="owned-papyrus-local-probe-") as temporary:
            root = Path(temporary)
            dll, words, binary = root / "StorageUtil.dll", root / "boot.bin", root / "probe"
            code_view = Path(executable_path)
            words.write_bytes(owner_bootstrap_words(code_view))
            with zipfile.ZipFile(archive_path) as archive:
                members = [entry for entry in archive.infolist() if entry.filename == "SKSE/Plugins/StorageUtil.dll"]
                self.assertEqual(len(members), 1)
                self.assertGreater(members[0].file_size, 0)
                self.assertLessEqual(members[0].file_size, 64 * 1024 * 1024)
                owned_library = archive.read(members[0])
                dll.write_bytes(owned_library)
            compile_local_probe(cc, binary)
            for mode, expected_hooks in (("--owned-papyrus", 0), ("--observe-papyrus-hooks", 6)):
                result = subprocess.run([binary, mode, dll, words], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                lines = result.stdout.splitlines()
                self.assertEqual(sum(line.startswith("attach status=0 ") for line in lines), 2)
                self.assertEqual(sum(line.startswith("observed hook ") for line in lines), expected_hooks)
                records = [dict(part.split("=") for part in line.split()) for line in lines if line.startswith("snapshot=")]
                self.assertEqual([int(record["registered"]) for record in records], [1, 1])
                callback, moved_callback = (int(record["callback"], 16) for record in records)
                base, routine = owned_register_integer_leaf(owned_library, callback)
                difference = 0x12000000 - base
                self.assertEqual(moved_callback - callback, difference)
                if expected_hooks:
                    observations = [dict(part.split("=") for part in line.split()[2:])
                                    for line in lines if line.startswith("observed hook ")]
                    for preferred, moved in zip(observations[:3], observations[3:], strict=True):
                        self.assertEqual(preferred["site"], moved["site"])
                        self.assertEqual(int(moved["target"], 16) - int(preferred["target"], 16), difference)
                print(result.stdout, end="")
            code_before = hashlib.sha256(code_view.read_bytes()).digest()
            audited = subprocess.run([binary, "--audit-papyrus", dll, words, code_view],
                                     capture_output=True, text=True, timeout=10)
            self.assertEqual(audited.returncode, 0, audited.stdout + audited.stderr)
            self.assertIn("observed 6 scalar requests across 3 known sites", audited.stdout)
            self.assertEqual(hashlib.sha256(code_view.read_bytes()).digest(), code_before)
            leaf = root / "owned-int-leaf.bin"
            leaf.write_bytes(routine)
            result = subprocess.check_output([binary, "--owned-int-leaf", leaf], text=True, timeout=10)
            self.assertEqual(result.splitlines(), ["GetVersion leaf=33", "GetVersion leaf=33"])
            print(result, end="")
            result = subprocess.run([binary, "--owned-cosave", dll, words], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.strip(),
                             "Owned StorageUtil restored a signed integer and wrote the current authored mod order.")
            print(result.stdout, end="")

    def test_owned_json_settings_round_trip_in_relocated_session(self):
        archive_path = os.environ.get("SKYRIM_TEST_OWNED_PAPYRUS_ZIP")
        executable_path = os.environ.get("SKYRIM_TEST_OWNED_LE_EXE")
        if not archive_path or not executable_path:
            self.skipTest("Optional locally owned LE executable and PapyrusUtil 3.3 ZIP required")
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required")
        with tempfile.TemporaryDirectory(prefix="owned-papyrus-json-") as temporary:
            root = Path(temporary)
            binary = root / "json-trial"
            dll, words, scripts = prepare_owned_json_inputs(archive_path, executable_path, root)
            compile_local_probe(cc, binary, json_trial=True)
            check_owned_json_sequence(self, binary, dll, words, root, scripts)
            check_owned_pex_contract(self, binary, dll, words, root / "app-private", scripts)


if __name__ == "__main__":
    unittest.main()
