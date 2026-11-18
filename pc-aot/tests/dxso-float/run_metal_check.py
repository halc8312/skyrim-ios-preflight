"""Bounded real Apple GPU arithmetic check, invoked only in an authorized Mac run."""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import struct


def verify_report(result):
    if result.get('schema') != 1 or type(result.get('schema')) is not int or \
            result.get('status') != 'passed' or type(result.get('case_count')) is not int or \
            result['case_count'] != 20 or result.get('metal_execution_verified') is not True or \
            result.get('public_only') is not True or result.get('runtime_inputs') is not True or \
            result.get('physical_iphone_verified') is not False or \
            result.get('game_rendering_verified') is not False:
        raise ValueError('Incomplete or invalid Metal arithmetic result')
    maximum = struct.unpack('<f', struct.pack('<I', 0x7f7fffff))[0]
    expected = {
        'rcp-zero': [maximum]*4, 'rsq-zero': [maximum]*4,
        'nrm-zero': [0]*4, 'log-zero': [-maximum]*4, 'pow-zero': [1]*4,
        'rcp-normal': [.5]*4, 'rsq-normal': [.5]*4,
        'nrm-normal': [.6,.8,0,0], 'log-normal': [2]*4, 'pow-normal': [8]*4,
    }
    cases = result.get('cases')
    if not isinstance(cases, list) or len(cases) != 20:
        raise ValueError('Missing arithmetic cases')
    seen = set()
    for case in cases:
        name, mode = case.get('case'), case.get('math_mode')
        key = (name, mode)
        if name not in {'float-'+s for s in expected} or mode not in ('safe','fast') or key in seen:
            raise ValueError('Unknown or repeated arithmetic case')
        seen.add(key)
        lanes = case.get('lanes')
        if case.get('matches') is not True or not isinstance(lanes,list) or len(lanes) != 4:
            raise ValueError('Incomplete arithmetic lanes')
        for lane, wanted in zip(lanes, expected[name.removeprefix('float-')]):
            bits = lane.get('bits')
            if type(bits) is not int or not 0 <= bits <= 0xffffffff or \
                    lane.get('finite') is not True or lane.get('matches') is not True:
                raise ValueError('Invalid arithmetic lane')
            value = struct.unpack('<f', struct.pack('<I',bits))[0]
            tolerance = 0 if wanted == 0 or abs(wanted) == maximum else max(1,abs(wanted))*1e-5
            if not math.isfinite(value) or abs(value-wanted) > tolerance:
                raise ValueError('Arithmetic readback does not match the independent expected value')


def run(fixtures, output):
    if platform.system() != 'Darwin':
        raise SystemExit('Apple SDK and Metal device required; no verification performed')
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('IPHONEOS_DEPLOYMENT_TARGET', None)
    env.pop('SDKROOT', None)
    binary = output / 'metal-arithmetic'
    compile_result = subprocess.run(['xcrun', '--sdk', 'macosx', 'clang++', '-std=c++17',
        '-O2', '-fobjc-arc', '-fno-fast-math', '-mmacosx-version-min=14.0',
        '-framework', 'Foundation', '-framework', 'Metal',
        str(Path(__file__).with_name('metal_check.mm')), '-o', str(binary)],
        env=env, capture_output=True, text=True, timeout=90)
    (output/'compile.log').write_text(compile_result.stdout+compile_result.stderr)
    if compile_result.returncode:
        raise SystemExit(compile_result.stderr[-6000:])
    try:
        checked = subprocess.run([str(binary.resolve()), str(fixtures.resolve())],
            env=env, capture_output=True, text=True, timeout=150)
    except subprocess.TimeoutExpired:
        (output/'result.json').write_text(json.dumps({'schema':1,'status':'timeout',
            'metal_execution_verified':False})+'\n')
        raise SystemExit('Authored Metal arithmetic check timed out; no success')
    (output/'run.log').write_text(checked.stderr)
    (output/'result.json').write_text(checked.stdout)
    print(checked.stdout)
    if checked.returncode:
        raise SystemExit('Authored Metal arithmetic check failed or was unavailable; see its result/log')
    result = json.loads(checked.stdout)
    verify_report(result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixtures', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    run(args.fixtures, args.output)
