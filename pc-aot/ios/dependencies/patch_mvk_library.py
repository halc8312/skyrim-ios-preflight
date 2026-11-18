#!/usr/bin/env python3
"""Compose pinned library copy reductions with owned macro variants.

A copied library shares the retained immutable Metal library, not its owning
raw-pointer child map. Derived variants are rebuilt on demand for the new
owner. Ordinary assignment/destruction still require no concurrent API use.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('compressed_arguments', Path(__file__).with_name('patch_mvk_compressed_copy.py'))
compressed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compressed)
PINS = compressed.PINS

OLD_COPY = b'''\t_maySpecializeWithMacro(other._maySpecializeWithMacro),
\t_specializationVariants(other._specializationVariants) {

\t_mtlLibrary = [other._mtlLibrary retain];
\t_shaderConversionResultInfo = other._shaderConversionResultInfo;
\t_compressedMSL = other._compressedMSL;'''
NEW_COPY = b'''\t_mtlLibrary(nil),
\t_compressedMSL(other._compressedMSL),
\t_shaderConversionResultInfo(other._shaderConversionResultInfo),
\t_maySpecializeWithMacro(other._maySpecializeWithMacro) {
\t// Variants belong to exactly one owner. Cache merge may rebind this
\t// library's owner, so start with an empty derived map and specialize lazily.
\t// Finish allocating owned C++ data before retaining the Metal object.
\t_mtlLibrary = [other._mtlLibrary retain];'''
OLD_ASSIGN = b'''\tif (_mtlLibrary != other._mtlLibrary) {
\t\t[_mtlLibrary release];
\t\t_mtlLibrary = [other._mtlLibrary retain];
\t}
\t_owner = other._owner;
\t_shaderConversionResultInfo = other._shaderConversionResultInfo;
\t_compressedMSL = other._compressedMSL;
\treturn *this;'''
NEW_ASSIGN = b'''\tif (this == &other) { return *this; }
\t// Prepare the replacement first. Its derived map is empty; swapping
\t// transfers old children to the temporary for exactly-once destruction.
\tMVKShaderLibrary replacement(other);
\tstd::swap(_owner, replacement._owner);
\tstd::swap(_mtlLibrary, replacement._mtlLibrary);
\tstd::swap(_shaderConversionResultInfo, replacement._shaderConversionResultInfo);
\tstd::swap(_compressedMSL, replacement._compressedMSL);
\tstd::swap(_maySpecializeWithMacro, replacement._maySpecializeWithMacro);
\tstd::swap(_iosShaderOrigin, replacement._iosShaderOrigin);
\t_specializationVariants.swap(replacement._specializationVariants);
\treturn *this;'''

# Never serialized. Copies start empty through the member initializer, and
# assignment receives the empty copy's origin. An actual published root alone
# can describe the prefetch which produced the object returned to a waiter.
ORIGIN_FIELD = b'\tSkyrimPerfShaderOrigin _iosShaderOrigin{};'
ORIGIN_METHODS = b'''\t// Runtime-only producer observations; none identifies a lock owner.
\tvoid iosBeginRootCompilerRequest() {
\t\tconst auto context = skyrim_perf_context_get();
\t\tif (context.role != SKYRIM_PERF_ROLE_PREFETCH || context.source != SKYRIM_PERF_SOURCE_ROOT ||
\t\t\t_owner->getVkObjectType() != VK_OBJECT_TYPE_PIPELINE_CACHE) { return; }
\t\tconst auto prefetch = skyrim_perf_prefetch_get();
\t\t// A duplicate registration can leave the original prerequisite time unknown.
\t\tif (!prefetch.queued_ns || !prefetch.worker_ns) { return; }
\t\t_iosShaderOrigin.prefetch = prefetch;
\t\t// Caller boundary immediately before newMTLLibrary, not the dispatched
\t\t// Metal API invocation, compiler execution, or GPU execution time.
\t\t_iosShaderOrigin.compile_ns = skyrim_perf_now_ns();
\t}
\tvoid iosMarkPublished() {
\t\t// Called only for the actual winning root under the catalog lock.
\t\tif (_iosShaderOrigin.compile_ns && _mtlLibrary) {
\t\t\t_iosShaderOrigin.published_ns = skyrim_perf_now_ns();
\t\t}
\t}
\tSkyrimPerfShaderOrigin iosShaderOrigin() const {
\t\treturn _iosShaderOrigin.published_ns ? _iosShaderOrigin : SkyrimPerfShaderOrigin{};
\t}

'''
MAP = b'\tstd::map<std::vector<std::pair<uint32_t, MVKShaderMacroValue>>, MVKShaderLibrary *> _specializationVariants;'
LOOKUP = b'''\t\t\tauto entry = _specializationVariants.find(spec_list);
\t\t\tif (entry != _specializationVariants.end()) {
\t\t\t\tlib = entry->second->_mtlLibrary;
\t\t\t} else {
\t\t\t\tMVKShaderLibrary *new_mvklib = new MVKShaderLibrary(_owner, _shaderConversionResultInfo, _compressedMSL, &spec_list);
\t\t\t\t_specializationVariants[spec_list] = new_mvklib;
\t\t\t\tlib = new_mvklib->_mtlLibrary;
\t\t\t}'''
LOCKED_LOOKUP = b'''\t\t\t// The pipeline-cache lock does not protect this derived map.
\t\t\t// Never hold its lock across Metal compilation or a device monitor.
\t\t\tMVKShaderLibrary* variant = nullptr;
\t\t\t{
\t\t\t\tstd::lock_guard<std::mutex> guard(_specializationLock);
\t\t\t\tauto entry = _specializationVariants.find(spec_list);
\t\t\t\tif (entry != _specializationVariants.end()) { variant = entry->second; }
\t\t\t}
\t\t\tif (!variant) {
\t\t\t\tstd::unique_ptr<MVKShaderLibrary> candidate(new MVKShaderLibrary(_owner, _shaderConversionResultInfo, _compressedMSL, &spec_list));
\t\t\t\t{
\t\t\t\t\tstd::lock_guard<std::mutex> guard(_specializationLock);
\t\t\t\t\tauto published = _specializationVariants.emplace(spec_list, candidate.get());
\t\t\t\t\tvariant = published.first->second;
\t\t\t\t\tif (published.second) { candidate.release(); }
\t\t\t\t}
\t\t\t\t// Racing cold misses may compile twice; the losing object is owned
\t\t\t\t// locally and released outside the map lock. Failed variants keep
\t\t\t\t// the existing cached-error policy, without automatic retries.
\t\t\t}
\t\t\tlib = variant->_mtlLibrary;'''

# A specialized child never specializes again and is not a root in the
# serialized shader cache. Borrow only during synchronous decompression;
# the decoded string and Metal compiler retain their existing ownership.
DECOMPRESS_DECL = b'\tvoid decompressMSL(std::string& msl);'
DECOMPRESS_BODY = b'''void MVKShaderLibrary::decompressMSL(string& msl) {
\tuint64_t startTime = getPerformanceTimestamp();
\t_compressedMSL.decompress(msl);
\taddPerformanceInterval(getPerformanceStats().shaderCompilation.mslDecompress, startTime);
}'''
DECOMPRESS_BORROWED = b'''void MVKShaderLibrary::decompressMSL(string& msl) {
\tdecompressMSL(msl, _compressedMSL);
}

void MVKShaderLibrary::decompressMSL(string& msl, const MVKCompressor<string>& source) {
\tuint64_t startTime = getPerformanceTimestamp();
\t// Same pinned MVKCompressor decoder, reading only immutable source fields.
\tmsl.resize(source._uncompressedSize);
\tmvkDecompress(source._compressed.data(), source._compressed.size(),
\t\t(uint8_t*)msl.data(), msl.size(), source._algorithm);
\taddPerformanceInterval(getPerformanceStats().shaderCompilation.mslDecompress, startTime);
}'''
CONSTRUCTOR_PAYLOAD = b'''\t_compressedMSL = compressedMSL;
\tstring msl;
\tdecompressMSL(msl);
\tcompileLibrary(msl, specializationMacroDef);'''
BORROWED_PAYLOAD = b'''\t// Only roots need compressed source for serialization or later variants.
\t// The variant input is immutable and remains alive throughout this call.
\tif (!specializationMacroDef) { _compressedMSL = compressedMSL; }
\tstring msl;
\tdecompressMSL(msl, specializationMacroDef ? compressedMSL : _compressedMSL);
\tcompileLibrary(msl, specializationMacroDef);'''


def once(raw, old, new):
    if raw.count(old) != 1:
        raise ValueError('Macro variant ownership anchor changed')
    return raw.replace(old, new)


def patched(name, raw):
    # This also validates the pristine hash and exact compressed-copy anchors.
    raw = compressed.patched(name, raw)
    if name.endswith('.h'):
        raw = once(raw, DECOMPRESS_DECL, DECOMPRESS_DECL + b'\n\tvoid decompressMSL(std::string& msl, const MVKCompressor<std::string>& source);')
        raw = once(raw, b'class MVKShaderLibrary : public MVKBaseDeviceObject {\n\npublic:\n\n',
                   b'class MVKShaderLibrary : public MVKBaseDeviceObject {\n\npublic:\n\n' + ORIGIN_METHODS)
        raw = once(raw, MAP, ORIGIN_FIELD + b'\n' + MAP + b'\n\tstd::mutex _specializationLock;')
        return b'#include "skyrim_performance_metrics.h"\n#include <mutex>\n#include <memory>\n' + raw
    raw = once(raw, OLD_COPY, NEW_COPY)
    raw = once(raw, OLD_ASSIGN, NEW_ASSIGN)
    raw = once(raw, DECOMPRESS_BODY, DECOMPRESS_BORROWED)
    raw = once(raw, CONSTRUCTOR_PAYLOAD, BORROWED_PAYLOAD)
    raw = once(raw, b'\tcompileLibrary(conversionResult.msl);',
        b'\tSkyrimPerfContextScope iosSource(SKYRIM_PERF_CONTEXT_KEEP, SKYRIM_PERF_SOURCE_ROOT);\n\tcompileLibrary(conversionResult.msl);')
    raw = once(raw, b'\tcompileLibrary(msl, specializationMacroDef);',
        b'\tSkyrimPerfContextScope iosSource(SKYRIM_PERF_CONTEXT_KEEP, specializationMacroDef ? SKYRIM_PERF_SOURCE_MACRO : SKYRIM_PERF_SOURCE_RESTORE);\n\tcompileLibrary(msl, specializationMacroDef);')
    raw = once(raw, b'\t_mtlLibrary = slc->newMTLLibrary(nsSrc, _shaderConversionResultInfo, macro_def);',
        b'\tiosBeginRootCompilerRequest();\n\t_mtlLibrary = slc->newMTLLibrary(nsSrc, _shaderConversionResultInfo, macro_def);')
    return b'#include "skyrim_performance_metrics.h"\n' + once(raw, LOOKUP, LOCKED_LOOKUP)


def verify(name, current, pristine):
    if hashlib.sha256(pristine).hexdigest() != PINS[name] or current != patched(name, pristine):
        raise ValueError('Library stage differs from composed reviewed patches')


def apply(source, pristine):
    edits = {}
    for name in PINS:
        raw = (pristine/name).read_bytes()
        expected = patched(name, raw)
        current = (source/name).read_bytes()
        if current not in (raw, compressed.patched(name, raw), expected):
            raise ValueError('Unreviewed library source stage: '+name)
        edits[name] = expected
    for name, content in edits.items():
        (source/name).write_bytes(content)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--pristine', type=Path, required=True)
    args = parser.parse_args()
    apply(args.source, args.pristine)
    print('Pinned library copy and macro-variant ownership fixes present')
