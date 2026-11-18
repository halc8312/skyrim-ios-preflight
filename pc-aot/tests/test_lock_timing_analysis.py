"""Lock observations survive private export and remain noncausal in analysis."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


a = load('lock_analysis_test', 'scripts/analyze-frame-timing.py')
r = load('lock_receiver_test', 'scripts/receive_private_diagnostics.py')


def lock_line(events=(), start=1, end=500000000, busy=0, full=0):
    return ('IOS_LOCK schema=1 threshold_ns=16666667 '
            f'count={len(events)} dropped_busy={busy} dropped_full={full} '
            f'window_start_ns={start} window_end_ns={end} totals=2,60000000,1,90000000,0,0 '
            'fields=' + a.LOCK_FIELDS + ' spans=' +
            ';'.join(','.join(map(str, event)) for event in events))


def frame():
    return ('IOS_STALL schema=3 threshold_ns=50000000 count=1 dropped_busy=0 dropped_full=0 '
            'window_start_ns=1 window_end_ns=400000000 '
            'fields=kind,thread,start_ns,end_ns,failed,estimated_end,role,source,result '
            'spans=0,2,100000000,300000000,0,0,0,0,0')


class LockTimingAnalysisTests(unittest.TestCase):
    def test_wait_hold_work_overlap_uses_union_and_owner_is_observation(self):
        events = [(1, 1, 1, 1, 2, 3, 2, 120000000, 220000000, 20000000, 1),
                  (1, 2, 2, 1, 3, 0, 0, 110000000, 230000000, 0, 0),
                  (2, 3, 14, 1, 4, 0, 0, 200000000, 260000000, 0, 0)]
        result = a.summarize([frame(), lock_line(events)])
        gap = result['longest_sampled_frame_gaps'][0]
        self.assertEqual(result['invalid_records'], 0)
        self.assertEqual(result['sampled_lock_spans'], 3)
        self.assertEqual(gap['sampled_work_overlap_union_ms'], 0)
        self.assertEqual(gap['sampled_lock_overlap_union_ms'], 150)
        self.assertEqual(gap['sampled_work_and_lock_overlap_union_ms'], 150)
        wait = next(x for x in gap['lock_overlaps'] if x['phase'] == 'wait')
        self.assertEqual(wait['observed_owner_thread'], 3)
        self.assertEqual(wait['observed_owner_operation'], 'd3d9_present')
        self.assertEqual(wait['observed_thread_cpu_ms'], 20)
        self.assertEqual(wait['thread_relation'], 'same')
        self.assertNotIn('cause', wait)
        self.assertEqual(gap['lock_sampling_status'], 'no_reported_drops')

    def test_subthreshold_work_counts_are_not_fabricated_spans(self):
        result = a.summarize([frame(), lock_line()])
        self.assertEqual(result['lock_observed_totals']['wait_count'], 2)
        self.assertEqual(result['sampled_lock_spans'], 0)
        self.assertEqual(result['longest_sampled_frame_gaps'][0]['lock_overlaps'], [])
        self.assertIn('not every acquisition', ' '.join(result['caveats']))

    def test_completion_windows_and_later_drops_are_not_a_cause(self):
        event = (2, 1, 5, 1, 2, 4, 4, 150000000, 800000000, 0, 0)
        result = a.summarize([frame(), lock_line(start=1, end=400000000),
                              lock_line([event], start=400000000, end=900000000, full=7)])
        gap = result['longest_sampled_frame_gaps'][0]
        self.assertEqual(gap['lock_sampling_status'], 'possible_late_drops')
        self.assertEqual(gap['lock_later_window_dropped_full'], 7)
        self.assertEqual(gap['sampled_lock_overlap_union_ms'], 150)
        self.assertEqual(gap['lock_overlaps'][0]['duration_ms'], 650)
        self.assertIsNone(gap['lock_overlaps'][0]['observed_thread_cpu_ms'])

    def test_no_new_records_preserves_old_log_interpretation(self):
        result = a.summarize([frame()])
        gap = result['longest_sampled_frame_gaps'][0]
        self.assertEqual(result['invalid_records'], 0)
        self.assertEqual(result['lock_records'], 0)
        self.assertEqual(gap['duration_ms'], 200)
        self.assertEqual(gap['lock_sampling_status'], 'not_recorded')

    def test_receiver_accepts_only_complete_numeric_valid_schema(self):
        schema = r.Schema()
        good = lock_line([(3, 1, 9, 1, 2, 3, 11, 100000000, 120000000, 0, 0)])
        schema.line(good)
        for bad in (good + ' path=private', good.replace('schema=1', 'schema=2'),
                    good.replace('count=1 ', 'count=2 '),
                    good.replace('3,1,9,', '3,1,99,'),
                    good.replace('100000000,120000000', '100000000,100000001'),
                    good.replace('100000000,120000000', '120000000,100000000'),
                    good.replace('120000000,0,0', '120000000,7,0'),
                    good.replace('count=1 ', 'count=18446744073709551616 '),
                    good.replace('window_start_ns=1 window_end_ns=500000000', 'window_start_ns=0 window_end_ns=0'),
                    good.replace('spans=', 'spans=secret')):
            with self.subTest(bad=bad[-70:]):
                with self.assertRaises(r.Rejected):
                    schema.line(bad)
                self.assertEqual(a.summarize([bad])['invalid_records'], 1)

    def test_capacity_threshold_and_explicit_drop_counts(self):
        item = (1, 1, 1, 0, 0, 0, 0, 1, 16666668, 0, 0)
        result = a.summarize([lock_line([item] * 16, busy=2, full=5)])
        self.assertEqual((result['sampled_lock_spans'], result['lock_dropped_busy'],
                          result['lock_dropped_full']), (16, 2, 5))
        self.assertEqual(a.summarize([lock_line([item] * 17)])['invalid_records'], 1)

    def test_duplicate_windows_and_restarted_clock_do_not_inflate_totals(self):
        row = lock_line()
        result = a.summarize([row, row])
        self.assertEqual(result['lock_records'], 1)
        self.assertEqual(result['duplicate_lock_records'], 1)
        self.assertEqual(result['lock_observed_totals']['wait_count'], 2)
        with self.assertRaisesRegex(RuntimeError, 'clock moved backwards'):
            a.summarize([row, lock_line(end=400000000)])


if __name__ == '__main__':
    unittest.main()
