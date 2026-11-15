#!/usr/bin/env python3
"""Summarize one launch's numeric host timing log locally, without copying raw lines.

Histograms are approximate, slow spans are sampled, and overlap is not causation.
No game names, addresses, paths from the log, or shader contents are emitted.
"""
import argparse
import json
from pathlib import Path
import re
import sys

KINDS = ('frame_gap', 'present_host', 'pacer', 'hlsl', 'graphics_pipeline',
         'compute_pipeline', 'shader_module', 'graphics_compile_wait',
         'compute_compile_wait', 'read_file', 'read_file_ex', 'd3d_shader_create',
         'texture_load', 'xwma_decode', 'resource_wait', 'd3d_shader_wait',
         'image_acquire', 'queue_present', 'queue_submit', 'fence_wait', 'submission_wait',
         'mvk_msl_wait', 'mvk_specialize_wait', 'mvk_pipeline_wait', 'mvk_other_wait',
         'mvk_dispatch_wait', 'mvk_dispatch_block', 'write_file', 'write_file_ex',
         'save_candidate_write', 'save_candidate_write_ex', 'save_candidate_window', 'mvk_cold_lock_wait',
         'mvk_convert', 'pacer_runloop', 'pacer_sleep', 'ui_pump', 'pacer_overrun')
U64 = (1 << 64) - 1
UPPER = (0, 1000000, 2000000, 4000000, 8000000, 12000000, 16666667,
         20000000, 25000000, 33333334, 40000000, 50000000, 66666667,
         100000000, 166666667, 250000000, 500000000, 1000000000, 2000000000, U64)
THRESHOLD = 50000000
MAX_BYTES = 256 * 1024 * 1024
MAX_SPANS = 100000
MAX_LINE = 1024 * 1024


def unsigned(value):
    if not re.fullmatch(r'[0-9]{1,20}', value):
        raise ValueError('invalid integer')
    result = int(value)
    if result > U64:
        raise ValueError('integer overflow')
    return result


def fields(text, expected):
    result = {}
    for token in text.strip().split():
        key, separator, value = token.partition('=')
        if not separator or key in result or key not in expected:
            raise ValueError('invalid fields')
        result[key] = value
    if result.keys() != expected:
        raise ValueError('missing fields')
    return result


def timing(text):
    f = fields(text, set(('schema', 'approximate_window', 'end_ns', 'window_ns',
                        'kind', 'samples', 'total_ns', 'max_ns', 'failures_known',
                        'failures', 'sleep_calls', 'bins')))
    if f['schema'] != '1' or f['approximate_window'] != '1' or f['kind'] not in KINDS:
        raise ValueError('unknown schema')
    result = {k: unsigned(v) for k, v in f.items() if k not in ('bins', 'kind')}
    result['kind'] = f['kind']
    result['bins'] = [unsigned(v) for v in f['bins'].split(',')]
    if len(result['bins']) != 20 or sum(result['bins']) != result['samples']:
        raise ValueError('histogram mismatch')
    if result['failures_known'] > 1 or not result['end_ns']:
        raise ValueError('invalid metadata')
    return result


