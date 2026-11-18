"""Receive bounded, sanitized SkyrimIOS diagnostics over a private Serve proxy.

Run only behind Tailscale Serve on loopback; this is separate from the read-only
OTA server. No network access occurs at import. This receiver never runs commands
from a request, extracts archives, or accepts client-supplied filesystem paths.
"""
from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass
from decimal import Decimal
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import stat
import tempfile
import threading
import time
from urllib.parse import urlsplit
import uuid

ROOT = Path(__file__).resolve().parents[1]
PRIVATE_ROOT = ROOT / 'private'
MAX_BODY = 16 << 20
MAX_LINE = 8191
MAX_LINES = 60000
MAX_SESSION_BYTES = 6 << 20
MAX_SNAPSHOT = 64 << 20
MAX_REPORTS = 128
MAX_STORAGE = 512 << 20
BUFFER = 64 << 10
ROUTES = ('/skyrim-diagnostics/v1/reports', '/v1/reports')
SCHEMA_PATH = ROOT / 'ios/runtime-port/diagnostic_schema.h'
REPORT_ID = re.compile(r'[0-9a-f]{32}')
SHA256 = re.compile(r'[0-9a-f]{64}')


class Rejected(ValueError):
    def __init__(self, code, status=422):
        super().__init__(code)
        self.code, self.status = code, status


def require(condition, code, status=422):
    if not condition:
        raise Rejected(code, status)


def unsigned(value, maximum=(1 << 64) - 1):
    require(type(value) is int and 0 <= value <= maximum, 'integer_range')
    return value


def no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate_json_key')
        result[key] = value
    return result


def parse_json(raw):
    def invalid_constant(_):
        raise Rejected('nonfinite_json_number')
    try:
        return json.loads(raw.decode('utf-8'), object_pairs_hook=no_duplicate_keys,
                          parse_constant=invalid_constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError, OverflowError) as error:
        raise Rejected('invalid_json') from error


def plain(path, directory=False):
    info = path.stat(follow_symlinks=False)
    require((stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)) and
            not getattr(info, 'st_file_attributes', 0) & 0x400,
            'storage_link_or_type', 503)
    return info


def private_directory(path, create=False):
    path = Path(path)
    require(path.is_absolute() and path != PRIVATE_ROOT and
            PRIVATE_ROOT in path.parents and '..' not in path.parts, 'private_storage_scope', 503)
    for ancestor in reversed((path, *path.parents)):
        if not ancestor.exists():
            require(create and (ancestor == PRIVATE_ROOT or PRIVATE_ROOT in ancestor.parents),
                    'storage_missing', 503)
            ancestor.mkdir(exist_ok=False)
        plain(ancestor, directory=True)
    return path


def read_small(path, maximum):
    before = plain(path)
    require(before.st_size <= maximum, 'stored_metadata_size', 503)
    raw = path.read_bytes()
    after = plain(path)
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
            'stored_metadata_changed', 503)
    return raw


def load_timing_parser():
    definition = importlib.util.spec_from_file_location('private_diagnostic_timing',
                                                       ROOT / 'scripts/analyze-frame-timing.py')
    module = importlib.util.module_from_spec(definition)
    definition.loader.exec_module(module)
    return module


