"""Compile exactly two authored shaders, for one explicit Apple SDK variant."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

def verify_sources(source,shader_set='frame'):
    manifest=source/'source-pins.json'
    if not manifest.is_file() or manifest.is_symlink() or manifest.stat().st_size>4096:
        raise ValueError('authored_source_pins_required')
    raw=manifest.read_bytes(); pins=json.loads(raw)
    if shader_set not in ('frame','sampled'):raise ValueError('explicit_shader_set_required')
    expected=({'vertex.metal','pixel.metal','PublicFrameShaders.h'} if shader_set=='frame'
              else {'pixel.metal','PublicSampledShaders.h'})
    if set(pins)!={'schema','files'} or pins['schema']!=1 or type(pins['schema']) is not int or set(pins['files'])!=expected:
        raise ValueError('authored_source_pin_schema')
    for name,record in pins['files'].items():
        path=source/name
        if not path.is_file() or path.is_symlink():raise ValueError('authored_shader_input_required')
        content=path.read_bytes()
        if (type(record) is not dict or set(record)!={'bytes','sha256'} or type(record['bytes']) is not int
            or not 0<len(content)<=1_000_000 or len(content)!=record['bytes'] or b'\0' in content
            or hashlib.sha256(content).hexdigest()!=record['sha256']):
            raise ValueError('authored_source_pin_mismatch')
        content.decode('utf-8')
    return hashlib.sha256(raw).digest()

def compile(variant, source, destination,shader_set='frame'):
    if variant not in ('device','simulator'):
        raise ValueError('explicit_variant_required')
    source=Path(source).resolve(strict=True); destination=Path(destination)
    if destination.exists():
        raise ValueError('fresh_shader_output_required')
    pinned=verify_sources(source,shader_set)
    sdk='iphoneos' if variant=='device' else 'iphonesimulator'
    target='air64-apple-ios26.0'+('' if variant=='device' else '-simulator')
    sdkroot=subprocess.check_output(['xcrun','--sdk',sdk,'--show-sdk-path'],text=True,timeout=30).strip()
    destination.mkdir(parents=True)
    for stem,name in ([('vertex','PublicXenosVertex'),('pixel','PublicXenosPixel')] if shader_set=='frame'
                      else [('pixel','PublicSampledPixel')]):
        path=source/(stem+'.metal')
        if not path.is_file() or path.is_symlink() or path.stat().st_size>1_000_000:
            raise ValueError('authored_shader_input_required')
        original=hashlib.sha256(path.read_bytes()).digest()
        air=destination/(name+'.air'); library=destination/(name+'.metallib')
        subprocess.run(['xcrun','--sdk',sdk,'metal','-c','-target',target,'-isysroot',sdkroot,
            '-std=ios-metal2.4','-fno-fast-math',str(path),'-o',str(air)],check=True,timeout=120)
        subprocess.run(['xcrun','--sdk',sdk,'metal','-target',target,str(air),'-o',str(library)],check=True,timeout=120)
        content=library.read_bytes()
        if hashlib.sha256(path.read_bytes()).digest()!=original or len(content)<512 or not content.startswith(b'MTLB'):
            raise ValueError('shader_output_or_input_changed')
        print('PUBLIC_OFFLINE_XENOS_SHADER',variant,stem,library.stat().st_size,hashlib.sha256(library.read_bytes()).hexdigest(),flush=True)
    if verify_sources(source,shader_set)!=pinned:raise ValueError('authored_source_pins_changed')

if __name__=='__main__':
    if len(sys.argv) not in (4,5):raise SystemExit('variant authored_source_dir fresh_output_dir [frame|sampled]')
    compile(*sys.argv[1:])
