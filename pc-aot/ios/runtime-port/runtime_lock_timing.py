"""Observe pinned runtime lock boundaries without moving protected work.

Only the already-prepared public source is adapted. Guest data, mutex types,
callbacks, queue ordering, release order, errors and cleanup remain unchanged.
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent


def once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise ValueError(f"Runtime lock timing {label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def function(text, anchor):
    if text.count(anchor) != 1:
        raise ValueError(f"Runtime lock timing: missing/duplicate function {anchor}")
    start = text.index(anchor)
    # These pinned definitions close at column zero. Nested scopes do not.
    end = text.index('\n}', start) + 2
    return start, end, text[start:end]


def audio(text):
    original_lock = 'pthread_mutex_lock(&g_lock);'
    original_unlock = 'pthread_mutex_unlock(&g_lock);'
    functions = {
        'static void engine_pass(CPU *c, int frames, float *out) {': ('MIX', 'MIX', 'DESTROY'),
        'static Voice *voice_new(int kind, int ch, int rate, uint32_t flags) {': ('OTHER',),
        'SHIM(xv_SetOutputVoices) {': ('OTHER',),
        'SHIM(xv_SetOutputMatrix) {': ('OTHER',),
        'SHIM(xv_DestroyVoice) {': ('DESTROY', 'DESTROY'),
        'SHIM(xs_SubmitSourceBuffer) {': ('SUBMIT',),
        'SHIM(xs_FlushSourceBuffers) {': ('FLUSH',),
        'SHIM(xs_ExitLoop) {': ('OTHER',),
        'SHIM(xs_GetState) {': ('GET_STATE',),
        'SHIM(xs_SetSourceSampleRate) {': ('OTHER',),
        'SHIM(xa_CreateSourceVoice) {': ('OTHER',),
        'SHIM(xa_CreateSubmixVoice) {': ('OTHER',),
    }
    for anchor, operations in functions.items():
        start, end, body = function(text, anchor)
        if body.count(original_lock) != len(operations):
            raise ValueError(f"Runtime audio lock count changed: {anchor}")
        for operation in operations:
            body = body.replace(original_lock,
                f'ios_audio_lock(SKYRIM_LOCK_OP_AUDIO_{operation});', 1)
        text = text[:start] + body + text[end:]
    if original_lock in text or text.count(original_unlock) != 17:
        raise ValueError('Runtime audio mutex entry/exit set changed')
    text = text.replace(original_unlock, 'ios_audio_unlock();')
    text = once(text,
        '    if (v->fmt == FMT_ADPCM && v->block_align && v->spb) {',
        '    if (v->fmt == FMT_ADPCM && v->block_align && v->spb) {\n'
        '        const uint64_t iosAdpcmStart = ios_runtime_lock_now();', 'ADPCM start')
    text = once(text, '    } else if (v->fmt == FMT_XWMA) {',
        '        ios_runtime_lock_report(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WORK,\n'
        '            SKYRIM_LOCK_OP_ADPCM_DECODE, iosAdpcmStart, ios_runtime_lock_now(), 0);\n'
        '    } else if (v->fmt == FMT_XWMA) {', 'ADPCM completion')
    text = once(text, '    xwma_reset(v->wma);',
        '    {\n'
        '        const uint64_t iosResetStart = ios_runtime_lock_now();\n'
        '        xwma_reset(v->wma);\n'
        '        ios_runtime_lock_report(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WORK,\n'
        '            SKYRIM_LOCK_OP_AUDIO_RESET, iosResetStart, ios_runtime_lock_now(), 0);\n'
        '    }', 'external audio reset')
    helper = (HERE / 'runtime_lock_event.h.in').read_text(encoding='utf-8') + '\n'
    helper += (HERE / 'runtime_audio_lock.c.in').read_text(encoding='utf-8')
    return once(text, 'static pthread_mutex_t g_lock;',
                'static pthread_mutex_t g_lock;\n' + helper, 'audio helper')


def com(text):
    guard = 'std::lock_guard<std::recursive_mutex> l(g_lock);'
    for anchor, operation, timed in (
            ('void *com_host(uint32_t g) {', 'LOOKUP', 'false'),
            ('uint32_t com_wrap(void *host, int iface) {', 'WRAP', 'true'),
            ('static void com_forget(uint32_t g) {', 'FORGET', 'true')):
        start, end, body = function(text, anchor)
        body = once(body, guard,
                    f'IosComLock l(SKYRIM_LOCK_OP_COM_{operation}, {timed});', operation)
        text = text[:start] + body + text[end:]
    if guard in text:
        raise ValueError('Runtime COM registry mutex entry set changed')
    for call, category in (('shadow_forget(h);', 'SHADOW'), ('bc_forget(h);', 'BC')):
        text = once(text, '    ' + call,
            '    {\n'
            f'        IosComCleanupSpan iosCleanup(SKYRIM_LOCK_{category}, SKYRIM_LOCK_OP_{category}_CLEANUP);\n'
            f'        {call}\n'
            '    }', category + ' cleanup interval')
    helper = (HERE / 'runtime_com_lock.cpp.in').read_text(encoding='utf-8')
    text = once(text, 'std::recursive_mutex g_lock;',
                'std::recursive_mutex g_lock;\n' + helper, 'COM helper')
    # Standard/system headers must stay outside the registry's anonymous namespace.
    return '#include <atomic>\n' + (HERE / 'runtime_lock_event.h.in').read_text(encoding='utf-8') + '\n' + text


def adapt(name, text):
    if name == 'xaudio2.c':
        return audio(text)
    if name == 'd3d9_bridge.cpp':
        return com(text)
    return text
