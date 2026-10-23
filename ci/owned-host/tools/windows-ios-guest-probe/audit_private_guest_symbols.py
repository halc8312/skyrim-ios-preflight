"""Local, fail-closed symbol closure audit for a completed private object batch.

No demangling, linking, SDK, network, source-body output, or input mutation.
Raw symbol names and object ownership stay in a new local JSON. Stdout contains
only counts, classifications and explicitly limited readiness booleans.

Mach-O weak definitions/references are distinguished using LLVM nm -x n_desc,
not POSIX T/U letters (which lose that distinction for Mach-O). Bit semantics:
public Darwin mach-o/nlist.h and llvm/tools/llvm-nm/llvm-nm.cpp.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest


LAB = Path(__file__).resolve().parents[2]
LLVM = Path(r"C:\Program Files\LLVM\bin")
TARGET = "--target=arm64-apple-ios26.0"
HASH = re.compile(r"[0-9a-f]{64}\Z")
NM_LINE = re.compile(r"^([0-9A-Fa-f]{16}) ([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{4}) ([0-9A-Fa-f]{8}) (\S+)$")


class AuditError(Exception):
    def __init__(self, classification: str):
        self.classification = classification


def require(condition, classification):
    if not condition:
        raise AuditError(classification)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate_json_key")
        result[key] = value
    return result


def macho_metadata(path):
    """Validate bounded MH_OBJECT/iOS commands and fingerprint the same file."""
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        header = stream.read(32)
        require(len(header) == 32, "truncated_macho_header")
        magic, cpu, subtype, kind, count, command_bytes, flags, reserved = struct.unpack("<8I", header)
        require((magic, cpu, subtype & 0xFFFFFF, kind) == (0xFEEDFACF, 0x100000C, 0, 1), "not_arm64_macho_object")
        require(count <= 65536 and 0 < command_bytes <= min(1024 * 1024, before.st_size - 32), "invalid_macho_commands")
        commands = stream.read(command_bytes)
        at, builds, symtabs, sections = 0, [], [], 0
        for _ in range(count):
            require(at + 8 <= len(commands), "truncated_macho_command")
            command, size = struct.unpack_from("<II", commands, at)
            require(size >= 8 and size % 8 == 0 and at + size <= len(commands), "invalid_macho_command")
            if command == 0x32:
                require(size >= 24, "invalid_build_version_command")
                platform, minimum, sdk, tools = struct.unpack_from("<4I", commands, at + 8)
                require(24 + tools * 8 == size, "invalid_build_version_tools")
                builds.append((platform, minimum))
            elif command == 0x2:
                require(size == 24, "invalid_symbol_table_command")
                symtabs.append(struct.unpack_from("<4I", commands, at + 8))
            elif command == 0x19:
                require(size >= 72, "invalid_segment_command")
                section_count = struct.unpack_from("<I", commands, at + 64)[0]
                require(size == 72 + 80 * section_count, "invalid_segment_sections")
                sections += section_count
            require(command != 0x1D, "unexpected_code_signature")
            at += size
        require(at == len(commands), "extra_macho_command_bytes")
        require(builds == [(2, 0x1A0000)], "not_ios26_object")
        require(len(symtabs) == 1, "missing_or_duplicate_symbol_table")
        symoff, nsyms, stroff, strsize = symtabs[0]
        require(32 + command_bytes <= symoff <= before.st_size and nsyms <= (before.st_size - symoff) // 16, "symbol_table_out_of_bounds")
        require(32 + command_bytes <= stroff <= before.st_size and strsize <= before.st_size - stroff, "string_table_out_of_bounds")
        require(symoff + nsyms * 16 <= stroff or stroff + strsize <= symoff, "overlapping_symbol_and_string_tables")
        stream.seek(0)
        fingerprint = hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
        require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns), "object_changed_during_hash")
    return {"format": "Mach-O", "architecture": "arm64", "type": "MH_OBJECT", "platform": "iOS", "minimum": "26.0", "sdk_verified": False, "bytes": before.st_size, "sha256": fingerprint, "symbol_table_entries": nsyms, "section_count": sections}


def load_index(path):
    path = path.resolve(strict=True)
    index_hash = digest(path)
    data = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)
    require(data.get("schema_version") == 1 and data.get("local_only") is True, "unsupported_batch_index")
    require(Path(data["output_dir"]).resolve(strict=True) == path.parent, "index_output_directory_mismatch")
    require(data.get("changed_inputs") == [], "batch_input_changed")
    units = data.get("translation_units")
    require(isinstance(units, list) and units, "empty_or_incomplete_batch")
    summary = data.get("summary", {})
    require(summary.get("total") == len(units) and summary.get("succeeded") == len(units) and summary.get("failed") == 0 and summary.get("remaining") == 0, "batch_not_all_successful")
    flags = data.get("compiler_flags", [])
    require(isinstance(flags, list) and flags.count(TARGET) == 1 and not any(f == "-target" or f == "--target" or f.startswith("--target=") and f != TARGET for f in flags), "batch_target_mismatch")
    inputs = data.get("input_sha256", {})
    expected_sources = {name for name in inputs if name.endswith(".cpp")}
    object_directory = (path.parent / "objects").resolve(strict=True)
    require(object_directory.parent == path.parent, "object_directory_outside_batch")
    names, objects, records = set(), set(), []
    for item in units:
        require(item.get("success") is True and item.get("compiler_exit") == 0, "unsuccessful_translation_unit")
        source = item.get("source")
        require(isinstance(source, str) and Path(source).name == source and source.endswith(".cpp"), "invalid_source_name")
        require(source.casefold() not in names, "duplicate_source_name")
        names.add(source.casefold())
        require(HASH.fullmatch(str(item.get("source_sha256", ""))) and inputs.get(source) == item["source_sha256"], "source_fingerprint_mismatch")
        relative = Path(item["object"])
        require(not relative.is_absolute() and len(relative.parts) == 2 and relative.parts[0] == "objects" and relative.name == Path(source).stem + ".o", "invalid_object_path")
        obj = (path.parent / relative).resolve(strict=True)
        require(obj.parent == object_directory and obj.is_file(), "object_outside_batch")
        require(str(obj).casefold() not in objects, "duplicate_object_path")
        objects.add(str(obj).casefold())
        expected = item.get("object_metadata", {})
        require(HASH.fullmatch(str(expected.get("sha256", ""))), "missing_object_fingerprint")
        records.append({"source": source, "path": obj, "relative": str(relative), "expected": expected})
    require({item["source"] for item in units} == expected_sources, "translation_unit_coverage_mismatch")
    actual_objects = {str(p.resolve(strict=True)).casefold() for p in (path.parent / "objects").glob("*.o")}
    require(actual_objects == objects, "object_directory_coverage_mismatch")
    return path, index_hash, data, records


def parse_nm(text, metadata):
    symbols, names = [], set()
    for line in text.splitlines():
        if not line.strip():
            continue
        match = NM_LINE.fullmatch(line)
        require(match is not None, "unexpected_llvm_nm_output")
        value, ntype, section, desc, strx = (int(part, 16) for part in match.groups()[:5])
        name = match[6]
        require(name not in names, "duplicate_external_name_in_object")
        names.add(name)
        require(ntype & 1 and not ntype & 0xE0, "nm_returned_nonexternal_symbol")
        kind = ntype & 0xE
        require(kind in (0, 2, 0xE), "unsupported_external_symbol_type")
        require(kind != 0xE or 1 <= section <= metadata["section_count"], "symbol_section_out_of_bounds")
        require(kind == 0xE or section == 0, "unexpected_symbol_section")
        # Common tentative definitions need a real linker policy; never silently
        # count them as strong definitions or satisfy a guest reference with one.
        common = kind == 0 and value != 0
        defined = kind != 0
        weak = bool(desc & (0x80 if defined else 0x40))
        symbols.append({"raw": name, "defined": defined, "weak": weak, "common": common, "private_external": bool(ntype & 0x10), "references_weak_definition": bool(not defined and desc & 0x80), "n_type": ntype, "n_desc": desc, "section": section, "value": value})
    require(len(symbols) <= metadata["symbol_table_entries"], "nm_symbol_count_exceeds_table")
    return symbols


def inspect_one(record, nm, timeout):
    obj = record["path"]
    metadata = macho_metadata(obj)
    for key in ("format", "architecture", "type", "platform", "minimum", "bytes", "sha256"):
        require(metadata[key] == record["expected"].get(key), "object_metadata_or_hash_mismatch")
    result = subprocess.run([str(nm), "--extern-only", "--no-demangle", "--no-llvm-bc", "-x", str(obj)], capture_output=True, timeout=timeout, check=False)
    require(result.returncode == 0 and not result.stderr.strip(), "llvm_nm_failed_or_warned")
    symbols = parse_nm(result.stdout.decode("utf-8", errors="strict"), metadata)
    require(digest(obj) == metadata["sha256"], "object_changed_during_nm")
    return {"object": record["relative"], "source": record["source"], "metadata": metadata, "external_symbols": symbols}


# Classification is informational. It never removes an unresolved symbol.
GUEST = re.compile(r"^_(?:__imp__)?(?:sub_[0-9A-Fa-f]{8}|__(?:save|rest)[A-Za-z0-9_]+|_?xstart)$|^__Z\d+(?:__imp__)?(?:sub_[0-9A-Fa-f]{8}|__(?:save|rest)[A-Za-z0-9_]+|_?xstart)(?:R|P|v)")
HLE = re.compile(r"^_(?:__imp__)?(?:Ke|Nt|Zw|Ex|Rtl|Xam|Xapi|XAudio|XMA|Xex|Xc|Xe|XGet|XSet|XMsg|Ob|Mm|Hal|Dbg|D3D|Vd|Hid|NetDll|WSA|socket|connect|send|recv)|^__Z\d+(?:__imp__)?(?:Ke|Nt|Zw|Ex|Rtl|Xam|Xapi|XAudio|XMA|Xex|Xc|Xe|XGet|XSet|XMsg|Ob|Mm|Hal|Dbg|D3D|Vd|Hid|NetDll|WSA)")
SYSTEM = {"_memcpy", "_memmove", "_memset", "_memcmp", "_bzero", "_bcmp", "_malloc", "_calloc", "_realloc", "_free", "_abort", "_setjmp", "_longjmp", "__setjmp", "__longjmp", "___stack_chk_guard", "___stack_chk_fail", "_exp2f", "_log2f", "_sqrt", "_sqrtf", "_floor", "_floorf", "_ceil", "_ceilf", "_trunc", "_truncf", "_round", "_roundf", "_fmod", "_fmodf", "_pow", "_powf", "_exp", "_expf", "_log", "_logf", "_log2", "_exp2", "_sin", "_sinf", "_cos", "_cosf", "_tan", "_tanf", "_atan", "_atanf", "_atan2", "_atan2f", "_acos", "_acosf", "_asin", "_asinf"}


def classify(raw):
    if GUEST.search(raw):
        return "internal_guest"
    if raw.startswith("_skyrt_"):
        return "host_runtime_bridge"
    if HLE.search(raw):
        return "hle_import_candidate"
    if raw in SYSTEM or raw.startswith(("___cxa_", "___gxx_", "__Unwind_", "__ZSt", "__ZNSt", "__ZTI", "__ZTV", "__Zdl", "__Zda", "__Znwm", "__Znam")):
        return "system_or_cxx_runtime_candidate"
    return "unclassified_external"


def closure(objects):
    definitions, references, commons = defaultdict(list), defaultdict(list), []
    for obj in objects:
        for symbol in obj["external_symbols"]:
            owner = {"object": obj["object"], "weak": symbol["weak"], "private_external": symbol["private_external"]}
            if symbol["common"]:
                commons.append({"raw": symbol["raw"], **owner})
            elif symbol["defined"]:
                definitions[symbol["raw"]].append(owner)
            else:
                references[symbol["raw"]].append(owner)
    duplicates = [{"raw": raw, "owners": owners} for raw, owners in sorted(definitions.items()) if sum(not item["weak"] for item in owners) > 1]
    weak_multiple = [{"raw": raw, "owners": owners} for raw, owners in sorted(definitions.items()) if len(owners) > 1 and sum(not item["weak"] for item in owners) <= 1]
    unresolved = [{"raw": raw, "classification": classify(raw), "references": owners, "all_references_weak": all(item["weak"] for item in owners)} for raw, owners in sorted(references.items()) if raw not in definitions]
    resolved = [{"raw": raw, "definitions": definitions[raw], "references": owners} for raw, owners in sorted(references.items()) if raw in definitions]
    categories = dict(Counter(item["classification"] for item in unresolved))
    internal = categories.get("internal_guest", 0)
    structural = not duplicates and not commons
    # An unclassified external could be another internal guest spelling. Do not
    # claim guest closure merely because the known-name regex did not match it.
    guest_closed = structural and internal == 0 and not categories.get("unclassified_external", 0)
    return {"definitions": dict(sorted(definitions.items())), "resolved_cross_object_references": resolved, "strong_duplicate_definitions": duplicates, "allowed_weak_or_strong_plus_weak_multiples": weak_multiple, "unsupported_common_symbols": commons, "unresolved_symbols": unresolved, "summary": {"objects": len(objects), "defined_external_names": len(definitions), "referenced_external_names": len(references), "resolved_external_names": len(resolved), "strong_duplicate_names": len(duplicates), "allowed_weak_multiple_names": len(weak_multiple), "unsupported_common_symbols": len(commons), "unresolved_external_names": len(unresolved), "unresolved_classifications": categories, "internal_guest_reference_closure": guest_closed, "full_link_ready": guest_closed and not unresolved, "link_performed": False, "runtime_compatibility_verified": False}}


def audit(index, nm, jobs=2, timeout=60):
    index_path, index_hash, data, records = load_index(index)
    nm = nm.resolve(strict=True)
    version = subprocess.run([str(nm), "--version"], capture_output=True, timeout=30, check=False)
    require(version.returncode == 0 and not version.stderr.strip(), "llvm_nm_version_failed")
    nm_hash = digest(nm)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda item: inspect_one(item, nm, timeout), records))
    require(digest(index_path) == index_hash, "batch_index_changed_during_audit")
    require(digest(nm) == nm_hash, "llvm_nm_changed_during_audit")
    combined = closure(results)
    return {"schema_version": 1, "local_only": True, "operation": "arm64-ios-private-object-external-symbol-closure", "batch_index": str(index_path), "batch_index_sha256": index_hash, "llvm_nm": str(nm), "llvm_nm_sha256": nm_hash, "llvm_nm_version": version.stdout.decode("utf-8", errors="strict").strip(), "llvm_nm_arguments": ["--extern-only", "--no-demangle", "--no-llvm-bc", "-x"], "target": "arm64-apple-ios26.0", "jobs": jobs, "objects": results, **combined, "notes": ["All symbol keys remain raw mangled Mach-O names; no demangling or fuzzy resolution is used.", "Classification names are hints, not proof that a host provider exists or has compatible ABI.", "Every unresolved symbol, including weak imports, prevents full_link_ready.", "Weak duplicate definitions and one strong plus weak definitions are permitted; common and indirect symbols fail closed.", "Object-level symbol closure does not prove a link: no archive extraction, relocation, SDK, ABI, signing, loading, or execution is tested."]}


def save_report(report, output):
    require(output.parent.resolve(strict=True).is_dir(), "output_parent_unavailable")
    require(not output.exists(), "output_already_exists")
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


class SyntheticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = LAB / "build" / "windows-ios-guest-probe"
        parent.mkdir(parents=True, exist_ok=True)
        cls.root = Path(tempfile.mkdtemp(prefix="symbol-audit-public-", dir=parent)).resolve()
        require(cls.root.is_relative_to(parent.resolve()), "synthetic_path_escape")
        cls.nm, cls.clang = LLVM / "llvm-nm.exe", LLVM / "clang++.exe"

    def fixture(self, sources):
        directory = Path(tempfile.mkdtemp(prefix="fixture-", dir=self.root))
        (directory / "objects").mkdir()
        units, fingerprints = [], {}
        for number, source in enumerate(sources):
            path, obj = directory / f"public{number}.cpp", directory / "objects" / f"public{number}.o"
            path.write_text(source, encoding="utf-8")
            result = subprocess.run([str(self.clang), TARGET, "-O0", "-c", str(path), "-o", str(obj)], capture_output=True, timeout=30, check=False)
            self.assertEqual(result.returncode, 0, "public fixture compilation failed")
            fingerprint = digest(path)
            fingerprints[path.name] = fingerprint
            units.append({"source": path.name, "source_sha256": fingerprint, "success": True, "compiler_exit": 0, "object": str(obj.relative_to(directory)), "object_metadata": macho_metadata(obj)})
        index = directory / "index.json"
        index.write_text(json.dumps({"schema_version": 1, "local_only": True, "output_dir": str(directory), "changed_inputs": [], "compiler_flags": [TARGET], "input_sha256": fingerprints, "translation_units": units, "summary": {"total": len(units), "succeeded": len(units), "failed": 0, "remaining": 0}}), encoding="utf-8")
        return index

    def test_cross_object_resolution_and_raw_mangling(self):
        index = self.fixture(['extern "C" int sub_82000000(); extern "C" int entry(){return sub_82000000();} int raw_mangled(int v){return v;}', 'extern "C" int sub_82000000(){return 3;}'])
        result = audit(index, self.nm)
        self.assertTrue(result["summary"]["internal_guest_reference_closure"])
        self.assertTrue(result["summary"]["full_link_ready"])
        self.assertEqual(result["summary"]["resolved_external_names"], 1)
        self.assertTrue(any(name.startswith("__Z") for name in result["definitions"]))
        self.assertFalse(result["summary"]["link_performed"])

    def test_multiple_weak_definitions_allowed(self):
        index = self.fixture(['extern "C" __attribute__((weak)) int shared_weak(){return 1;} extern "C" int first(){return shared_weak();}', 'extern "C" __attribute__((weak)) int shared_weak(){return 2;} extern "C" int second(){return shared_weak();}'])
        result = audit(index, self.nm)
        self.assertEqual(result["summary"]["strong_duplicate_names"], 0)
        self.assertEqual(result["summary"]["allowed_weak_multiple_names"], 1)
        self.assertTrue(result["summary"]["full_link_ready"])

    def test_strong_duplicates_rejected(self):
        index = self.fixture(['extern "C" int duplicate(){return 1;}', 'extern "C" int duplicate(){return 2;}'])
        result = audit(index, self.nm)
        self.assertEqual(result["summary"]["strong_duplicate_names"], 1)
        self.assertFalse(result["summary"]["full_link_ready"])
        self.assertFalse(result["summary"]["internal_guest_reference_closure"])

    def test_unresolved_classification_and_weak_import_stay_unresolved(self):
        index = self.fixture(['extern "C" int sub_82000000(); extern "C" int skyrt_call(); extern "C" int KeFixture(); extern "C" int exp2f(); extern "C" int unknown_public() __attribute__((weak_import)); extern "C" int entry(){return sub_82000000()+skyrt_call()+KeFixture()+exp2f()+(unknown_public?unknown_public():0);}'])
        result = audit(index, self.nm)
        self.assertEqual(result["summary"]["unresolved_external_names"], 5)
        self.assertEqual(set(result["summary"]["unresolved_classifications"]), {"internal_guest", "host_runtime_bridge", "hle_import_candidate", "system_or_cxx_runtime_candidate", "unclassified_external"})
        self.assertFalse(result["summary"]["full_link_ready"])
        weak = next(s for s in result["unresolved_symbols"] if s["raw"] == "_unknown_public")
        self.assertTrue(weak["all_references_weak"])

    def test_hash_target_coverage_and_incomplete_batch_rejected(self):
        index = self.fixture(['extern "C" int harmless(){return 1;}'])
        original = index.read_text(encoding="utf-8")
        for mutation, category in [(lambda m: m["translation_units"][0]["object_metadata"].update(sha256="0"*64), "object_metadata_or_hash_mismatch"), (lambda m: m.update(compiler_flags=["--target=x86_64-apple-ios26.0-simulator"]), "batch_target_mismatch"), (lambda m: m["summary"].update(remaining=1), "batch_not_all_successful"), (lambda m: m["input_sha256"].update(uncompiled_cpp="0"*64, **{"missing.cpp":"0"*64}), "translation_unit_coverage_mismatch")]:
            data = json.loads(original); mutation(data); index.write_text(json.dumps(data), encoding="utf-8")
            with self.subTest(category=category), self.assertRaises(AuditError) as caught:
                audit(index, self.nm)
            self.assertEqual(caught.exception.classification, category)
        index.write_text(original, encoding="utf-8")

    def test_existing_output_and_bad_symbol_forms_rejected(self):
        out = self.root / "exclusive-output.json"
        save_report({"public_fixture": True}, out)
        before = digest(out)
        with self.assertRaises((AuditError, FileExistsError)):
            save_report({"overwritten": True}, out)
        self.assertEqual(digest(out), before)
        metadata = {"section_count": 1, "symbol_table_entries": 2}
        with self.assertRaises(AuditError):
            parse_nm("0000000000000000 0b 00 0000 00000001 _indirect (indirect for _target)", metadata)
        common = parse_nm("0000000000000004 01 00 0000 00000001 _tentative", metadata)
        result = closure([{"object":"public.o", "external_symbols":common}])
        self.assertFalse(result["summary"]["full_link_ready"])
        self.assertEqual(result["summary"]["unsupported_common_symbols"], 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=Path)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--llvm-nm", type=Path, default=LLVM / "llvm-nm.exe")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SyntheticTests)
        with open(os.devnull, "w") as quiet:
            result = unittest.TextTestRunner(stream=quiet, verbosity=0).run(suite)
        print(json.dumps({"tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors), "public_synthetic_only": True}))
        return 0 if result.wasSuccessful() else 1
    if args.index is None or args.output_json is None:
        parser.error("--index and --output-json are required unless --self-test is used")
    if not 1 <= args.timeout_seconds <= 600:
        parser.error("timeout-seconds must be between 1 and 600")
    try:
        require(not args.output_json.exists(), "output_already_exists")
        report = audit(args.index, args.llvm_nm, args.jobs, args.timeout_seconds)
        save_report(report, args.output_json)
        print(json.dumps(report["summary"]), flush=True)
        return 0 if report["summary"]["full_link_ready"] else 1
    except AuditError as error:
        category = error.classification
    except Exception:
        # Never emit exception text: an external tool or malformed input may
        # place private symbols or object content in an exception message.
        category = "local_driver_or_toolchain_failure"
    print(json.dumps({"objects": 0, "full_link_ready": False, "runtime_compatibility_verified": False, "diagnostic_categories": {"error:" + category: 1}}), flush=True)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
