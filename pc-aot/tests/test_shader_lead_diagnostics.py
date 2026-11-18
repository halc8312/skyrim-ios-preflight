"""Authored numeric producer records; no game input, devices, or services."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


analysis = load('shader_lead_analysis', 'scripts/analyze-frame-timing.py')
receiver = load('shader_lead_receiver', 'scripts/receive_private_diagnostics.py')


def lead(**overrides):
    values = dict(schema=1, seq=1, result=1, ready_ns=1010000000, queued_ns=1020000000,
                  worker_ns=1030000000, compile_ns=1040000000, published_ns=1150000000,
                  wait_start_ns=1100000000, wait_end_ns=1200000000)
    values.update(overrides)
    return 'IOS_SHADER_LEAD ' + ' '.join(f'{key}={value}' for key, value in values.items())


def unknown(**overrides):
    return lead(**{**dict.fromkeys(analysis.SHADER_LEAD_TIMES[:5], 0), **overrides})


class ShaderLeadDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = receiver.Schema()

    def parse(self, row):
        return analysis.shader_lead(row.split(' ', 1)[1])

    def test_verified_producer_can_precede_or_follow_wait_start_and_ready_can_be_unknown(self):
        cases = [lead(), lead(published_ns=1090000000), lead(ready_ns=0),
                 lead(ready_ns=0, queued_ns=1110000000, worker_ns=1120000000,
                      compile_ns=1130000000, published_ns=1200000000),
                 unknown(), unknown(result=2), unknown(seq=analysis.U64),
                 unknown(wait_start_ns=analysis.U64-50000000, wait_end_ns=analysis.U64)]
        for row in cases:
            with self.subTest(row=row):
                self.schema.line(row)
                self.assertGreaterEqual(self.parse(row)['wait_end_ns'] - self.parse(row)['wait_start_ns'],
                                        analysis.THRESHOLD)

    def test_incomplete_producer_invalid_order_unknown_fields_and_numeric_overflow_are_rejected(self):
        cases = [lead(schema=2), lead(seq=0), lead(seq=-1), lead(seq=analysis.U64+1),
                 lead().replace(' seq=1', ''), lead(result=0), lead(result=3), lead(result=2),
                 unknown(ready_ns=1), unknown(queued_ns=1), unknown(worker_ns=1),
                 unknown(compile_ns=1), unknown(published_ns=1),
                 lead(ready_ns=1020000001), lead(worker_ns=1019999999),
                 lead(compile_ns=1029999999), lead(published_ns=1039999999),
                 lead(published_ns=1200000001), lead(wait_start_ns=0),
                 lead(wait_end_ns=1099999999), lead(wait_end_ns=1149999999),
                 lead(ready_ns=-1), lead(ready_ns='NaN'), lead(ready_ns=analysis.U64+1),
                 lead() + ' wait_end_ns=1200000000', lead() + ' shader=private',
                 lead().replace(' ready_ns=1010000000', ''),
                 lead().replace('ready_ns=1010000000', 'ready_ns=1.01')]
        for row in cases:
            with self.subTest(row=row):
                with self.assertRaises(ValueError):
                    self.parse(row)
                with self.assertRaises(receiver.Rejected):
                    self.schema.line(row)
        result = analysis.summarize(cases)
        self.assertEqual(result['invalid_records'], len(cases))
        self.assertEqual(result['shader_lead']['status'], 'unobserved')
        self.assertNotIn('private', json.dumps(result))

    def test_launch_relative_times_signed_lead_and_unknown_are_distinct_from_zero(self):
        rows = ['IOS_STAGE sdl-application-entry monotonic=1.000000000', lead(),
                lead(seq=2, ready_ns=0, queued_ns=1110000000, worker_ns=1120000000,
                     compile_ns=1130000000, published_ns=1200000000),
                lead(seq=3, published_ns=1090000000), unknown(seq=4), unknown(seq=5, result=2)]
        result = analysis.summarize(rows)['shader_lead']
        self.assertEqual(result['time_origin'], 'launch')
        self.assertEqual(result['observations'], 5)
        self.assertEqual({key: value['count'] for key, value in result['results'].items()},
                         {'hit_verified_prefetch': 3, 'hit_producer_unobserved': 1, 'create': 1})
        first, later, published, unobserved, created = result['longest_waits']
        self.assertEqual(first['times_ms_from_origin']['wait_start'], 100)
        self.assertEqual(first['producer_lead_to_wait_start_ms']['compile'], 60)
        self.assertEqual(first['compile_to_publication_ms'], 110)
        self.assertEqual((first['ready_to_queue_ms'], first['queue_to_worker_ms'],
                          first['worker_to_compile_request_ms']), (10, 10, 10))
        self.assertEqual(later['producer_lead_to_wait_start_ms']['compile'], -30)
        self.assertIsNone(later['ready_to_queue_ms'])
        self.assertIsNone(later['times_ms_from_origin']['ready'])
        self.assertEqual(published['producer_lead_to_wait_start_ms']['published'], 10)
        for row in (unobserved, created):
            self.assertFalse(row['verified_prefetch_producer'])
            self.assertIsNone(row['compile_to_publication_ms'])
            self.assertTrue(all(value is None for value in row['producer_lead_to_wait_start_ms'].values()))
        self.assertEqual(result['producer_intervals']['ready_to_queue']['samples'], 2)
        self.assertEqual(result['producer_intervals']['queue_to_worker']['samples'], 3)
        zero = analysis.summarize([lead(worker_ns=1020000000)])['shader_lead']['longest_waits'][0]
        self.assertEqual(zero['queue_to_worker_ms'], 0)

    def test_reporter_sequence_keeps_identical_times_and_counts_as_distinct_observations(self):
        drop = 'IOS_SHADER_LEAD_DROPS schema=1 seq=20 busy=2 full=3'
        self.schema.line(drop)
        result = analysis.summarize([lead(seq=8), lead(seq=8), lead(seq=12), drop, drop,
                                     drop.replace('seq=20', 'seq=24')])['shader_lead']
        self.assertEqual((result['observations'], result['duplicate_records']), (2, 2))
        self.assertEqual((result['duplicate_lead_records'], result['duplicate_drop_records']), (1, 1))
        self.assertEqual({row['seq'] for row in result['longest_waits']}, {8, 12})
        self.assertEqual((result['drop_records'], result['dropped_busy'], result['dropped_full']), (2, 4, 6))
        self.assertEqual(result['status'], 'observed_with_drops')
        alone = analysis.summarize([drop])['shader_lead']
        self.assertEqual(alone['status'], 'drops_only')
        self.assertEqual(alone['longest_waits'], [])
        self.assertIsNone(alone['results']['hit_verified_prefetch']['max_wait_ms'])
        for bad in (drop.replace('schema=1', 'schema=2'), drop.replace('seq=20', 'seq=0'),
                    drop.replace('seq=20', f'seq={analysis.U64+1}'), drop.replace(' seq=20', ''),
                    drop.replace('busy=2', 'busy=-1'),
                    drop.replace('full=3', f'full={analysis.U64+1}'), drop + ' address=123',
                    drop + ' busy=2'):
            with self.subTest(row=bad), self.assertRaises(receiver.Rejected):
                self.schema.line(bad)

    def test_conflicting_sequence_reuse_is_not_attributed_and_receiver_scopes_each_launch(self):
        drop = 'IOS_SHADER_LEAD_DROPS schema=1 seq=42 busy=2 full=3'
        conflicting = [lead(seq=42), unknown(seq=42), drop, unknown(seq=55)]
        report = analysis.summarize(conflicting)
        summary = report['shader_lead']
        self.assertEqual((report['invalid_records'], summary['conflicting_sequences'],
                          summary['conflicting_records']), (3, 1, 3))
        self.assertEqual(summary['status'], 'observed_with_conflicts')
        self.assertEqual((summary['observations'], summary['drop_records'], summary['dropped_full']), (1, 0, 0))
        self.assertEqual(summary['longest_waits'][0]['seq'], 55)
        self.assertEqual(analysis.summarize([drop, lead(seq=42)])['shader_lead']['status'], 'conflicts_only')

        def bundle(current, previous=None):
            sessions = [dict(slot='current', capture='live_prefix', build=15, snapshot_bytes=10000,
                             excluded_lines=0, lines=current)]
            if previous:
                sessions.append(dict(slot='previous', capture='closed', build=15, snapshot_bytes=10000,
                                     excluded_lines=0, lines=previous))
            return json.dumps(dict(schema=1, report_id='a'*32, app_build=15, sessions=sessions)).encode()
        self.schema.bundle(bundle([lead(seq=8), lead(seq=8), lead(seq=12), drop]))
        self.schema.bundle(bundle([lead(seq=42)], [drop]))
        for records in ([lead(seq=42), drop], [drop, lead(seq=42)], [lead(seq=42), unknown(seq=42)]):
            with self.assertRaisesRegex(receiver.Rejected, 'shader_lead_sequence_conflict'):
                self.schema.bundle(bundle(records))

    def test_detail_and_memory_bounds_and_out_of_completion_order_are_explicit(self):
        rows = [unknown(seq=i+1, wait_start_ns=1000000000 + i, wait_end_ns=1100000000 + 2*i) for i in range(23)]
        result = analysis.summarize(list(reversed(rows)))['shader_lead']
        self.assertEqual((result['observations'], len(result['longest_waits']), result['additional_observations']),
                         (23, analysis.MAX_SHADER_LEAD_DETAILS, 3))
        self.assertEqual(result['time_origin'], 'first_observed_shader_wait')
        self.assertEqual(result['longest_waits'][0]['wait_ms'], 100.000022)
        with patch.object(analysis, 'MAX_SPANS', 2):
            with self.assertRaisesRegex(RuntimeError, 'shader lead reporter sequence bound'):
                analysis.summarize(rows[:3])
            with self.assertRaisesRegex(RuntimeError, 'shader lead reporter sequence bound'):
                analysis.summarize([f'IOS_SHADER_LEAD_DROPS schema=1 seq={i+1} busy=0 full=0' for i in range(3)])

    def test_old_frame_records_and_shared_launch_origin_remain_compatible(self):
        old = ('IOS_STALL schema=1 threshold_ns=50000000 count=1 dropped_busy=0 dropped_full=0 '
               'fields=kind,thread,start_ns,end_ns,failed,estimated_end spans=0,1,1100000000,1200000000,0,0')
        before = analysis.summarize([old])
        self.assertEqual(before['shader_lead']['status'], 'unobserved')
        self.assertEqual(before['longest_sampled_frame_gaps'][0]['duration_ms'], 100)
        self.assertIsNone(before['longest_sampled_frame_gaps'][0]['start_ms_from_launch'])
        after = analysis.summarize(['IOS_STAGE sdl-application-entry monotonic=1.000000000', old, lead()])
        self.assertEqual(after['longest_sampled_frame_gaps'][0]['start_ms_from_launch'], 100)
        self.assertEqual(after['longest_sampled_frame_gaps'][0]['sampled_work_overlap_union_ms'], 0)
        # This sparse evidence does not masquerade as an additional work span.
        self.assertEqual(after['sampled_spans'], 1)
        with self.assertRaisesRegex(RuntimeError, 'multiple launch origins'):
            analysis.summarize(['IOS_STAGE sdl-application-entry monotonic=1.000000000',
                                'IOS_STAGE sdl-application-entry monotonic=2.000000000'])

    def test_sparse_only_and_drop_only_logs_are_useful_cli_inputs(self):
        with tempfile.TemporaryDirectory(prefix='shader-lead-diagnostic-') as temporary:
            path = Path(temporary) / 'authored.log'
            for content in (lead(), 'IOS_SHADER_LEAD_DROPS schema=1 seq=80 busy=1 full=0'):
                path.write_text(content + '\n', encoding='utf-8')
                result = subprocess.run([sys.executable, str(ROOT/'scripts/analyze-frame-timing.py'), str(path)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['invalid_records'], 0)
                self.assertEqual(path.read_text(encoding='utf-8'), content + '\n')


if __name__ == '__main__':
    unittest.main()
