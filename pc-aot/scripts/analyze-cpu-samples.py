#!/usr/bin/env python3
"""Turn one launch's IOS_CPU_SAMPLE lines into a ranked function profile, locally.

The device logs only an anonymous thread ordinal, an image index and an offset
from that image's mach header. Names come from a symbol table of the *same*
private build (for example `llvm-nm -n --defined-only SkyrimGuest`), which stays
outside the repository; this script never copies raw log lines or paths.

Self time counts a sample for the function holding the program counter.
Inclusive time counts a sample once for every distinct function on its stack,
as far as the frame-pointer walk reached (eight frames). A frameless leaf
function hides its immediate caller from the inclusive view; self time is exact.
Windows are classified by the presented-frame rate in the matching IOS_TIMING
frame_gap window, so loading or a long stall is reported apart from play. That
classification is a rate heuristic, not a scene label.
"""
import argparse
import bisect
import json
from pathlib import Path
import re
import sys

U64 = (1 << 64) - 1
MAX_BYTES = 256 * 1024 * 1024
MAX_LINE = 1024 * 1024
MAX_WINDOWS = 20000
MAX_SYMBOL_DISTANCE = 4 * 1024 * 1024   # a symbol further away than this is not a match
LOW_RATE_FPS = 10.0
WINDOW_FIELDS = ('schema', 'end_ns', 'window_ns', 'target_hz', 'ticks', 'samples', 'unresolved_leaf',
                 'suspend_errors', 'overruns', 'dropped', 'stacks_total', 'omitted_stacks',
                 'omitted_samples', 'stacks')
STACK = re.compile(r'([0-9]{1,10}):([0-9]{1,5}):([01]):([0-9]{1,3}@[0-9a-f]{1,8}(?:,[0-9]{1,3}@[0-9a-f]{1,8}){0,7})')
UNRESOLVED = 255


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
    if result.keys() != set(expected):
        raise ValueError('missing fields')
    return result


def parse_window(text):
    f = fields(text, WINDOW_FIELDS)
    if f['schema'] != '1':
        raise ValueError('unknown schema')
    window = {k: unsigned(v) for k, v in f.items() if k not in ('stacks', 'schema')}
    stacks = []
    for record in f['stacks'].split(';') if f['stacks'] not in ('', '-') else []:
        match = STACK.fullmatch(record)
        if not match:
            raise ValueError('invalid stack')
        frames = []
        for frame in match[4].split(','):
            image, offset = frame.split('@')
            frames.append((int(image), int(offset, 16)))
        stacks.append(dict(count=int(match[1]), thread=int(match[2]), main=int(match[3]), frames=frames))
    listed = sum(s['count'] for s in stacks)
    if not window['ticks'] or window['samples'] > window['ticks'] * 64 or \
            len(stacks) + window['omitted_stacks'] > window['stacks_total'] or \
            listed + window['omitted_samples'] + window['dropped'] != window['samples']:
        raise ValueError('inconsistent window')
    window['stack_list'] = stacks
    return window


def parse_image(text):
    f = fields(text, ('schema', 'index', 'uuid', 'text_size'))
    if f['schema'] != '1' or not re.fullmatch(r'[0-9a-f]{32}', f['uuid']):
        raise ValueError('unknown image schema')
    return unsigned(f['index']), dict(uuid=f['uuid'], text_size=unsigned(f['text_size']))


def parse_timing_window(text):
    """(end_ns, window_ns, frames) of one frame_gap histogram, else None."""
    f = dict(token.partition('=')[::2] for token in text.split())
    if f.get('kind') != 'frame_gap':
        return None
    return unsigned(f['end_ns']), unsigned(f['window_ns']), unsigned(f['samples'])


