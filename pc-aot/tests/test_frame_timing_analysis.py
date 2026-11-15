"""Authored numeric logs and actual public recorder output, no device inputs."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('frame_analysis',ROOT/'scripts/analyze-frame-timing.py')
analyzer=importlib.util.module_from_spec(spec);spec.loader.exec_module(analyzer)

def timing(kind='frame_gap',end=5000000000,bins=None):
    bins=bins or [0]*20
    return ('IOS_TIMING schema=1 approximate_window=1 '+f'end_ns={end} window_ns=5000000000 '
            f'kind={kind} samples={sum(bins)} total_ns=123000000 max_ns=100000000 failures_known=0 '
            'failures=0 sleep_calls=0 bins='+','.join(map(str,bins)))

def stalls(items,busy=0,full=0):
    return ('IOS_STALL schema=1 threshold_ns=50000000 '+f'count={len(items)} dropped_busy={busy} '
            f'dropped_full={full} fields=kind,thread,start_ns,end_ns,failed,estimated_end spans='+
            ';'.join(','.join(map(str,item)) for item in items))

def stalls_v3(items, start, end, busy=0, full=0):
    return stalls(items,busy,full).replace('schema=1', 'schema=3').replace(
        ' fields=',f' window_start_ns={start} window_end_ns={end} fields=').replace(
        'estimated_end spans=', 'estimated_end,role,source,result spans=')

class FrameTimingAnalysisTests(unittest.TestCase):
    def test_public_schema_matches_real_recorder(self):
        source=(ROOT/'ios/runtime-port/performance_metrics.c').read_text()
        bounds=source.split('static const uint64_t upper_ns[SKYRIM_PERF_BUCKET_COUNT] = {',1)[1].split('};',1)[0]
        self.assertEqual(tuple(analyzer.U64 if s.strip()=='UINT64_MAX' else int(s) for s in bounds.split(',')),analyzer.UPPER)
        names=source.split('static const char *const names[SKYRIM_PERF_KIND_COUNT] = {',1)[1].split('};',1)[0]
        self.assertEqual(tuple(re.findall(r'"([a-z0-9_]+)"',names)),analyzer.KINDS)

    def test_percentiles_are_ranges_and_threshold_counts_exclude_exact_boundary(self):
        bins=[0]*20;bins[0]=1;bins[11]=89;bins[12]=5;bins[13]=3;bins[15]=1;bins[19]=1
        result=analyzer.summarize([timing(bins=bins)])['kinds']['frame_gap']
        self.assertEqual(result['samples'],100)
        self.assertEqual(result['p95'],dict(lower_exclusive_ms=50,upper_inclusive_ms=66.666667))
        self.assertEqual(result['p99'],dict(lower_exclusive_ms=166.666667,upper_inclusive_ms=250))
        self.assertEqual((result['over_50ms'],result['over_100ms'],result['over_250ms']),(10,2,1))
        self.assertIsNone(result['failures'])
        self.assertIsNone(analyzer.percentile([0]*20,99))
        self.assertIsNone(analyzer.percentile([0]*19+[1],99)['upper_inclusive_ms'])

    def test_overlaps_keep_threads_estimates_and_incompleteness_without_summing(self):
        spans=[(0,1,100000000,300000000,0,0), (4,1,150000000,280000000,0,0),
               (9,2,200000000,400000000,0,0), (13,0,260000000,360000000,1,1),
               (6,1,400000000,500000000,0,0)]
        r=analyzer.summarize([stalls(spans,3,5)])
        gap=r['longest_sampled_frame_gaps'][0]
        self.assertEqual(gap['duration_ms'],200)
        self.assertEqual([s['overlap_ms'] for s in gap['overlaps']],[130,100,40])
        self.assertEqual([s['thread_relation'] for s in gap['overlaps']],['same','different','unknown'])
        self.assertTrue(gap['overlaps'][-1]['estimated_endpoint'])
        self.assertTrue(gap['overlaps'][-1]['failed'])
        self.assertEqual((r['sampled_spans'],r['dropped_busy'],r['dropped_full']),(5,3,5))
        self.assertNotIn('cause',gap)
        self.assertEqual(gap['sampled_work_overlap_union_ms'],150)
        self.assertEqual(gap['frame_interval_without_sampled_work_ms'],50)
        self.assertEqual(gap['sampling_status'],'drops_reported')

    def test_new_sampling_window_reports_drops_without_fabricating_a_cause(self):
        frame = (0,1,100000000,300000000,0,0,0,0,0)
        compile = (4,2,150000000,280000000,0,0,1,0,0)
        nested = (33,2,160000000,270000000,0,0,1,0,0)
        # The missing work is recorded in a neighboring report whose interval
        # overlaps the frame. Later drops are kept separate because a long
        # operation could finish later while still overlapping this frame.
        rows=[stalls_v3([frame,compile,nested],50000000,250000000),
              stalls_v3([],250000000,400000000,full=2),
              stalls_v3([],500000000,700000000,busy=3)]
        result=analyzer.summarize(rows);gap=result['longest_sampled_frame_gaps'][0]
        self.assertEqual((result['invalid_records'],result['sampling_windows_with_drops']),(0,2))
        self.assertEqual((gap['window_dropped_busy'],gap['window_dropped_full']),(0,2))
        self.assertEqual(gap['later_window_dropped_busy'],3)
        self.assertEqual(gap['sampled_work_overlap_union_ms'],130)
        self.assertEqual(gap['frame_interval_without_sampled_work_ms'],70)
        self.assertEqual(gap['sampling_status'],'drops_reported')
        self.assertIsNone(gap['start_ms_from_first_timing_window'])
        self.assertEqual(analyzer.summarize(rows[:1])['longest_sampled_frame_gaps'][0]['sampling_status'],
                         'no_reported_drops')
        late=analyzer.summarize([rows[0],rows[2]])['longest_sampled_frame_gaps'][0]
        self.assertEqual(late['sampling_status'],'possible_late_drops')
        self.assertEqual((late['window_dropped_busy'],late['later_window_dropped_busy']),(0,3))
        malformed=rows[0].replace('window_start_ns=50000000','window_start_ns=300000000')
        self.assertEqual(analyzer.summarize([malformed])['invalid_records'],1)

    def test_old_sampling_windows_can_be_inferred_without_losing_unknown_coverage(self):
        frame=(0,1,100000000,300000000,0,0)
        unknown=analyzer.summarize([stalls([frame])])['longest_sampled_frame_gaps'][0]
        self.assertEqual(unknown['sampling_status'],'window_unknown')
        result=analyzer.summarize([timing(),stalls([frame],full=1)])
        gap=result['longest_sampled_frame_gaps'][0]
        self.assertEqual(gap['sampling_status'],'drops_reported')
        self.assertEqual(gap['start_ms_from_first_timing_window'],100)

    def test_v3_capacity_and_new_kinds_keep_old_contracts(self):
        frames=[(0,1,1,60000001,0,0,0,0,0)]*8
        work=[(34,1,1,60000001,0,0,0,0,0)]*32
        row=stalls_v3(frames+work,0,70000000)
        self.assertEqual(len(analyzer.stalls(row.split('IOS_STALL ',1)[1])[0]),40)
        self.assertEqual(analyzer.summarize([row])['invalid_records'],0)
        for invalid in (stalls_v3(frames+[frames[0]],0,70000000),
                        stalls_v3(work+[work[0]],0,70000000)):
            self.assertEqual(analyzer.summarize([invalid])['invalid_records'],1)
        # Old versions retain their original 4/12 limits.
        old=stalls([(0,1,1,60000001,0,0)]*5)
        self.assertEqual(analyzer.summarize([old])['invalid_records'],1)

    def test_malformed_unknown_duplicate_and_cross_launch_inputs_are_not_success_samples(self):
        valid=timing(bins=[1]+[0]*19)
        invalid=[valid.replace('schema=1','schema=2'),valid.replace('samples=1','samples=2'),
                 valid+' schema=1',valid.replace('kind=frame_gap','kind=game_private_name'),
                 valid.replace('max_ns=100000000','max_ns=-1'),
                 valid.replace('max_ns=100000000','max_ns=18446744073709551616'),
                 stalls([(99,1,1,60000000,0,0)]),stalls([(0,1,1,10,0,0)]),
                 stalls([(0,1,1,60000000,0,0)]).replace('count=1','count=2')]
        r=analyzer.summarize(['private path /Game/secret.ess',valid,valid,*invalid])
        self.assertEqual(r['timing_records'],1);self.assertEqual(r['invalid_records'],len(invalid))
        self.assertEqual(r['duplicate_timing_records'],1)
        self.assertNotIn('secret',json.dumps(r));self.assertNotIn('game_private_name',json.dumps(r))
        with self.assertRaises(RuntimeError):analyzer.summarize([valid,timing(end=10)])
        self.assertEqual(analyzer.summarize([stalls([],1,2)])['dropped_full'],2)

    def test_context_partitions_and_v1_v2_spans_are_not_double_counted(self):
        bins=[0]*20;bins[14]=2
        root=timing('mvk_msl_wait',bins=bins)
        part=root.replace('IOS_TIMING ','IOS_TIMING_CONTEXT ')+' role=1 source=1 result=0'
        old=stalls([(0,1,100000000,300000000,0,0)])
        new=stalls([(21,2,100000000,300000000,0,0,2,3,0)]).replace('schema=1','schema=2').replace('estimated_end spans=','estimated_end,role,source,result spans=')
        r=analyzer.summarize([root,part,part,old,new])
        self.assertEqual(r['invalid_records'],0)
        self.assertEqual(r['kinds']['mvk_msl_wait']['samples'],2)
        self.assertEqual(len(r['work_contexts']),1)
        self.assertEqual(r['work_contexts'][0]['over_100ms'],2)
        self.assertEqual(r['duplicate_context_records'],1)
        self.assertIsNone(r['work_contexts'][0]['failures'])
        span=r['longest_sampled_frame_gaps'][0]['overlaps'][0]
        self.assertEqual((span['role'],span['source'],span['result']),(2,3,0))
        for bad in (part.replace('role=1','role=99'),new.replace(',2,3,0',',2,9,0')):
            self.assertEqual(analyzer.summarize([bad])['invalid_records'],1)

    def test_pipeline_role_contexts_reject_source_and_wait_result_tags(self):
        for kind in ('graphics_pipeline', 'compute_pipeline', 'mvk_convert'):
            row = timing(kind, bins=[1]+[0]*19).replace('IOS_TIMING ', 'IOS_TIMING_CONTEXT ')
            row += ' role=1 source=0 result=0'
            self.assertEqual(analyzer.summarize([row])['invalid_records'], 0)
            for bad in (row.replace('source=0', 'source=1'), row.replace('result=0', 'result=1')):
                self.assertEqual(analyzer.summarize([bad])['invalid_records'], 1)

    def test_cli_bounds_and_read_only_output(self):
        with tempfile.TemporaryDirectory(prefix='frame-analysis-') as tmp:
            path=Path(tmp)/'authored.log';content=timing(bins=[1]+[0]*19)+'\n'
            path.write_text(content)
            r=analyzer.summarize(analyzer.bounded_lines(path));self.assertEqual(r['timing_records'],1)
            self.assertEqual(path.read_text(),content)
            path.write_bytes(b'x'*(analyzer.MAX_LINE+1))
            with self.assertRaises(RuntimeError):list(analyzer.bounded_lines(path))

    def test_actual_c_recorder_outputs_are_accepted(self):
        cc=shutil.which(os.environ.get('CC','cc'))
        if not cc:self.skipTest('POSIX host C compiler required')
        with tempfile.TemporaryDirectory(prefix='frame-recorder-') as tmp:
            work=Path(tmp);source=work/'emit.c';binary=work/'emit'
            source.write_text('''#include "performance_metrics.h"
int main(int argc,char **argv){
 uint64_t now=skyrim_perf_now_ns();
 (void)argv;
 if(argc>1){
  for(unsigned i=0;i<8;++i)
   skyrim_perf_interval(SKYRIM_PERF_FRAME_GAP,now-200000000+i,now,0,0);
  for(unsigned i=0;i<32;++i)
   skyrim_perf_interval(SKYRIM_PERF_PACER_RUNLOOP,now-150000000+i,now,0,0);
  skyrim_perf_log_window();return 0;
 }
 skyrim_perf_interval(SKYRIM_PERF_FRAME_GAP,now-200000000,now,0,0);
 skyrim_perf_interval(SKYRIM_PERF_GRAPHICS_PIPELINE,now-150000000,now-10000000,0,0);
 for(unsigned k=SKYRIM_PERF_MVK_MSL_WAIT;k<=SKYRIM_PERF_MVK_DISPATCH_BLOCK;++k)
  skyrim_perf_interval(k,now-80000000,now,0,0);
 for(unsigned role=0;role<3;++role){
  SkyrimPerfContext old=skyrim_perf_context_set((SkyrimPerfContext){role,3,2});
  skyrim_perf_interval(SKYRIM_PERF_GRAPHICS_PIPELINE,now-20000000,now,0,0);
  skyrim_perf_interval(SKYRIM_PERF_COMPUTE_PIPELINE,now-20000000,now,role==2,0);
  skyrim_perf_context_set(old);
 }
 skyrim_perf_log_window();return 0;
}''')
            subprocess.run([cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-O2',
                '-I',str(ROOT/'ios/runtime-port'),str(source),str(ROOT/'ios/runtime-port/performance_metrics.c'),
                '-o',str(binary)],check=True,timeout=45)
            result=subprocess.run([str(binary)],capture_output=True,text=True,check=True,timeout=5)
            r=analyzer.summarize(result.stderr.splitlines())
            self.assertEqual((r['timing_records'],r['stall_records'],r['invalid_records']),(9,1,0))
            self.assertEqual(r['kinds']['frame_gap']['observed_max_ms'],200)
            self.assertEqual(r['longest_sampled_frame_gaps'][0]['overlaps'][0]['overlap_ms'],140)
            contexts = {(c['kind'], c['role']): c for c in r['work_contexts']}
            for kind, expected_samples in (('graphics_pipeline', 4), ('compute_pipeline', 3)):
                parts = [contexts[kind, role] for role in range(3)]
                self.assertEqual(r['kinds'][kind]['samples'], expected_samples)
                self.assertEqual(sum(p['samples'] for p in parts), expected_samples)
                self.assertTrue(all(p['source'] == p['result'] == 0 for p in parts))
                self.assertEqual(sum(p['total_ns'] for p in parts), 200000000 if kind == 'graphics_pipeline' else 60000000)
                self.assertEqual(sum(p['failures'] for p in parts), r['kinds'][kind]['failures'])
            self.assertEqual(contexts['graphics_pipeline', 0]['over_100ms'], 1)
            self.assertEqual(contexts['graphics_pipeline', 1]['over_100ms'], 0)
            self.assertEqual(contexts['compute_pipeline', 2]['failures'], 1)
            for kind in analyzer.KINDS[21:27]:
                self.assertEqual(r['kinds'][kind]['observed_max_ms'],80)
                # NSError may be a warning; these observations do not classify
                # the original API's failure/success or driver cache hits.
                self.assertIsNone(r['kinds'][kind]['failures'])
            burst=subprocess.run([str(binary),'burst'],capture_output=True,text=True,check=True,timeout=5)
            r=analyzer.summarize(burst.stderr.splitlines())
            self.assertEqual((r['invalid_records'],r['sampled_spans'],r['dropped_full']),(0,40,0))
            self.assertEqual(len(r['longest_sampled_frame_gaps']),8)
            self.assertEqual(r['kinds']['pacer_runloop']['samples'],32)
            stall_line=next(line for line in burst.stderr.splitlines() if line.startswith('IOS_STALL '))
            self.assertIn('schema=3 ',stall_line)
            self.assertLess(len(stall_line),8192)

    def test_save_window_is_not_attributed_to_the_closing_thread(self):
        frame = (0, 1, 100000000, 300000000, 0, 0)
        window = (analyzer.KINDS.index('save_candidate_window'), 1, 50000000, 310000000, 0, 0)
        write = (analyzer.KINDS.index('save_candidate_write'), 1, 180000000, 240000000, 1, 0)
        result = analyzer.summarize([stalls([frame, window, write])])
        overlaps = result['longest_sampled_frame_gaps'][0]['overlaps']
        self.assertEqual([x['thread_relation'] for x in overlaps], ['handle_lifetime', 'same'])
        self.assertIn('not proof of autosave', ' '.join(result['caveats']))

if __name__=='__main__':unittest.main()