def stalls(text, include_window=False):
    expected = set(('schema', 'threshold_ns', 'count', 'dropped_busy',
                    'dropped_full', 'fields', 'spans'))
    if re.search(r'(?:^|\s)schema=3(?:\s|$)', text):
        expected.update(('window_start_ns', 'window_end_ns'))
    f = fields(text, expected)
    schema = unsigned(f['schema'])
    layout = 'kind,thread,start_ns,end_ns,failed,estimated_end'
    if schema in (2,3): layout += ',role,source,result'
    if schema not in (1,2,3) or unsigned(f['threshold_ns']) != THRESHOLD or f['fields'] != layout:
        raise ValueError('unknown schema')
    window = None
    if schema == 3:
        start, end = unsigned(f['window_start_ns']), unsigned(f['window_end_ns'])
        if start > end: raise ValueError('invalid stall window')
        if end: window = dict(start=start, end=end, inferred=False)
    frame_capacity, work_capacity = (8,32) if schema == 3 else (4,12)
    count = unsigned(f['count'])
    if count > frame_capacity + work_capacity:
        raise ValueError('too many spans')
    result = []
    for record in f['spans'].split(';') if f['spans'] else []:
        v = [unsigned(item) for item in record.split(',')]
        if len(v) != (6 if schema == 1 else 9):
            raise ValueError('invalid span')
        kind, thread, start, end, failed, estimated = v[:6]
        role, source, wait_result = v[6:] if schema in (2,3) else (0,0,0)
        if role > 2 or source > 3 or wait_result > 2:
            raise ValueError('invalid work context')
        if kind >= len(KINDS) or not start or end < start or end-start < THRESHOLD or failed > 1 or estimated > 1:
            raise ValueError('invalid span')
        result.append(dict(kind=KINDS[kind], thread=thread, start=start, end=end,
                           failed=failed, estimated_end=estimated, role=role, source=source, result=wait_result))
    if len(result) != count or sum(s['kind'] == 'frame_gap' for s in result) > frame_capacity or \
            sum(s['kind'] != 'frame_gap' for s in result) > work_capacity:
        raise ValueError('span count mismatch')
    output = (result, unsigned(f['dropped_busy']), unsigned(f['dropped_full']))
    return (*output, window) if include_window else output


def percentile(bins, percent):
    total = sum(bins)
    if not total:
        return None
    rank = (total * percent + 99) // 100
    accumulated = 0
    for i, count in enumerate(bins):
        accumulated += count
        if accumulated >= rank:
            return {'lower_exclusive_ms': UPPER[i-1]/1e6 if i else None,
                    'upper_inclusive_ms': UPPER[i]/1e6 if UPPER[i] != U64 else None}
    raise AssertionError('rank')


def interval_union_ns(intervals):
    """Wall-time union only: nested/cross-thread work is never added twice."""
    total = 0
    previous_end = 0
    for start, end in sorted(intervals):
        if end > max(start, previous_end):
            total += end - max(start, previous_end)
        previous_end = max(previous_end, end)
    return total


