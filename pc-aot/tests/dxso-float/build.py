"""Build only the public shader-translator fixture for the local host, not an app."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
def build(output):
    source=ROOT/'vendor/skydream/third_party/dxvk'
    if platform.system() not in ('Darwin','Linux'):raise SystemExit('Host Linux/Mac compiler required')
    cc=shutil.which(os.environ.get('CXX','c++'))
    if not cc:raise SystemExit('C++ compiler required')
    output.mkdir(parents=True,exist_ok=False)
    deadline=time.monotonic()+300
    flags=[cc,'-std=c++17','-O1','-ffunction-sections','-fdata-sections',
        '-Wno-deprecated-declarations','-include','cstdint','-include','cstring',
        '-DDXVK_NATIVE','-DNOMINMAX','-D_WIN32_WINNT=0xa00']
    for folder in ('src','src/util','include','include/native','include/native/windows','include/native/directx'):
        flags+=['-I',str(source/folder)]
    units=sorted((source/'src/dxso').glob('*.cpp'))
    units+=[source/'src'/name for name in ('d3d9/d3d9_fixed_function.cpp','dxvk/dxvk_shader.cpp',
        'dxvk/dxvk_shader_key.cpp','dxvk/dxvk_spec_const.cpp','spirv/spirv_module.cpp',
        'spirv/spirv_code_buffer.cpp','spirv/spirv_compression.cpp','util/log/log.cpp',
        'util/log/log_debug.cpp','util/util_env.cpp','util/sha1/sha1_util.cpp','util/sha1/sha1.c')]
    def run(cmd,name):
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('public translator fixture deadline')
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=remaining)
        (output/(name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError(name+': '+result.stderr[-4000:])
    def compile(unit):
        obj=output/(unit.stem+'.o')
        run(flags+['-x','c++','-c',str(unit),'-o',str(obj)],unit.stem)
        return obj
    with ThreadPoolExecutor(max_workers=2) as pool:objects=list(pool.map(compile,units))
    binary=output/'dxso-float'
    link=['-Wl,-dead_strip'] if platform.system()=='Darwin' else ['-Wl,--gc-sections','-pthread','-ldl']
    run(flags+[str(Path(__file__).with_name('fixture.cpp')),*map(str,objects),*link,'-o',str(binary)],'link')
    print(binary)
    return binary

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();build(args.output.resolve())