class Schema:
    """Use the same anchored ASCII line rules as the app, not a prefix filter."""
    def __init__(self, path=SCHEMA_PATH):
        self.patterns = []
        expression = re.compile(r'^\s*X\(("(?:[^"\\]|\\.)*"),\s*("(?:[^"\\]|\\.)*")\)(?: \\)?$')
        for line in path.read_text(encoding='utf-8').splitlines():
            if not line.lstrip().startswith('X('):
                continue
            match = expression.fullmatch(line)
            require(match is not None, 'diagnostic_schema_syntax', 503)
            name, pattern = (json.loads(value) for value in match.groups())
            self.patterns.append((name, re.compile(pattern, re.ASCII)))
        require(self.patterns, 'diagnostic_schema_empty', 503)
        self.timing = load_timing_parser()

    def line(self, value):
        require(type(value) is str and 0 < len(value) <= MAX_LINE and
                value.isascii() and all(32 <= ord(c) < 127 for c in value), 'line_bounds_or_encoding')
        require(any(pattern.fullmatch(value) for _, pattern in self.patterns), 'line_not_allowlisted')
        try:
            if value.startswith('IOS_TIMING '):
                self.timing.timing(value.split(' ', 1)[1])
            elif value.startswith('IOS_TIMING_CONTEXT '):
                raw = value.split(' ', 1)[1]
                base, separator, suffix = raw.partition(' role=')
                require(separator, 'timing_context')
                row = self.timing.timing(base)
                context = self.timing.fields('role=' + suffix, {'role', 'source', 'result'})
                role, source, result = (self.timing.unsigned(context[k]) for k in ('role', 'source', 'result'))
                require(role <= 2 and source <= 3 and result <= 2, 'timing_context')
                role_only = row['kind'] in ('mvk_cold_lock_wait', 'graphics_pipeline', 'compute_pipeline', 'mvk_convert')
                require(row['kind'] in ('mvk_msl_wait', 'graphics_compile_wait', 'compute_compile_wait', 'graphics_state_wait') or role_only,
                        'timing_context_kind')
                require(not (row['kind'] == 'mvk_msl_wait' and result) and
                        not (row['kind'] != 'mvk_msl_wait' and source) and
                        not (role_only and result), 'timing_context_fields')
            elif value.startswith('IOS_STALL '):
                self.timing.stalls(value.split(' ', 1)[1])
            elif value.startswith('IOS_LOCK '):
                self.timing.locks(value.split(' ', 1)[1])
            elif value.startswith('IOS_SHADER_LEAD '):
                return 'lead', self.timing.shader_lead(value.split(' ', 1)[1])
            elif value.startswith('IOS_SHADER_LEAD_DROPS '):
                return 'drops', self.timing.shader_lead_drops(value.split(' ', 1)[1])
            elif value.startswith('IOS_DIAG_SETTING '):
                number = Decimal(value.rsplit('=', 1)[1])
                require(abs(number) <= 1000000000, 'setting_range')
            elif value.startswith('IOS_LIFECYCLE '):
                values = dict(token.split('=', 1) for token in value.split(' ')[1:])
                require(-(1 << 31) <= int(values['result']) < 1 << 31 and
                        int(values['active_calls']) < 1 << 32 and
                        all(int(values[k]) < 1 << 64 for k in ('generation', 'monotonic_ns', 'elapsed_ns')),
                        'lifecycle_range')
        except ValueError as error:
            raise Rejected('invalid_timing_record') from error

    def bundle(self, raw):
        require(0 < len(raw) <= MAX_BODY, 'body_size', 413)
        value = parse_json(raw)
        require(type(value) is dict and set(value) == {'schema', 'report_id', 'app_build', 'sessions'},
                'bundle_fields')
        require(type(value['schema']) is int and value['schema'] == 1, 'bundle_schema')
        require(type(value['report_id']) is str and REPORT_ID.fullmatch(value['report_id']), 'report_id')
        require(1 <= unsigned(value['app_build'], 999999), 'app_build')
        sessions = value['sessions']
        require(type(sessions) is list and 1 <= len(sessions) <= 2, 'session_count')
        slots = set()
        for session in sessions:
            require(type(session) is dict and set(session) == {'slot', 'build', 'capture', 'snapshot_bytes',
                                                               'excluded_lines', 'lines'}, 'session_fields')
            require(type(session['slot']) is str and session['slot'] in ('current', 'previous') and
                    session['slot'] not in slots, 'session_slot')
            slots.add(session['slot'])
            require(1 <= unsigned(session['build'], 999999), 'session_build')
            require(session['capture'] == ('live_prefix' if session['slot'] == 'current' else 'closed'),
                    'session_capture')
            require(session['slot'] != 'current' or session['build'] == value['app_build'],
                    'current_build_mismatch')
            unsigned(session['snapshot_bytes'], MAX_SNAPSHOT)
            unsigned(session['excluded_lines'], MAX_SNAPSHOT)
            lines = session['lines']
            require(type(lines) is list and 1 <= len(lines) <= MAX_LINES, 'line_count')
            require(all(type(line) is str for line in lines), 'line_shape')
            require(sum(len(line) + 1 for line in lines) <= MAX_SESSION_BYTES, 'session_byte_limit')
            # The reporter shares a sequence across lead/drop records. Each
            # captured launch has its own namespace; repeated identical rows
            # are harmless, conflicting reuse cannot be trusted.
            shader_sequences = {}
            for line in lines:
                record = self.line(line)
                if record is not None:
                    seq = record[1]['seq']
                    require(seq not in shader_sequences or shader_sequences[seq] == record,
                            'shader_lead_sequence_conflict')
                    shader_sequences[seq] = record
        require('current' in slots, 'current_session_required')
        return value