class Symbols:
    """Sorted address list from `nm`-style text: `<hex address> <type> <name>`."""

    def __init__(self, text):
        entries = {}
        for line in text.splitlines():
            parts = line.split()
            if len(parts) == 3 and re.fullmatch(r'[0-9a-fA-F]{1,16}', parts[0]) and parts[1] in 'tTsS':
                # Keep the first name at an address; an alias adds no information.
                entries.setdefault(int(parts[0], 16), parts[2])
        self.addresses = sorted(entries)
        self.names = [entries[a] for a in self.addresses]

    def lookup(self, offset):
        i = bisect.bisect_right(self.addresses, offset) - 1
        if i < 0 or offset - self.addresses[i] > MAX_SYMBOL_DISTANCE:
            return None
        return self.names[i]


def display_name(image, offset, symbols):
    if image == UNRESOLVED:
        return '(outside app images)'
    table = symbols.get(image)
    name = table.lookup(offset) if table else None
    return name if name else 'image%d+0x%x' % (image, offset)


def guest_address(name):
    """Original x86 address encoded in a generated function name, else None."""
    match = re.fullmatch(r'_?f_([0-9a-fA-F]{5,8})', name)
    return int(match[1], 16) if match else None


def summarize(lines, symbols=None, expected_uuids=None, top=50):
    """symbols: {image index or 32-hex UUID: Symbols}; a UUID key follows the log's own image table."""
    symbols = dict(symbols or {})
    images, windows, timing, invalid = {}, [], [], 0
    for line in lines:
        try:
            if 'IOS_CPU_IMAGE ' in line:
                index, image = parse_image(line.split('IOS_CPU_IMAGE ', 1)[1])
                if images.get(index, image) != image:
                    raise RuntimeError('image table changed within one launch; use one unmodified launch log')
                images[index] = image
            elif 'IOS_CPU_SAMPLE ' in line:
                if len(windows) >= MAX_WINDOWS:
                    raise RuntimeError('window bound exceeded')
                windows.append(parse_window(line.split('IOS_CPU_SAMPLE ', 1)[1]))
            elif 'IOS_TIMING ' in line:
                row = parse_timing_window(line.split('IOS_TIMING ', 1)[1])
                if row:
                    timing.append(row)
        except ValueError:
            invalid += 1
    for index, expected in (expected_uuids or {}).items():
        if index not in images:
            raise RuntimeError('image %d is not in the log' % index)
        if images[index]['uuid'] != expected:
            raise RuntimeError('image %d does not match the symbol table build' % index)
    for key in [k for k in symbols if isinstance(k, str)]:
        # The log says which image index carries this build; a table whose UUID
        # is absent from the log cannot be matched, and is reported, not guessed.
        matches = [index for index, image in images.items() if image['uuid'] == key]
        if not matches:
            raise RuntimeError('no image with a symbol table UUID is in the log')
        table = symbols.pop(key)
        for index in matches:
            symbols[index] = table
    classes = {name: dict(self={}, inclusive={}, samples=0, windows=0) for name in ('running', 'low_rate', 'unknown')}
    per_thread, totals = {}, dict(samples=0, listed=0, omitted=0, dropped=0, unresolved_leaf=0,
                                  suspend_errors=0, overruns=0, ticks=0)
    for window in windows:
        label = 'unknown'
        best = None
        for end, span, frames in timing:
            distance = abs(end - window['end_ns'])
            if distance <= max(window['window_ns'], span) and (best is None or distance < best[0]):
                best = (distance, span, frames)
        if best and best[1]:
            label = 'low_rate' if best[2] * 1e9 / best[1] < LOW_RATE_FPS else 'running'
        bucket = classes[label]
        bucket['windows'] += 1
        totals['samples'] += window['samples']
        totals['omitted'] += window['omitted_samples']
        totals['dropped'] += window['dropped']
        totals['unresolved_leaf'] += window['unresolved_leaf']
        totals['suspend_errors'] += window['suspend_errors']
        totals['overruns'] += window['overruns']
        totals['ticks'] += window['ticks']
        for stack in window['stack_list']:
            totals['listed'] += stack['count']
            bucket['samples'] += stack['count']
            thread = per_thread.setdefault(stack['thread'], dict(samples=0, main=bool(stack['main'])))
            thread['samples'] += stack['count']
            leaf = display_name(*stack['frames'][0], symbols)
            bucket['self'][leaf] = bucket['self'].get(leaf, 0) + stack['count']
            for name in {display_name(*frame, symbols) for frame in stack['frames']}:
                bucket['inclusive'][name] = bucket['inclusive'].get(name, 0) + stack['count']

    def ranking(table, samples):
        rows = sorted(table.items(), key=lambda item: (-item[1], item[0]))[:top]
        return [dict(function=name, samples=count, percent_of_listed=round(100.0 * count / samples, 2) if samples else None,
                     guest_x86_address=('0x%x' % guest_address(name)) if guest_address(name) is not None else None)
                for name, count in rows]

    return dict(schema=1, scope='one launch; sampled running threads only; stacks limited to 8 frames',
        windows=len(windows), invalid_records=invalid, images=images, totals=totals,
        threads=[dict(thread=t, **v) for t, v in sorted(per_thread.items())],
        classes={name: dict(windows=c['windows'], listed_samples=c['samples'],
                            self=ranking(c['self'], c['samples']), inclusive=ranking(c['inclusive'], c['samples']))
                 for name, c in classes.items() if c['windows']},
        caveats=['Samples cover threads that were running at each tick; waiting threads cost nothing here.',
                 'A report window lists its most frequent stacks; omitted_samples and dropped samples are excluded from the ranking and counted in totals.',
                 'Frames outside the app images (system libraries) are not named; their time shows under the app function that called them when that caller is on the sampled stack.',
                 'A frameless leaf function hides its immediate caller from inclusive time. Self time is not affected.',
                 'low_rate windows have fewer than 10 presented frames per second in the matching timing window; this is a rate heuristic, not proof of loading or a menu.',
                 'Sampling perturbs timing slightly. Compare against a launch without sampling before drawing speed conclusions.',
                 'Offsets belong to one build. A symbol table from a different build gives wrong names; the image UUID check exists to stop that.'])