def summarize(lines):
    aggregate = {}
    context_rows = []
    spans = []
    sampling_windows = []
    current_window = None
    timing_origin = None
    invalid = duplicate = dropped_busy = dropped_full = timing_records = stall_records = 0
    last_end = 0
    current_kinds = set()
    for line in lines:
        if 'IOS_TIMING_CONTEXT ' in line:
            try:
                raw = line.split('IOS_TIMING_CONTEXT ',1)[1]
                base, separator, suffix = raw.partition(' role=')
                if not separator: raise ValueError('missing context')
                c = {k:unsigned(v) for k,v in fields('role='+suffix, {'role','source','result'}).items()}
                if c['role']>2 or c['source']>3 or c['result']>2: raise ValueError('invalid context')
                row = timing(base)
                role_only = row['kind'] in ('mvk_cold_lock_wait','graphics_pipeline','compute_pipeline','mvk_convert')
                if row['kind'] not in ('mvk_msl_wait','graphics_compile_wait','compute_compile_wait') and not role_only:
                    raise ValueError('unexpected context kind')
                if (row['kind']=='mvk_msl_wait' and c['result']) or (row['kind']!='mvk_msl_wait' and c['source']) or (role_only and c['result']):
                    raise ValueError('noncanonical context')
                if len(context_rows) >= MAX_SPANS: raise RuntimeError('context record bound exceeded')
                context_rows.append((row,c))
            except ValueError:
                invalid += 1
            continue
        marker = 'IOS_TIMING ' if 'IOS_TIMING ' in line else 'IOS_STALL ' if 'IOS_STALL ' in line else None
        if marker is None:
            continue
        try:
            tail = line.split(marker, 1)[1]
            if marker == 'IOS_TIMING ':
                row = timing(tail)
                if row['end_ns'] < last_end:
                    raise RuntimeError('timing clock moved backwards; use one unmodified launch log')
                if row['end_ns'] != last_end:
                    current_kinds = set()
                    last_end = row['end_ns']
                    current_window = dict(start=max(0, last_end-row['window_ns']),
                                          end=last_end, inferred=True)
                    if timing_origin is None: timing_origin = current_window['start']
                if row['kind'] in current_kinds:
                    duplicate += 1
                    continue
                current_kinds.add(row['kind'])
                a = aggregate.setdefault(row['kind'], dict(bins=[0]*20, total_ns=0, max_ns=0,
                                            failures=0, failures_known=True, sleep_calls=0, records=0))
                a['bins'] = [x+y for x,y in zip(a['bins'],row['bins'])]
                for key in ('total_ns','failures','sleep_calls'):
                    a[key] += row[key]
                a['max_ns'] = max(a['max_ns'],row['max_ns'])
                a['failures_known'] &= bool(row['failures_known'])
                a['records'] += 1
                timing_records += 1
            else:
                samples, busy, full, window = stalls(tail, include_window=True)
                if len(spans)+len(samples) > MAX_SPANS:
                    raise RuntimeError('span bound exceeded; use a shorter single-launch log')
                if len(sampling_windows) >= MAX_SPANS:
                    raise RuntimeError('stall window bound exceeded')
                # Older versions have no explicit sampling window. The nearest
                # preceding histogram report is a conservative approximation;
                # standalone old stall records retain an unknown window.
                for sample in samples: sample['sampling_record'] = len(sampling_windows)
                sampling_windows.append(dict(window=window or current_window,
                                             dropped_busy=busy, dropped_full=full))
                spans.extend(samples)
                dropped_busy += busy
                dropped_full += full
                stall_records += 1
        except ValueError:
            invalid += 1
    kinds = {}
    for kind in KINDS:
        if kind not in aggregate:
            continue
        a = aggregate[kind]
        count = sum(a['bins'])
        kinds[kind] = dict(samples=count, records=a['records'],
            approximate_mean_ms=a['total_ns']/count/1e6 if count else None,
            observed_max_ms=a['max_ns']/1e6, p50=percentile(a['bins'],50),
            p95=percentile(a['bins'],95), p99=percentile(a['bins'],99),
            over_50ms=sum(a['bins'][12:]), over_100ms=sum(a['bins'][14:]),
            over_250ms=sum(a['bins'][16:]), failures=a['failures'] if a['failures_known'] else None,
            sleep_calls=a['sleep_calls'])
    frames = sorted((s for s in spans if s['kind']=='frame_gap'),
                    key=lambda s:s['end']-s['start'], reverse=True)[:20]
    origin = min((s['start'] for s in spans),default=0)
    correlations = []
    for f in frames:
        overlap = []
        covered = []
        for s in spans:
            if s['kind'] == 'frame_gap':
                continue
            common = min(f['end'],s['end'])-max(f['start'],s['start'])
            if common <= 0:
                continue
            covered.append((max(f['start'],s['start']), min(f['end'],s['end'])))
            relation = 'unknown' if not f['thread'] or not s['thread'] else \
                       'same' if f['thread']==s['thread'] else 'different'
            if s['kind'] == 'save_candidate_window':
                relation = 'handle_lifetime'
            overlap.append(dict(kind=s['kind'],thread_relation=relation, overlap_ms=common/1e6,
                duration_ms=(s['end']-s['start'])/1e6, estimated_endpoint=bool(s['estimated_end']),
                failed=bool(s['failed']), role=s['role'], source=s['source'], result=s['result']))
        overlap.sort(key=lambda s:s['overlap_ms'], reverse=True)
        source_record = sampling_windows[f['sampling_record']]
        relevant_drops = []
        later_drops = []
        for index, record in enumerate(sampling_windows):
            if not (record['dropped_busy'] or record['dropped_full']): continue
            w = record['window']
            if index == f['sampling_record'] or w is None or \
                    (w['start'] <= f['end'] and w['end'] >= f['start']):
                relevant_drops.append(record)
            elif w['end'] >= f['end']:
                # Work is recorded when it ends. A long overlapping operation
                # can be dropped in a later completion window, even if that
                # window itself no longer intersects this frame.
                later_drops.append(record)
        union = interval_union_ns(covered)
        correlations.append(dict(start_ms_from_first_sample=(f['start']-origin)/1e6,
            start_ms_from_first_timing_window=(f['start']-timing_origin)/1e6 if timing_origin is not None else None,
            duration_ms=(f['end']-f['start'])/1e6, estimated_endpoint=bool(f['estimated_end']),
            overlaps=overlap[:12], additional_overlaps=max(0,len(overlap)-12),
            sampled_work_overlap_union_ms=union/1e6,
            frame_interval_without_sampled_work_ms=(f['end']-f['start']-union)/1e6,
            sampling_status='drops_reported' if relevant_drops else
                'possible_late_drops' if later_drops else
                'no_reported_drops' if source_record['window'] else 'window_unknown',
            window_dropped_busy=sum(r['dropped_busy'] for r in relevant_drops),
            window_dropped_full=sum(r['dropped_full'] for r in relevant_drops),
            later_window_dropped_busy=sum(r['dropped_busy'] for r in later_drops),
            later_window_dropped_full=sum(r['dropped_full'] for r in later_drops)))
    contexts = {}
    seen_contexts = set()
    context_duplicates = 0
    context_last_end = 0
    for row,c in context_rows:
        if row['end_ns'] < context_last_end:
            raise RuntimeError('context clock moved backwards; use one unmodified launch log')
        context_last_end = row['end_ns']
        key = (row['kind'], c['role'], c['source'], c['result'])
        identity = (row['end_ns'], *key)
        if identity in seen_contexts:
            context_duplicates += 1
            continue
        seen_contexts.add(identity)
        a = contexts.setdefault(key, dict(kind=key[0], **c, samples=0, over_100ms=0, total_ns=0, max_ns=0, failures=0))
        a['samples'] += row['samples']
        a['over_100ms'] += sum(row['bins'][14:])
        a['total_ns'] += row['total_ns']
        a['max_ns'] = max(a['max_ns'], row['max_ns'])
        a['failures'] = a['failures'] + row['failures'] if row['failures_known'] and a['failures'] is not None else None
    return dict(schema=1, scope='one launch; host wall time; not GPU time or a causal attribution',
        timing_records=timing_records, stall_records=stall_records, invalid_records=invalid,
        work_contexts=list(contexts.values()), duplicate_context_records=context_duplicates,
        duplicate_timing_records=duplicate, sampled_spans=len(spans), dropped_busy=dropped_busy,
        dropped_full=dropped_full, kinds=kinds, longest_sampled_frame_gaps=correlations,
        sampling_windows_with_drops=sum(bool(r['dropped_busy'] or r['dropped_full']) for r in sampling_windows),
        caveats=['Histogram windows may straddle concurrent updates; percentiles are bucket ranges.',
                 'Slow spans are incomplete samples; absence of overlap does not exclude a cause.',
                 'Overlapping or nested durations must not be added together.',
                 'Work-overlap union is across recorded threads, not causal attribution or GPU/CPU time. Uncovered time is not proof of guest CPU work.',
                 'Sampling windows are approximate completion windows; unknown or dropped windows cannot establish absence of work. No reported drops still excludes operations below the sampling threshold.',
                 'A later completion-window drop may hide a long operation that overlapped an earlier frame; possible_late_drops is uncertainty, not an attributed cause.',
                 'Context histograms partition existing timings; they are not additional work. Role identifies the requester, not the owner of a contended lock.',
                 'Save candidates are filename-classified, not proof of autosave; their handle windows include idle time and exclude earlier serialization and later durability.',
                 'Use a separate log for each launch; anonymous thread ordinals restart per process.'])


def bounded_lines(path):
    with path.open('rb') as source:
        consumed = 0
        while True:
            line = source.readline(MAX_LINE+1)
            if not line:
                return
            consumed += len(line)
            if len(line)>MAX_LINE or consumed>MAX_BYTES:
                raise RuntimeError('input exceeds local analysis bound')
            yield line.decode('utf-8',errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log',type=Path,help='one locally collected launch log; read only')
    args = parser.parse_args()
    try:
        result = summarize(bounded_lines(args.log))
    except (OSError,RuntimeError) as error:
        # Do not echo raw lines or file names in generated reports.
        parser.exit(2, 'Unable to analyze this local log: '+
                    (str(error) if isinstance(error,RuntimeError) else 'file could not be read')+'\n')
    print(json.dumps(result,indent=2,allow_nan=False))
    return 0 if result['timing_records'] or result['stall_records'] else 1


if __name__ == '__main__':
    sys.exit(main())