@dataclass(frozen=True)
class Config:
    storage: Path
    login: str
    token: str
    expires: int

    @classmethod
    def from_dict(cls, value):
        require(type(value) is dict and set(value) == {'storage_dir', 'tailscale_login', 'token', 'expires_unix'},
                'receiver_configuration', 503)
        login, token, expires = value['tailscale_login'], value['token'], value['expires_unix']
        require(type(login) is str and 1 <= len(login) <= 320 and login.isascii() and
                all(32 < ord(c) < 127 for c in login), 'receiver_identity_configuration', 503)
        require(type(token) is str and re.fullmatch(r'[a-zA-Z0-9_-]{43,128}', token), 'receiver_token_configuration', 503)
        require(type(expires) is int and 0 < expires <= (1 << 53) - 1, 'receiver_expiry_configuration', 503)
        return cls(private_directory(value['storage_dir'], create=True), login, token, expires)


class Receiver:
    def __init__(self, config, schema=None):
        self.config = config
        self.schema = schema or Schema()
        self.lock = threading.Lock()

    def headers(self, path, headers):
        identity = headers.get_all('Tailscale-User-Login', [])
        authorization = headers.get_all('Authorization', [])
        require(len(identity) == 1 and len(authorization) == 1 and
                hmac.compare_digest(identity[0].encode(), self.config.login.encode()) and
                hmac.compare_digest(authorization[0].encode(), ('Bearer ' + self.config.token).encode()),
                'not_found', 404)
        parsed = urlsplit(path)
        require(parsed.path in ROUTES and not parsed.query and not parsed.fragment and
                not parsed.scheme and not parsed.netloc,
                'not_found', 404)
        require(time.time() < self.config.expires, 'authorization_expired', 410)
        require(not headers.get_all('Transfer-Encoding') and not headers.get_all('Content-Encoding'),
                'unsupported_body_encoding', 400)
        lengths = headers.get_all('Content-Length', [])
        digests = headers.get_all('X-Content-SHA256', [])
        types = headers.get_all('Content-Type', [])
        require(len(lengths) == 1 and re.fullmatch(r'[0-9]{1,8}', lengths[0]), 'content_length', 411)
        require(len(digests) == 1 and SHA256.fullmatch(digests[0]), 'content_sha256', 400)
        require(types == ['application/json'], 'content_type', 415)
        length = int(lengths[0])
        require(0 < length <= MAX_BODY, 'body_size', 413)
        return length, digests[0]

    def existing(self, report_id, verify=True):
        path = self.config.storage / ('report-' + report_id + '.json')
        if not os.path.lexists(path):
            return None
        record = parse_json(read_small(path, 16 << 10))
        require(type(record) is dict and type(record.get('schema')) is int and record['schema'] == 1 and
                record.get('report_id') == report_id,
                'stored_report_invalid', 503)
        directory = record.get('directory')
        require(type(directory) is str and re.fullmatch(r'bundle-[0-9a-f]{32}', directory), 'stored_directory_invalid', 503)
        require(type(record.get('sha256')) is str and SHA256.fullmatch(record['sha256']) and
                type(record.get('bytes')) is int and 0 < record['bytes'] <= MAX_BODY and
                type(record.get('stored_bytes')) is int and 0 < record['stored_bytes'] <= 3 * MAX_BODY,
                'stored_receipt_invalid', 503)
        folder = self.config.storage / directory
        plain(folder, directory=True)
        require(plain(folder / 'bundle.json').st_size == record['bytes'], 'stored_bundle_missing', 503)
        sessions = record.get('sessions')
        require(type(sessions) is list and 1 <= len(sessions) <= 2, 'stored_session_shape', 503)
        files = {'bundle.json': (record['bytes'], record['sha256'])}
        for session in sessions:
            require(type(session) is dict and session.get('file') in ('current.log', 'previous.log') and
                    session['file'] not in files and type(session.get('bytes')) is int and
                    0 < session['bytes'] <= MAX_SESSION_BYTES and type(session.get('sha256')) is str and
                    SHA256.fullmatch(session['sha256']), 'stored_session_shape', 503)
            files[session['file']] = (session['bytes'], session['sha256'])
        for name, (size, expected) in files.items():
            require(plain(folder / name).st_size == size, 'stored_file_size', 503)
            if verify:
                with (folder / name).open('rb') as stream:
                    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                require(digest == expected, 'stored_file_hash', 503)
        return record

    def capacity(self, additional, new_report=False):
        # Account for incomplete bundles left by a killed process too. Do not
        # delete or recursively traverse unknown files to make room.
        total, reports, seen = 0, 0, set()
        entries = list(self.config.storage.iterdir())
        require(len(entries) <= 1024, 'receiver_full', 507)
        for path in entries:
            if re.fullmatch(r'bundle-[0-9a-f]{32}', path.name):
                plain(path, directory=True)
                children = list(path.iterdir())
                require(len(children) <= 4 and all(p.name in ('bundle.json', 'current.log', 'previous.log', 'manifest.json')
                                                 for p in children), 'storage_entry_invalid', 503)
            else:
                require(path.name == '.receiver.lock' or
                        re.fullmatch(r'report-[0-9a-f]{32}\.json|\.incoming-[A-Za-z0-9_-]+\.pending', path.name),
                        'storage_entry_invalid', 503)
                reports += path.name.startswith('report-')
                children = [path]
            for child in children:
                info = plain(child)
                identity = (info.st_dev, info.st_ino) if info.st_ino else str(child)
                if identity not in seen:
                    seen.add(identity)
                    total += info.st_size
        require((not new_report or reports < MAX_REPORTS) and total + additional <= MAX_STORAGE,
                'receiver_full', 507)

    @staticmethod
    def receipt(record, status):
        return status, {'schema': 1, 'report_id': record['report_id'],
                        'sha256': record['sha256'], 'bytes': record['bytes']}

    def receive(self, stream, length, expected_sha, *, deadline=None):
        require(0 < length <= MAX_BODY and SHA256.fullmatch(expected_sha), 'body_size_or_hash', 413)
        require(self.lock.acquire(blocking=False), 'receiver_busy', 503)
        incoming, folder, record = None, None, None
        owned = []
        try:
            root = private_directory(self.config.storage)
            self.capacity(length)
            deadline = time.monotonic() + 60 if deadline is None else deadline
            fd, temporary = tempfile.mkstemp(prefix='.incoming-', suffix='.pending', dir=root)
            incoming = Path(temporary)
            digest, remaining = hashlib.sha256(), length
            with os.fdopen(fd, 'wb') as output:
                while remaining:
                    require(time.monotonic() < deadline, 'receive_timeout', 408)
                    chunk = stream.read(min(BUFFER, remaining))
                    require(type(chunk) is bytes and 0 < len(chunk) <= remaining, 'incomplete_body', 400)
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                output.flush()
                os.fsync(output.fileno())
            require(time.time() < self.config.expires, 'authorization_expired', 410)
            require(digest.hexdigest() == expected_sha, 'body_hash_mismatch', 400)
            raw = read_small(incoming, MAX_BODY)
            bundle = self.schema.bundle(raw)
            report_id = bundle['report_id']
            previous = self.existing(report_id)
            if previous:
                require(previous['sha256'] == expected_sha and previous['bytes'] == length,
                        'report_id_conflict', 409)
                return self.receipt(previous, 200)
            stored_size = length + sum(sum(len(line) + 1 for line in s['lines']) for s in bundle['sessions'])
            self.capacity(stored_size - length + (16 << 10), new_report=True)
            folder = root / ('bundle-' + uuid.uuid4().hex)
            folder.mkdir(mode=0o700, exist_ok=False)
            os.link(incoming, folder / 'bundle.json')
            owned.append(folder / 'bundle.json')
            sessions = []
            for session in bundle['sessions']:
                name = session['slot'] + '.log'
                path = folder / name
                checksum, size = hashlib.sha256(), 0
                with path.open('xb') as output:
                    owned.append(path)
                    for line in session['lines']:
                        encoded = (line + '\n').encode('ascii')
                        output.write(encoded)
                        checksum.update(encoded)
                        size += len(encoded)
                    output.flush()
                    os.fsync(output.fileno())
                sessions.append({'file': name, 'build': session['build'], 'capture': session['capture'],
                                 'snapshot_bytes': session['snapshot_bytes'], 'excluded_lines': session['excluded_lines'],
                                 'bytes': size, 'sha256': checksum.hexdigest(), 'lines': len(session['lines'])})
            record = {'schema': 1, 'report_id': report_id, 'sha256': expected_sha, 'bytes': length,
                      'stored_bytes': stored_size, 'directory': folder.name, 'app_build': bundle['app_build'],
                      'sessions': sessions}
            manifest = folder / 'manifest.json'
            with manifest.open('xb') as output:
                owned.append(manifest)
                output.write((json.dumps(record, separators=(',', ':')) + '\n').encode('ascii'))
                output.flush()
                os.fsync(output.fileno())
            # The manifest is the sole completion signal. Hard-link publication
            # is atomic and rejects an existing report ID on every supported OS.
            try:
                os.link(manifest, root / ('report-' + report_id + '.json'))
            except FileExistsError:
                previous = self.existing(report_id)
                require(previous and previous['sha256'] == expected_sha and previous['bytes'] == length,
                        'report_id_conflict', 409)
                return self.receipt(previous, 200)
            return self.receipt(record, 201)
        finally:
            # Only exact paths created by this request are eligible for cleanup.
            # A successful link with a lost/interrupted return still preserves
            # its bundle; the next explicit retry receives the same receipt.
            published = False
            if record is not None:
                try:
                    completed = self.existing(record['report_id'], verify=False)
                    published = completed is not None and completed['directory'] == record['directory']
                except (OSError, ValueError):
                    published = True  # Unknown publication state: preserve evidence.
            if folder is not None and not published:
                for path in reversed(owned):
                    try:
                        path.unlink()
                    except OSError:
                        pass
                try:
                    folder.rmdir()
                except OSError:
                    pass  # Do not recursively remove an unexpected entry.
            if incoming is not None:
                try:
                    incoming.unlink()
                except OSError:
                    pass
            self.lock.release()


