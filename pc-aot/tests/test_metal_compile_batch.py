"""Authored/synthetic batch controls; no private shaders or Apple compiler runs."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("private_msl_batch", ROOT / "experiments/metal-compile/batch.py")
batch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(batch)
PROFILE = {"target": "ios", "deployment": "18.0", "language": "3.1", "math_mode": "safe",
           "math_functions": "precise", "preserve_invariance": False, "optimization": "default", "macros": {}}
AUTHORED = b"#include <metal_stdlib>\nusing namespace metal;\nkernel void test_empty() {}\n"
SYNTHETIC = r'''
from pathlib import Path
import os, sys, time
name, *args = sys.argv[1:]
if '--version' in args:
    print('SYNTHETIC CONTROL; NOT APPLE METAL')
    raise SystemExit(0)
output = Path(args[args.index('-o') + 1])
mode = os.environ.get('MSL_BATCH_TEST_MODE', 'success')
print('synthetic output', flush=True)
print('synthetic diagnostic', file=sys.stderr, flush=True)
if name == 'metal':
    output.write_bytes(b'synthetic AIR')
    if mode == 'one-error' and output.parent.name == 'case-0002':
        raise SystemExit(23)
    if mode == 'timeout':
        time.sleep(30)
else:
    output.write_bytes(b'WRONG' if mode == 'bad-library' else b'MTLB synthetic output')
'''


class PrivateMslBatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="msl-batch-control-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve()
        self.sources = self.directory / "authored-input"
        self.sources.mkdir()
        (self.sources / "a.metal").write_bytes(AUTHORED)
        (self.sources / "b.metal").write_bytes(AUTHORED)
        (self.sources / "c.metal").write_bytes(AUTHORED + b"// another authored input\n")
        self.profile = self.directory / "profile.json"
        self.profile.write_text(json.dumps(PROFILE), encoding="utf-8")
        self.prepared = self.directory / "prepared"
        self.output = self.directory / "run"
        self.tools = self.directory / "synthetic tools"
        self.tools.mkdir()
        for name in ("metal.exe", "metallib.exe"):
            (self.tools / name).write_bytes(b"synthetic tool identity")
        self.shim = self.directory / "synthetic.py"
        self.shim.write_text(SYNTHETIC, encoding="utf-8")
        self.original_popen = subprocess.Popen
        self.children = []
        self.calls = []

    def make_manifest(self):
        report = batch.prepare(self.sources, self.prepared, self.profile, "Authored test inputs; not runtime capture")
        self.assertEqual(report["status"], "prepared")
        return self.prepared / "manifest.json"

    def arguments(self, manifest, *extra):
        return batch.parse_args(["run", "--manifest", str(manifest), "--output", str(self.output),
                                 "--tools", str(self.tools), *extra])

    def child(self, command, **kwargs):
        if Path(command[0]).name.casefold() == "taskkill.exe":
            return self.original_popen(command, **kwargs)
        self.assertTrue(all(process.poll() is not None for process in self.children), "compiler commands overlapped")
        self.assertIs(kwargs["shell"], False)
        self.assertNotEqual(kwargs["stdout"], subprocess.PIPE)
        self.calls.append(command)
        process = self.original_popen([sys.executable, str(self.shim), Path(command[0]).stem, *command[1:]], **kwargs)
        self.children.append(process)
        return process

    def run_synthetic(self, manifest, mode="success", *extra):
        with patch.object(batch.platform, "system", return_value="Windows"), \
                patch.dict(os.environ, {"MSL_BATCH_TEST_MODE": mode}), \
                patch.object(batch.offline.subprocess, "Popen", side_effect=self.child):
            report = batch.run(self.arguments(manifest, *extra))
        saved = json.loads((self.output / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(report, saved)
        for key in ("production_parity", "gpu_execution_verified", "physical_iphone_verified",
                    "game_rendering_verified", "performance_improvement_verified"):
            self.assertIs(report[key], False)
        self.assertFalse(report["limits"]["child_memory_limit_enforced"])
        self.assertFalse(report["limits"]["descendant_process_count_limit_enforced"])
        return report

    def change_manifest(self, path, update):
        manifest = json.loads(path.read_text(encoding="utf-8"))
        update(manifest)
        path.write_text(json.dumps(manifest), encoding="utf-8")

    def test_snapshot_preserves_duplicates_and_bytes_with_explicit_options(self):
        manifest = self.make_manifest()
        prepared = json.loads((self.prepared / "prepare-result.json").read_text())
        self.assertEqual((prepared["cases"], prepared["unique_source_hashes"], prepared["unique_input_keys"]), (3, 2, 2))
        rows = json.loads(manifest.read_text())["cases"]
        for row in rows:
            data = (self.prepared / row["source"]).read_bytes()
            self.assertEqual(row["sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(row["bytes"], len(data))
            self.assertEqual(row["options"], PROFILE)

    def test_frozen_sources_compile_serially_and_repeats_do_not_silently_deduplicate(self):
        manifest = self.make_manifest()
        (self.sources / "a.metal").write_bytes(b"original changed after snapshot")
        report = self.run_synthetic(manifest, "success", "--repeat", "2")
        self.assertEqual(report["status"], "passed")
        self.assertTrue(report["offline_compile_verified"])
        self.assertEqual(report["summary"]["passed"], 6)
        self.assertEqual(report["manifest"]["unique_source_hashes"], 2)
        self.assertEqual(len(report["commands"]), 14)
        self.assertEqual((self.output / "sources/case-0001.metal").read_bytes(), AUTHORED)
        for row in report["runs"]:
            self.assertEqual(len(row["artifacts"]), 2)
            self.assertGreater(row["compile_duration_ns"], 0)
            self.assertGreater(row["link_duration_ns"], 0)
        compiles = [event["argv"] for event in report["commands"] if event["label"].endswith("-compile")]
        for command in compiles:
            self.assertIn("-std=metal3.1", command)
            self.assertIn("-fmetal-math-mode=safe", command)
            self.assertIn("-fmetal-math-fp32-functions=precise", command)
            self.assertFalse(any(arg.startswith("-D") for arg in command))
            self.assertNotIn("-fno-fast-math", command)

    def test_compile_failure_is_retained_while_other_cases_are_examined(self):
        report = self.run_synthetic(self.make_manifest(), "one-error")
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual([row["status"] for row in report["runs"]], ["passed", "failed", "passed"])
        self.assertEqual(report["summary"]["failed"], 1)
        failed = next(event for event in report["commands"] if event["returncode"] == 23)
        self.assertIn("synthetic diagnostic", (self.output / failed["stderr"]).read_text())
        self.assertEqual(len(report["runs"][1]["artifacts"]), 1)

    def test_invalid_library_is_not_promoted_to_success(self):
        report = self.run_synthetic(self.make_manifest(), "bad-library")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual(report["summary"]["failed"], 3)
        self.assertTrue(all("Invalid metallib" in row["error"] for row in report["runs"]))

    def test_timeout_stops_further_compilers_after_bounded_cleanup(self):
        report = self.run_synthetic(self.make_manifest(), "timeout", "--timeout", "0.5")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual([row["status"] for row in report["runs"]], ["timed_out", "not_run", "not_run"])
        event = report["commands"][-1]
        self.assertTrue(event["timed_out"])
        self.assertTrue(event["termination"]["parent_exited"])

    def test_late_source_mismatch_fails_before_any_compiler_starts(self):
        manifest = self.make_manifest()
        (self.prepared / "sources/case-0003.metal").write_bytes(b"changed")
        with patch.object(batch.offline.subprocess, "Popen") as process:
            report = batch.run(self.arguments(manifest))
        process.assert_not_called()
        self.assertFalse(report["offline_compile_verified"])
        self.assertIn("mismatch", report["error"])

    def test_source_path_escape_and_duplicate_ids_are_rejected(self):
        for mode in ("escape", "duplicate"):
            with self.subTest(mode=mode):
                self.prepared = self.directory / ("prepared-" + mode)
                self.output = self.directory / ("run-" + mode)
                manifest = self.make_manifest()
                def update(value):
                    if mode == "escape":
                        value["cases"][2]["source"] = "../outside.metal"
                    else:
                        value["cases"][2]["id"] = value["cases"][0]["id"]
                self.change_manifest(manifest, update)
                with patch.object(batch.offline.subprocess, "Popen") as process:
                    report = batch.run(self.arguments(manifest))
                process.assert_not_called()
                self.assertEqual(report["status"], "failed")

    def test_no_production_parity_or_implicit_compile_options_are_accepted(self):
        for mode in ("parity", "missing_option", "macro_injection"):
            with self.subTest(mode=mode):
                self.prepared = self.directory / ("prepared-" + mode)
                self.output = self.directory / ("run-" + mode)
                manifest = self.make_manifest()
                def update(value):
                    if mode == "parity":
                        value["production_parity"] = True
                    elif mode == "missing_option":
                        del value["cases"][0]["options"]["math_mode"]
                    else:
                        value["cases"][0]["options"]["macros"] = {"VALUE": "1; arbitrary()"}
                self.change_manifest(manifest, update)
                with patch.object(batch.offline.subprocess, "Popen") as process:
                    report = batch.run(self.arguments(manifest))
                process.assert_not_called()
                self.assertFalse(report["offline_compile_verified"])

    def test_input_key_changes_for_source_or_any_explicit_option(self):
        original = batch.input_key("0" * 64, PROFILE)
        self.assertNotEqual(original, batch.input_key("1" * 64, PROFILE))
        changes = {"target": "macos", "deployment": "19.0", "language": "3.2", "math_mode": "relaxed",
                   "math_functions": "fast", "preserve_invariance": True, "macros": {"VALUE": "7u"}}
        for field, value in changes.items():
            with self.subTest(field=field):
                updated = {**PROFILE, field: value}
                batch.validate_profile(updated)
                self.assertNotEqual(original, batch.input_key("0" * 64, updated))

    def test_explicit_macros_invariance_and_legacy_language_are_forwarded(self):
        profile = {**PROFILE, "target": "macos", "deployment": "15.0", "language": "2.4",
                   "preserve_invariance": True, "macros": {"VALUE": "7u", "ENABLED": "true"}}
        command = batch.compile_command("metal", "input.metal", "out.air", profile, "sdk with spaces")
        self.assertIn("-std=macos-metal2.4", command)
        self.assertIn("-fpreserve-invariance", command)
        self.assertIn("-DVALUE=7u", command)
        self.assertIn("-DENABLED=true", command)
        self.assertEqual(command[command.index("-isysroot") + 1], "sdk with spaces")
        self.assertFalse(any("PROBE_BIAS" in arg for arg in command))

    def test_input_byte_and_count_bounds_fail_instead_of_truncating_selection(self):
        for name, value in (("MAX_CASES", 2), ("MAX_SOURCE_BYTES", 8), ("MAX_TOTAL_SOURCE_BYTES", len(AUTHORED))):
            with self.subTest(limit=name), patch.object(batch, name, value):
                output = self.directory / name
                report = batch.prepare(self.sources, output, self.profile, "Authored bounded test")
                self.assertEqual(report["status"], "failed")
                self.assertTrue((output / "prepare-result.json").is_file())
                self.assertFalse((output / "manifest.json").exists())

    def test_uncaptured_external_includes_are_rejected_even_with_line_splicing(self):
        for data in (b'#include "local.h"\n', b'#include_next <metal_stdlib>\n', b'#inc\\\nlude "local.h"\n',
                     b'#include<local.h>\n', b'#include"local.h"\n', b'#include/**/"local.h"\n',
                     b'#inc\\\nlude<local.h>\n', b'#import<metal_stdlib>\n', b'#include_next<metal_stdlib>\n',
                     b'#include\n', b'#include HEADER\n'):
            with self.subTest(data=data), self.assertRaises(batch.BatchError):
                batch.validate_source(data)
        batch.validate_source(b"#include <metal_stdlib>\n#include<simd/simd.h>\n# include/**/<metal_stdlib>\n")

    def test_alternative_directive_introducer_is_explicitly_unsupported(self):
        for data in (b'%:include<local.h>\n', b'%:include <metal_stdlib>\n',
                     b' \t%:inc\\\nlude"local.h"\n', b'/*comment*/ %:include<local.h>\n'):
            with self.subTest(data=data), self.assertRaisesRegex(batch.BatchError, "Alternative"):
                batch.validate_source(data)

    def test_comment_markers_in_quoted_literals_cannot_hide_an_external_include(self):
        for opening, closing in ((b'"/*"', b'"*/"'), (b"'/*'", b"'*/'"),
                                 (b'L"/*"', b'L"*/"'), (b'u8"/*"', b'u8"*/"')):
            data = b'#define OPEN ' + opening + b'\n#include "external.h"\n#define CLOSE ' + closing + b'\n'
            with self.subTest(opening=opening), self.assertRaisesRegex(batch.BatchError, "declared Metal SDK includes"):
                batch.validate_source(data)
        # Escaped quotes, slashes and numeric separators are ordinary lexical
        # input; none of these strings is a real preprocessing directive.
        batch.validate_source(br'''#include <metal_stdlib>
#define URL "https://example.invalid/*not a comment*/"
#define QUOTE "escaped \" /* still inside */ and \\"
#define CHAR '\''
#define WIDE L'/'
constant uint count = 1'000;
constant uint mask = 0xAB'CD;
constant float scale = 1.0'5e+2;
''')

    def test_supported_line_endings_and_phase_two_splicing_do_not_hide_directives(self):
        for newline in (b'\n', b'\r\n', b'\r'):
            for first, second in ((b'#include <metal_stdlib>', b'#include "external.h"'),
                                  (b'#include <metal_stdlib>', b'#inc\\' + newline + b'lude<external.h>')):
                with self.subTest(newline=newline, second=second), self.assertRaises(batch.BatchError):
                    batch.validate_source(first + newline + second + newline)
            batch.validate_source(b'#inc\\' + newline + b'lude <metal_stdlib>' + newline)
            # Splicing precedes comments: the apparent include continues //.
            batch.validate_source(b'// continued \\' + newline + b'#include "ignored.h"' + newline + b'#include <metal_stdlib>' + newline)
            # The block-comment delimiters themselves can be line-spliced.
            batch.validate_source(b'/\\' + newline + b'* #include "ignored.h" *\\' + newline + b'/' + newline + b'#include<metal_stdlib>')

    def test_block_comment_newlines_keep_a_following_directive_at_line_start(self):
        for newline in (b'\n', b'\r\n', b'\r'):
            data = b'kernel void k() {} /* comment' + newline + b' */ #include "external.h"' + newline
            with self.subTest(newline=newline), self.assertRaisesRegex(batch.BatchError, "declared Metal SDK includes"):
                batch.validate_source(data)
        batch.validate_source(b'/* #include "ignored.h"\n*/ #/**/include/**/<metal_stdlib>\n// eof comment')

    def test_unclassified_and_unterminated_lexical_forms_fail_explicitly(self):
        for data, reason in ((b'/* unfinished', 'Unterminated block'),
                             (b'"unfinished', 'Unterminated quoted'),
                             (b"'unfinished", 'Unterminated quoted'),
                             (b'"line\n#include <metal_stdlib>"', 'Unspliced newline'),
                             (b'R"tag(raw\n#include "external.h"\n)tag"', 'Raw string'),
                             (b'u8R"(raw)"', 'Raw string'),
                             (b'#inc\\ \nlude "external.h"', 'Whitespace'),
                             (b'#inc\\\t\r\nlude "external.h"', 'Whitespace'),
                             (b'??=include "external.h"', 'Trigraph'),
                             (br'#\u0069nclude "external.h"', 'Universal-character')):
            with self.subTest(data=data), self.assertRaisesRegex(batch.BatchError, reason):
                batch.validate_source(data)

    def test_lexical_parse_view_never_rewrites_original_compiler_snapshot_bytes(self):
        original = b'/* authored */\r\n#inc\\\r\nlude <metal_stdlib>\r\n#define TEXT "/* literal */"\r\nkernel void k() {}\r\n'
        (self.sources / "a.metal").write_bytes(original)
        manifest = self.make_manifest()
        prepared = json.loads(manifest.read_text())
        first = prepared["cases"][0]
        self.assertEqual((self.prepared / first["source"]).read_bytes(), original)
        self.assertEqual(first["sha256"], hashlib.sha256(original).hexdigest())
        self.output.mkdir()
        snapshots, _ = batch.snapshot_manifest(manifest, self.output)
        self.assertEqual(Path(snapshots[0]["snapshot"]).read_bytes(), original)
        self.assertEqual(snapshots[0]["sha256"], first["sha256"])
        # These constructs remain accepted; include validation is deliberately
        # not a context-free-source or deduplication eligibility proof.
        batch.validate_source(b'#define CAT(a,b) a##b\n#line 7 "authored.metal"\nconstant char name[] = __FILE__;\n')

    def test_duplicate_json_fields_are_rejected(self):
        path = self.directory / "duplicate.json"
        path.write_text('{"math_mode":"safe","math_mode":"fast"}')
        with self.assertRaisesRegex(batch.BatchError, "Duplicate"):
            batch.read_json(path)

    def test_budget_exhaustion_never_starts_an_unbudgeted_tool(self):
        manifest = self.make_manifest()
        with patch.object(batch.time, "monotonic", side_effect=[0.0, 2.0, 2.1]), \
                patch.object(batch.offline.subprocess, "Popen") as process:
            report = batch.run(self.arguments(manifest, "--budget", "1"))
        process.assert_not_called()
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual(report["summary"]["not_run"], 3)
        self.assertFalse(report["offline_compile_verified"])

    def test_interruption_keeps_a_terminal_result_and_is_reraised(self):
        manifest = self.make_manifest()
        original = batch.offline.run_command
        def interrupted(command, label, *args):
            if label.endswith("-compile"):
                raise KeyboardInterrupt()
            return original(command, label, *args)
        with patch.object(batch.platform, "system", return_value="Windows"), \
                patch.object(batch.offline.subprocess, "Popen", side_effect=self.child), \
                patch.object(batch.offline, "run_command", side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt):
                batch.run(self.arguments(manifest))
        report = json.loads((self.output / "result.json").read_text())
        self.assertEqual(report["status"], "interrupted")
        self.assertEqual(report["summary"]["interrupted"], 1)
        self.assertFalse(report["offline_compile_verified"])

    def test_tool_budget_exhaustion_stops_later_cases_and_remains_distinct(self):
        manifest = self.make_manifest()
        original = batch.offline.run_command
        compile_calls = []
        def exhausted(command, label, *args):
            if label.endswith("-compile"):
                compile_calls.append(label)
                raise batch.offline.BudgetExhausted("inherited tool deadline expired")
            return original(command, label, *args)
        with patch.object(batch.platform, "system", return_value="Windows"), \
                patch.object(batch.offline.subprocess, "Popen", side_effect=self.child), \
                patch.object(batch.offline, "run_command", side_effect=exhausted):
            report = batch.run(self.arguments(manifest))
        self.assertEqual(len(compile_calls), 1)
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual(report["summary"]["budget_exhausted"], 1)
        self.assertEqual(report["summary"]["not_run"], 2)
        self.assertFalse(report["offline_compile_verified"])

    def test_fresh_output_and_checkout_privacy_guard_precede_any_write(self):
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_bytes(b"existing evidence")
        with self.assertRaises(FileExistsError):
            batch.fresh_directory(self.output)
        self.assertEqual(sentinel.read_bytes(), b"existing evidence")
        for path in (ROOT / "experiments/metal-compile/generated", ROOT / "tests/private-shaders"):
            with self.subTest(path=path), self.assertRaises(batch.BatchError):
                batch.private_destination(path)

    def test_existing_and_dangling_output_symlinks_are_not_fresh(self):
        for existing in (True, False):
            with self.subTest(target_exists=existing):
                target = self.directory / ("existing-link-target" if existing else "absent-link-target")
                if existing:
                    target.mkdir()
                    (target / "keep").write_bytes(b"retained evidence")
                link = self.directory / ("existing-output-link" if existing else "dangling-output-link")
                try:
                    link.symlink_to(target, target_is_directory=True)
                except (OSError, NotImplementedError) as error:
                    self.skipTest("directory symlink creation unavailable: " + str(error))
                with self.assertRaises(FileExistsError):
                    batch.fresh_directory(link)
                self.assertTrue(link.is_symlink())
                if existing:
                    self.assertEqual(list(target.iterdir()), [target / "keep"])
                    self.assertEqual((target / "keep").read_bytes(), b"retained evidence")
                else:
                    self.assertFalse(target.exists())

    def test_fresh_output_keeps_private_restrictions_and_valid_parent_links(self):
        checkout = self.directory / "authored-checkout"
        checkout.mkdir()
        local, private, vendor = [checkout / name for name in ("local", "private", "vendor")]
        for directory in (local, private, vendor):
            directory.mkdir()
        parent = private / "real-parent"
        parent.mkdir()
        try:
            (local / "allowed-parent").symlink_to(parent, target_is_directory=True)
            (local / "restricted-parent").symlink_to(vendor, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest("directory symlink creation unavailable: " + str(error))
        with patch.object(batch, "ROOT", checkout.resolve()):
            for path in (local / "fresh", private / "nested/fresh", self.directory / "outside-checkout/fresh",
                         local / "allowed-parent/nested/fresh"):
                with self.subTest(allowed=path):
                    self.assertEqual(batch.fresh_directory(path), path.resolve())
                    self.assertTrue(path.is_dir())
            for path in (vendor / "not-created", local / "restricted-parent/not-created"):
                with self.subTest(rejected=path), self.assertRaises(batch.BatchError):
                    batch.fresh_directory(path)
                self.assertFalse(path.exists())
        self.assertEqual(list(vendor.iterdir()), [])

    def test_cli_budget_and_repeat_bounds_are_finite(self):
        for flag, value in (("--repeat", "0"), ("--repeat", "4"), ("--budget", "nan"),
                            ("--budget", "3601"), ("--timeout", "0"), ("--timeout", "121")):
            with self.subTest(flag=flag, value=value), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    batch.parse_args(["run", "--manifest", "m.json", "--output", "fresh", flag, value])


if __name__ == "__main__":
    unittest.main()
