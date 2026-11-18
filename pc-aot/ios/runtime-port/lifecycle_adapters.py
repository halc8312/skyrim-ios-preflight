"""Higher-level lifecycle admission for the fixed public D3D runtime.

Never gate a native submit, CS worker, converter or initializer. An admitted
outer API call owns all nested producers until it returns. Present's UIKit
pacer/pump is deliberately outside admission so a UI callback cannot park its
own in-flight host call. No D3D HRESULT is replaced or synthesized.
"""
import re

ENTRY = re.compile(r'(?:static )?void (B_[A-Za-z0-9_]+)\(CPU \*c\) \{')
COUNTS = {'d3d9_bridge.cpp': 33, 'd3d9_bridge_gen.cpp': 249, 'd3d9_bcemu.cpp': 8}
PRESENT = {'B_IDirect3DDevice9_Present', 'B_IDirect3DSwapChain9_Present'}


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Lifecycle runtime anchor changed: ' + old[:70])
    return text.replace(old, new, 1)


def adapt(name, text):
    if name in COUNTS:
        matches = list(ENTRY.finditer(text))
        if len(matches) != COUNTS[name]:
            raise ValueError('Lifecycle entry set changed: ' + name)
        text = '#include "lifecycle_runtime.h"\n' + text
        text = ENTRY.sub(lambda m: m.group(0) +
            ('' if m.group(1) in PRESENT else '\n    SkyrimLifecycleCall iosLifecycle;'), text)
        if name == 'd3d9_bridge.cpp':
            text = once(text, 'static void shim_Direct3DCreate9(CPU *c) {',
                'static void shim_Direct3DCreate9(CPU *c) {\n    SkyrimLifecycleCall iosLifecycle;')
            # The optional capture itself performs GPU readback. Keep it in
            # the same admitted call as Present, after the UIKit pacer.
            text = once(text, '    capture_frame(d);\n', '')
            for obj in ('d', 's'):
                marker = f'    HRESULT hr = {obj}->Present('
                start = text.index(marker)
                end = text.index(';', start) + 1
                call = text[start:end].replace('    HRESULT hr =', '        hr =', 1)
                replacement = ('    HRESULT hr;\n    {\n        SkyrimLifecycleCall iosLifecycle;\n' +
                    ('        capture_frame(d);\n' if obj == 'd' else '') + call + '\n    }')
                text = text[:start] + replacement + text[end:]
        return text
    if name == 'd3dx_native.cpp':
        names = ('D3DXCreateTextureFromFileInMemory', 'D3DXCreateCubeTextureFromFileInMemory',
                 'D3DXCreateVolumeTextureFromFileInMemory', 'D3DXCreateTextureFromFileExA',
                 'D3DXLoadSurfaceFromMemory', 'D3DXLoadSurfaceFromSurface')
        text = '#include "lifecycle_runtime.h"\n' + text
        for function in names:
            anchor = f'SHIM({function}) {{'
            text = once(text, anchor, anchor + '\n    SkyrimLifecycleCall iosLifecycle;')
        return text
    if name == 'xaudio2.c':
        text = '#include "lifecycle_runtime.h"\n' + text
        anchor = '    for (;;) {\n        if (g_dev && !recovering) {'
        text = once(text, anchor, '''    for (;;) {
        /* No mixer/guest callback lock is held at this boundary. Preserve the
         * queue, voice cursors and game's StartEngine/StopEngine state. */
        if (skyrim_lifecycle_audio_should_pause()) {
            if (g_dev) SDL_PauseAudioDevice(g_dev, 1);
            skyrim_lifecycle_audio_event(1);
            skyrim_lifecycle_audio_wait();
            if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
            next = now_us();
            probe_started = 0;
            skyrim_lifecycle_audio_event(0);
        }
        if (g_dev && !recovering) {''')
        # A full queue must not hold the mixer in its recovery loop for 500ms
        # after Home; return to the top-level pause boundary without clearing.
        text = once(text, '                if (q < q0) { q0 = q; t0 = now_us(); }',
            '                if (skyrim_lifecycle_audio_should_pause()) break;\n'
            '                if (q < q0) { q0 = q; t0 = now_us(); }')
        return once(text, '        if (!g_running) { usleep(PASS_FRAMES_MS * 1000); continue; }',
            '        if (skyrim_lifecycle_audio_should_pause()) continue;\n'
            '        if (!g_running) { usleep(PASS_FRAMES_MS * 1000); continue; }')
    return text
