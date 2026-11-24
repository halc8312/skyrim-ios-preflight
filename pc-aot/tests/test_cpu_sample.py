"""CPU sampling core, wire format and analyzer. No device, game data or Apple SDK.

The Mach calls (thread suspend, register read) live in cpu_sampler.m and are only
compiled by the Apple host build; every decision around them is tested here.
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
UUID = '4c4c446855553144a10b798185d8698a'
SYMBOLS = """0000000000004000 t _f_401000
0000000000004100 t _TL32
0000000000005000 t _f_402000
"""


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


analyzer = load('cpu_sample_analyzer', ROOT / 'scripts/analyze-cpu-samples.py')
receiver = load('receiver_for_cpu', ROOT / 'scripts/receive_private_diagnostics.py')


def native_lines():
    cc = shutil.which(os.environ.get('CC', 'cc'))
    if not cc:
        raise unittest.SkipTest('Native C compiler needed; run on WSL/Mac')
    with tempfile.TemporaryDirectory(prefix='cpu-sample-') as tmp:
        binary = Path(tmp) / 'cpu'
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
        if os.getenv('SKYRIM_TEST_SANITIZERS') == '1':
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run([cc, *flags, '-I', str(PORT), str(PORT / 'cpu_sample_core.c'),
                        str(ROOT / 'tests/native/test_cpu_sample_core.c'), '-o', str(binary)],
                       check=True, timeout=60)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
    return result.stdout.splitlines()


class CpuSampleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lines = native_lines()
        cls.images = [line for line in cls.lines if line.startswith('IOS_CPU_IMAGE ')]
        cls.windows = [line for line in cls.lines if line.startswith('IOS_CPU_SAMPLE ')]

    def test_core_passes_and_prints_the_scenarios(self):
        self.assertIn('cpu_sample_core=PASS', self.lines)
        self.assertEqual(len(self.images), 2)
        self.assertEqual(len(self.windows), 2)

    def test_every_printed_line_satisfies_the_wire_allowlist(self):
        schema = receiver.Schema()
        for line in self.images + self.windows:
            self.assertLess(len(line), 8191)
            schema.line(line)

    def test_wire_schema_rejects_malformed_and_unexpected_fields(self):
        schema = receiver.Schema()
        good = self.windows[0]
        for bad in (good + ' path=/private/x',
                    good.replace('schema=1', 'schema=2'),
                    good.replace('stacks=', 'stacks=zz'),
                    good.replace('IOS_CPU_SAMPLE', 'IOS_CPU_SAMPLEX'),
                    'IOS_CPU_IMAGE schema=1 index=0 uuid=zz text_size=1',
                    'IOS_CPU_IMAGE schema=1 index=0 uuid=%s text_size=1 name=Skyrim' % UUID):
            with self.assertRaises(Exception, msg=bad[:80]):
                schema.line(bad)

    def test_analyzer_names_functions_and_checks_the_build(self):
        symbols = {0: analyzer.Symbols(SYMBOLS)}
        result = analyzer.summarize([*self.images, self.windows[0]], symbols, {0: UUID})
        self.assertEqual(result['invalid_records'], 0)
        self.assertEqual(result['windows'], 1)
        self.assertEqual(result['totals']['samples'], 41)
        self.assertEqual(result['totals']['listed'], 41)
        self.assertEqual(result['totals']['unresolved_leaf'], 10)
        self.assertEqual(result['totals']['suspend_errors'], 1)
        c = result['classes']['unknown']        # no timing window to classify it
        self.assertEqual({r['function']: r['samples'] for r in c['self']},
                         {'_f_401000': 31, '(outside app images)': 10})
        self.assertEqual(c['self'][0]['guest_x86_address'], '0x401000')
        inclusive = {r['function']: r['samples'] for r in c['inclusive']}
        self.assertEqual(inclusive, {'_f_401000': 31, '_TL32': 31, 'image1+0x100': 31,
                                     '_f_402000': 10, '(outside app images)': 10})
        self.assertEqual([t['thread'] for t in result['threads']], [1, 2])
        self.assertEqual(result['threads'][0]['samples'], 30)
        self.assertTrue(result['threads'][0]['main'])
        # A symbol table from another build must stop the analysis, not mislabel.
        with self.assertRaises(RuntimeError):
            analyzer.summarize([*self.images, self.windows[0]], symbols, {0: 'f' * 32})
        with self.assertRaises(RuntimeError):
            analyzer.summarize([self.windows[0]], symbols, {0: UUID})

    def test_symbol_table_can_be_selected_by_build_uuid(self):
        by_uuid = analyzer.summarize([*self.images, self.windows[0]], {UUID: analyzer.Symbols(SYMBOLS)})
        by_index = analyzer.summarize([*self.images, self.windows[0]], {0: analyzer.Symbols(SYMBOLS)})
        self.assertEqual(by_uuid['classes'], by_index['classes'])
        with self.assertRaises(RuntimeError):      # a table for a build that is not in this log
            analyzer.summarize([*self.images, self.windows[0]], {'f' * 32: analyzer.Symbols(SYMBOLS)})

    def test_long_window_reconciles_listed_omitted_and_dropped(self):
        result = analyzer.summarize([*self.images, self.windows[1]])
        self.assertEqual(result['invalid_records'], 0)
        totals = result['totals']
        self.assertEqual(totals['samples'], 1064)
        self.assertEqual(totals['dropped'], 40)
        self.assertGreater(totals['omitted'], 0)
        self.assertEqual(totals['listed'] + totals['omitted'] + totals['dropped'], totals['samples'])

    def test_tampered_windows_are_invalid_not_trusted(self):
        line = self.windows[0]
        for tampered in (line.replace('omitted_samples=0', 'omitted_samples=3'),
                         line.replace('samples=41', 'samples=40'),
                         line.replace('stacks_total=3', 'stacks_total=2'),
                         line.replace('ticks=50', 'ticks=0'),
                         line + ' extra=1'):
            result = analyzer.summarize([tampered])
            self.assertEqual((result['windows'], result['invalid_records']), (0, 1), tampered[:60])

    def test_windows_are_split_by_presented_rate(self):
        def timing(frames):
            bins = ','.join(['0'] * 6 + [str(frames)] + ['0'] * 13)
            return ('IOS_TIMING schema=1 approximate_window=1 end_ns=5000000000 window_ns=5000000000 '
                    'kind=frame_gap samples=%d total_ns=1 max_ns=1 failures_known=0 failures=0 sleep_calls=0 bins=%s'
                    % (frames, bins))
        running = analyzer.summarize([self.windows[0], timing(300)])
        self.assertEqual(list(running['classes']), ['running'])
        loading = analyzer.summarize([self.windows[0], timing(10)])
        self.assertEqual(list(loading['classes']), ['low_rate'])
        unrelated = analyzer.summarize([self.windows[0], timing(300).replace('end_ns=5000000000', 'end_ns=60000000000')])
        self.assertEqual(list(unrelated['classes']), ['unknown'])

    def test_symbol_lookup_limits(self):
        table = analyzer.Symbols(SYMBOLS + 'garbage line\n0000000000006000 U _undefined\n')
        self.assertIsNone(table.lookup(0x3fff))
        self.assertEqual(table.lookup(0x4000), '_f_401000')
        self.assertEqual(table.lookup(0x40ff), '_f_401000')
        self.assertEqual(table.lookup(0x5fff), '_f_402000')
        self.assertIsNone(table.lookup(0x5000 + 5 * 1024 * 1024))
        self.assertEqual(analyzer.guest_address('_f_401000'), 0x401000)
        self.assertIsNone(analyzer.guest_address('_TL32'))

    def test_command_line_output_has_no_raw_log_text(self):
        with tempfile.TemporaryDirectory(prefix='cpu-sample-cli-') as tmp:
            log, table = Path(tmp) / 'launch.log', Path(tmp) / 'guest.nm'
            log.write_text('\n'.join(['Documents path /private/var/mobile/Containers/SECRET', *self.images, self.windows[0]]) + '\n')
            table.write_text(SYMBOLS)
            args = [sys.executable, '-I', str(ROOT / 'scripts/analyze-cpu-samples.py'), str(log),
                    '--symbols', '0=' + str(table), '--uuid', '0=' + UUID.upper()]
            result = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data['classes']['unknown']['self'][0]['function'], '_f_401000')
            self.assertNotIn('SECRET', result.stdout + result.stderr)
            bad = subprocess.run(args[:-1] + ['0=' + 'a' * 32], capture_output=True, text=True, timeout=30)
            self.assertEqual(bad.returncode, 2)
            self.assertNotIn('SECRET', bad.stdout + bad.stderr)
            empty = Path(tmp) / 'empty.log'
            empty.write_text('nothing here\n')
            self.assertEqual(subprocess.run(args[:3] + [str(empty)], capture_output=True, text=True, timeout=30).returncode, 1)

    def test_shim_only_samples_what_it_reads_and_never_blocks_a_suspended_thread(self):
        source = (PORT / 'cpu_sampler.m').read_text()
        suspend = source.index('thread_suspend(')
        resume = source.index('thread_resume(', suspend)
        held = source[suspend:resume]
        # While a thread is stopped it may hold the allocator or a stdio lock:
        # nothing between suspend and resume may allocate, lock, log or call dyld.
        for forbidden in ('malloc', 'calloc', 'alloc]', 'fprintf', 'printf', 'fputs', 'NSLog', 'pthread_mutex',
                          '@synchronized', 'dispatch_', 'dladdr', '_dyld_', 'pthread_from_mach_thread_np',
                          'pthread_get_stack', 'task_threads', 'cpu_sample_add', 'cpu_sample_set_images', 'objc_'):
            self.assertNotIn(forbidden, held, forbidden)
        self.assertIn('thread_get_state(', held)
        self.assertIn('cpu_sample_unwind(', held)
        self.assertIn('SKYRIM_IOS_CPU_SAMPLE', source)
        # The sampler thread never samples itself.
        self.assertIn('self_port', source)
        entry = (PORT / 'ios_entry.m').read_text()
        self.assertIn('skyrim_cpu_sampler_start', entry)
        build = (ROOT / 'ios/dependencies/CMakeLists.txt').read_text()
        self.assertIn('cpu_sample_core.c', build)
        self.assertIn('cpu_sampler.m', build)


if __name__ == '__main__':
    unittest.main()
