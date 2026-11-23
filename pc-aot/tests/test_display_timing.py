"""Display/GPU observers: real recorder and core, analyzer, wire schema, cache identity.

No device, game data or Apple SDK needed. The Objective-C shim (display_timing.m)
is only compiled by the Apple host build; its logic lives in the C core tested here.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'ios/runtime-port'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module      # dataclasses in the receiver look themselves up here
    spec.loader.exec_module(module)
    return module


analyzer = load('frame_timing_for_display', ROOT / 'scripts/analyze-frame-timing.py')
namespace = load('cache_namespace_for_display', ROOT / 'ios/dependencies/pipeline_cache_namespace.py')
receiver = load('receiver_for_display', ROOT / 'scripts/receive_private_diagnostics.py')
NEW_KINDS = ('display_interval', 'display_dropped', 'display_latency',
             'gpu_command_buffer', 'gpu_frame_busy', 'gpu_frame_span')


def timing_line(kind, end_ns, bins, total_ns, failures_known=0, failures=0):
    return ('IOS_TIMING schema=1 approximate_window=1 end_ns=%d window_ns=5000000000 kind=%s samples=%d '
            'total_ns=%d max_ns=%d failures_known=%d failures=%d sleep_calls=0 bins=%s'
            % (end_ns, kind, sum(bins), total_ns, total_ns, failures_known, failures, ','.join(map(str, bins))))


def bins_at(index, count):
    bins = [0] * 20
    bins[index] = count
    return bins


class DisplayTimingTests(unittest.TestCase):
    def test_actual_core_and_recorder(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('Native C compiler needed; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='display-timing-') as tmp:
            binary = Path(tmp) / 'display'
            flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread']
            if os.getenv('SKYRIM_TEST_SANITIZERS') == '1':
                flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run([cc, *flags, '-I', str(PORT), str(PORT / 'display_timing_core.c'),
                            str(PORT / 'performance_metrics.c'), str(ROOT / 'tests/native/test_display_timing.c'),
                            '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('display_timing=PASS', result.stdout)
            logged = {line.split(' kind=')[1].split(' ')[0]: line
                      for line in result.stderr.splitlines() if line.startswith('IOS_TIMING ')}
            self.assertIn('display_latency', logged)
            self.assertIn('gpu_command_buffer', logged)
            # The logged lines must satisfy the app's own wire allowlist and parser.
            schema = receiver.Schema()
            for line in logged.values():
                schema.line(line)
            self.assertIn('failures_known=1', logged['gpu_command_buffer'])
            self.assertIn('failures_known=0', logged['display_latency'])

    def test_kind_ids_are_append_only(self):
        header = (PORT / 'performance_metrics.h').read_text()
        body = header[header.index('enum SkyrimPerfKind {'):header.index('SKYRIM_PERF_KIND_COUNT')]
        names = [n for n in re.findall(r'\bSKYRIM_PERF_[A-Z0-9_]+\b', re.sub(r'/\*.*?\*/', '', body, flags=re.S))
                 if n != 'SKYRIM_PERF_KIND_COUNT']
        # Older logs and analyzers number kinds by position; none may move.
        self.assertEqual(names[names.index('SKYRIM_PERF_SAVE_CLOSE_CALL')], 'SKYRIM_PERF_SAVE_CLOSE_CALL')
        self.assertEqual(names.index('SKYRIM_PERF_SAVE_CLOSE_CALL'), 41)
        self.assertEqual(names[42:], ['SKYRIM_PERF_' + k.upper() for k in NEW_KINDS])
        self.assertEqual(list(analyzer.KINDS[41:]), ['save_close_call', *NEW_KINDS])
        self.assertEqual(len(analyzer.KINDS), len(names))
        source = (PORT / 'performance_metrics.c').read_text()
        for kind in NEW_KINDS:
            self.assertIn('"%s"' % kind, source)

    def test_analyzer_display_summary_and_old_logs(self):
        lines = [
            timing_line('frame_gap', 5000000000, bins_at(6, 100), 1666666700),
            # 100 intervals: 90 steady 16.67 ms, 8 missed refreshes (33 ms), 2 long (250 ms).
            timing_line('display_interval', 5000000000,
                        [0, 0, 0, 0, 0, 0, 90, 0, 0, 8, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0], 90 * 16666667 + 8 * 33333334 + 2 * 250000000),
            timing_line('display_dropped', 5000000000, bins_at(0, 3), 0),
            timing_line('gpu_command_buffer', 5000000000, bins_at(4, 40), 40 * 7000000, 1, 2),
            timing_line('gpu_frame_busy', 5000000000, bins_at(5, 10), 10 * 11000000),
        ]
        result = analyzer.summarize(lines)
        self.assertEqual(result['invalid_records'], 0)
        display = result['display']
        self.assertEqual(display['dropped_presents'], 3)
        self.assertEqual(display['intervals'], 100)
        self.assertEqual(display['interval_bins_ms'], {'12-16.67': 90, '25-33.33': 8, '166.67-250': 2})
        self.assertAlmostEqual(display['share_over_20ms'], 0.10)
        self.assertAlmostEqual(display['share_over_33ms'], 0.02)
        self.assertAlmostEqual(display['share_over_100ms'], 0.02)
        self.assertEqual(result['kinds']['gpu_command_buffer']['failures'], 2)
        self.assertIsNone(result['kinds']['gpu_frame_busy']['failures'])
        self.assertIn('gpu_frame_busy', result['kinds'])
        # A launch from an older build has no such kinds: absent, not zero.
        old = analyzer.summarize([timing_line('frame_gap', 5000000000, bins_at(6, 100), 1666666700)])
        self.assertIsNone(old['display'])
        self.assertNotIn('display_interval', old['kinds'])

    def test_long_display_gap_correlates_with_frame_gap_spans(self):
        stall = ('IOS_STALL schema=3 threshold_ns=50000000 count=2 dropped_busy=0 dropped_full=0 '
                 'window_start_ns=1000000000 window_end_ns=6000000000 '
                 'fields=kind,thread,start_ns,end_ns,failed,estimated_end,role,source,result '
                 'spans=0,1,2000000000,2300000000,0,0,0,0,0;42,7,2010000000,2290000000,0,0,0,0,0')
        result = analyzer.summarize([timing_line('frame_gap', 6000000000, bins_at(6, 5), 83333335), stall])
        overlaps = result['longest_sampled_frame_gaps'][0]['overlaps']
        self.assertEqual([o['kind'] for o in overlaps], ['display_interval'])

    def test_wire_schema_accepts_only_the_status_line_it_prints(self):
        schema = receiver.Schema()
        for value in ('IOS_DISPLAY_TIMING schema=1 disabled=0 presented=1 gpu_methods=3',
                      'IOS_DISPLAY_TIMING schema=1 disabled=1 presented=0 gpu_methods=0'):
            schema.line(value)
        for value in ('IOS_DISPLAY_TIMING schema=1 disabled=2 presented=1 gpu_methods=3',
                      'IOS_DISPLAY_TIMING schema=1 disabled=0 presented=1 gpu_methods=4',
                      'IOS_DISPLAY_TIMING schema=1 disabled=0 presented=1 gpu_methods=3 queue=AGXG16',
                      'IOS_DISPLAY_TIMING schema=2 disabled=0 presented=1 gpu_methods=3'):
            with self.assertRaises(Exception, msg=value):
                schema.line(value)
        for kind in NEW_KINDS:
            schema.line(timing_line(kind, 5000000000, bins_at(3, 2), 8000000))
        # Context histograms stay limited to the kinds that have a context.
        context = ('IOS_TIMING_CONTEXT schema=1 approximate_window=1 end_ns=5 window_ns=5 kind=display_interval '
                   'samples=0 total_ns=0 max_ns=0 failures_known=0 failures=0 sleep_calls=0 bins=%s role=0 source=0 result=0'
                   % ','.join(['0'] * 20))
        with self.assertRaises(Exception):
            schema.line(context)

    def test_shim_uses_only_public_observation_points(self):
        source = (PORT / 'display_timing.m').read_text()
        self.assertIn('@selector(nextDrawable)', source)
        self.assertIn('addPresentedHandler:', source)
        self.assertIn('addCompletedHandler:', source)
        self.assertIn('SKYRIM_IOS_DISPLAY_TIMING', source)
        # Observation only: the original implementation is always called and its
        # result returned unchanged, and nothing here waits, locks or presents.
        for forbidden in ('presentDrawable', '[drawable present', 'waitUntil', 'dispatch_semaphore',
                          '@synchronized', 'pthread_mutex', 'commit]'):
            self.assertNotIn(forbidden, source)
        self.assertIn('original_next_drawable)(self, command)', source)
        entry = (PORT / 'ios_entry.m').read_text()
        self.assertLess(entry.index('skyrim_device_platform_initialize();'),
                        entry.index('skyrim_display_timing_install();'))
        self.assertLess(entry.index('skyrim_display_timing_install();'),
                        entry.index('skyrim_mobile_initialize('))
        build = (ROOT / 'ios/dependencies/CMakeLists.txt').read_text()
        self.assertIn('display_timing_core.c', build)
        self.assertIn('display_timing.m', build)

    def test_pinned_moltenvk_baseline_stays_untouched(self):
        # The shim exists so the byte-pinned native submission sources need no edit.
        deps = ROOT / 'ios/dependencies'
        verify = (deps / 'verify_lifecycle_baseline.py').read_text()
        for name in ('MVKQueue.mm', 'MVKDevice.mm', 'MVKImage.mm', 'MVKCommandBuffer.mm'):
            self.assertIn(name, verify)
        script = (deps / 'build-dependencies.sh').read_text()
        self.assertIn('verify_lifecycle_baseline.py', script)
        for patch in deps.glob('patch_mvk_*.py'):
            text = patch.read_text()
            for name in ('MVKImage.mm', 'MVKQueue.mm', 'MVKCommandBuffer.mm', 'MVKDevice.mm'):
                if patch.name != 'patch_mvk_lifecycle.py':   # the rejected, unused gate
                    self.assertNotIn("'%s'" % name, text, patch.name)

    def test_observation_files_keep_the_reviewed_cache_identity(self):
        contract = json.loads((ROOT / namespace.CONTRACT).read_text())
        header = PORT / 'performance_metrics.h'
        self.assertIn(namespace.source_digest(header.read_bytes()),
                      contract['observations']['ios/runtime-port/performance_metrics.h'])
        identity = namespace.identities(ROOT, 'clang-test', '/test/iphoneos.sdk', '18.0')
        self.assertEqual(identity['reviewed'], 1)
        # Pinned from the revision before this work (3209aab). An intentional,
        # shader-relevant change must update this value on purpose; an
        # observation-only change must not move it.
        self.assertEqual(identity['reuse'], BASELINE_REUSE_IDENTITY)


BASELINE_REUSE_IDENTITY = '889f546780319fa46e4c818e4b6e37ace37f7e04b45ff346ce84c4566a8a5267'

if __name__ == '__main__':
    unittest.main()