def bounded_lines(path):
    with path.open('rb') as source:
        consumed = 0
        while True:
            line = source.readline(MAX_LINE + 1)
            if not line:
                return
            consumed += len(line)
            if len(line) > MAX_LINE or consumed > MAX_BYTES:
                raise RuntimeError('input exceeds local analysis bound')
            yield line.decode('utf-8', errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path, help='one locally collected launch log; read only')
    parser.add_argument('--symbols', action='append', default=[], metavar='INDEX|UUID=FILE',
                        help='nm-style symbol file for an image index, or for the image with this 32-hex UUID (repeatable)')
    parser.add_argument('--uuid', action='append', default=[], metavar='INDEX=HEX',
                        help='expected image UUID (32 hex digits, no dashes); a mismatch is an error')
    parser.add_argument('--top', type=int, default=50)
    args = parser.parse_args()
    try:
        symbols = {}
        for item in args.symbols:
            key, _, file = item.partition('=')
            if re.fullmatch(r'[0-9a-fA-F]{32}', key):
                key = key.lower()
            elif re.fullmatch(r'[0-9]{1,2}', key):
                key = int(key)
            else:
                raise RuntimeError('invalid --symbols key')
            symbols[key] = Symbols(Path(file).read_text(encoding='utf-8', errors='replace'))
        uuids = {}
        for item in args.uuid:
            index, _, value = item.partition('=')
            if not re.fullmatch(r'[0-9a-fA-F]{32}', value):
                raise RuntimeError('invalid --uuid')
            uuids[int(index)] = value.lower()
        result = summarize(bounded_lines(args.log), symbols, uuids, args.top)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(2, 'Unable to analyze this local log: ' +
                    (str(error) if isinstance(error, RuntimeError) else 'file could not be read') + '\n')
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result['windows'] else 1


if __name__ == '__main__':
    sys.exit(main())