def handler_for(receiver):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'PrivateDiagnostics'
        sys_version = ''

        def setup(self):
            super().setup()
            self.started = time.monotonic()
            def expire():
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            self.ceiling = threading.Timer(60, expire)
            self.ceiling.daemon = True
            self.ceiling.start()

        def finish(self):
            self.ceiling.cancel()
            super().finish()

        def log_message(self, *_):
            pass  # Never log URLs, private identities, credentials or body data.

        def reply(self, status, body):
            raw = json.dumps(body, separators=(',', ':')).encode('ascii')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(raw)))
            self.send_header('Cache-Control', 'private, no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            self.wfile.write(raw)

        def send_error(self, code, message=None, explain=None):
            # BaseHTTPRequestHandler otherwise reflects malformed request text.
            self.reply(code, {'schema': 1, 'error': 'request_rejected'})

        def do_POST(self):
            try:
                length, digest = receiver.headers(self.path, self.headers)
                status, result = receiver.receive(self.rfile, length, digest, deadline=self.started + 60)
                self.reply(status, result)
            except Rejected as error:
                self.reply(error.status, {'schema': 1, 'error': error.code})
            except (TimeoutError, socket.timeout):
                self.reply(408, {'schema': 1, 'error': 'receive_timeout'})
            except (OSError, ValueError, TypeError):
                self.reply(503, {'schema': 1, 'error': 'receiver_unavailable'})

        def do_GET(self):
            self.reply(404, {'schema': 1, 'error': 'not_found'})

        def do_HEAD(self):
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True

    return Handler


class BoundedServer(HTTPServer):
    request_queue_size = 2
    allow_reuse_address = False

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(10)
        return connection, address


@contextlib.contextmanager
def storage_process_lock(root):
    path = root / '.receiver.lock'
    if os.path.lexists(path):
        plain(path)
    with path.open('a+b') as stream:
        if stream.seek(0, os.SEEK_END) == 0:
            stream.write(b'0')
            stream.flush()
        stream.seek(0)
        if os.name == 'nt':
            import msvcrt
            try:
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise Rejected('receiver_already_running', 503) from error
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            try:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise Rejected('receiver_already_running', 503) from error
            try:
                yield
            finally:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--port', type=int, default=8770)
    args = parser.parse_args()
    try:
        require(1024 <= args.port <= 65535, 'receiver_port', 503)
        require(args.config.is_absolute() and PRIVATE_ROOT in args.config.parents,
                'private_config_required', 503)
        config = Config.from_dict(parse_json(read_small(args.config, 16 << 10)))
        require(config.expires > time.time(), 'authorization_expired', 410)
        receiver = Receiver(config)
        # Deliberately single-request: bounded sockets, no unbounded thread pool.
        with storage_process_lock(config.storage), BoundedServer(('127.0.0.1', args.port), handler_for(receiver)) as server:
            print(json.dumps({'event': 'diagnostic_receiver_ready', 'port': args.port,
                              'schema': 1, 'max_bytes': MAX_BODY}), flush=True)
            server.serve_forever()
    except (Rejected, OSError, ValueError, TypeError):
        print(json.dumps({'event': 'diagnostic_receiver_stopped', 'error': 'configuration_or_storage'}), flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
