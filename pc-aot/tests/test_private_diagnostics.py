"""Offline receiver/schema tests. Authored data; no sockets, services or USB."""
import copy
from email.message import Message
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('private_diagnostic_receiver', ROOT / 'scripts/receive_private_diagnostics.py')
d = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = d
spec.loader.exec_module(d)


def timing():
    return ('IOS_TIMING schema=1 approximate_window=1 end_ns=5000000000 window_ns=5000000000 '
            'kind=frame_gap samples=1 total_ns=150000000 max_ns=150000000 failures_known=0 '
            'failures=0 sleep_calls=0 bins=' + ','.join('1' if n == 14 else '0' for n in range(20)))


def authored_bundle():
    return {'schema': 1, 'report_id': 'a' * 32, 'app_build': 12, 'sessions': [
        {'slot': 'current', 'build': 12, 'capture': 'live_prefix', 'snapshot_bytes': 3000,
         'excluded_lines': 4, 'lines': [timing(), 'IOS_DIAG_SETTING key=13 value=2.5',
             'IOS_PRESENT failure=0x88760868', 'IOS_DIAG_ERROR code=1',
             'IOS_LIFECYCLE schema=1 event=6 generation=3 phase=4 active_calls=0 requested=0 '
             'monotonic_ns=5000000000 elapsed_ns=4000000 result=-1']},
        {'slot': 'previous', 'build': 11, 'capture': 'closed', 'snapshot_bytes': 1500,
         'excluded_lines': 2, 'lines': ['IOS_GAME_LANGUAGE requested_text=1 text=1 text_reason=0 '
                                       'requested_voices=0 voices=0 voices_reason=0']}]}


class ReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = d.Schema()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        # macOS may return /var/folders under the OS /var -> /private/var alias.
        # Canonicalize this test-owned root; receiver anti-link checks stay strict.
        self.private = Path(self.temp.name).resolve() / 'private'
        self.private.mkdir()
        self.scope = patch.object(d, 'PRIVATE_ROOT', self.private)
        self.scope.start()
        self.config = d.Config.from_dict({'storage_dir': str(self.private / 'received'),
            'tailscale_login': 'authored@example.invalid', 'token': 'fixture_' + 't' * 40,
            'expires_unix': int(time.time()) + 600})
        self.receiver = d.Receiver(self.config, self.schema)
        self.bundle = authored_bundle()

    def tearDown(self):
        self.scope.stop()
        self.temp.cleanup()

    def encoded(self, bundle=None):
        return json.dumps(self.bundle if bundle is None else bundle, separators=(',', ':')).encode()

    def receive(self, raw=None, receiver=None):
        raw = self.encoded() if raw is None else raw
        return (receiver or self.receiver).receive(io.BytesIO(raw), len(raw), hashlib.sha256(raw).hexdigest())

    def headers(self, raw=None):
        raw = self.encoded() if raw is None else raw
        headers = Message()
        for key, value in {'Tailscale-User-Login': self.config.login,
                           'Authorization': 'Bearer ' + self.config.token,
                           'Content-Length': str(len(raw)), 'Content-Type': 'application/json',
                           'X-Content-SHA256': hashlib.sha256(raw).hexdigest()}.items():
            headers[key] = value
        return headers

    def assert_no_published_or_partial(self):
        self.assertEqual(list(self.config.storage.iterdir()), [])

    def test_durable_report_numeric_logs_and_existing_parser(self):
        status, receipt = self.receive()
        self.assertEqual(status, 201)
        self.assertEqual(set(receipt), {'schema', 'report_id', 'bytes', 'sha256'})
        record = self.receiver.existing(receipt['report_id'])
        folder = self.config.storage / record['directory']
        self.assertEqual((folder / 'bundle.json').read_bytes(), self.encoded())
        parsed = self.schema.timing.summarize((folder / 'current.log').read_text().splitlines())
        self.assertEqual(parsed['timing_records'], 1)
        self.assertEqual(parsed['invalid_records'], 0)
        self.assertEqual(record['sessions'][0]['capture'], 'live_prefix')
        self.assertEqual(record['sessions'][1]['build'], 11)
        self.assertFalse(list(self.config.storage.glob('.incoming-*')))
        for stage in ('runtime-start', 'guest-memory-bound', 'guest-entry', 'vulkan-window-ready'):
            self.schema.line('IOS_STAGE ' + stage + ' monotonic=1.000000000')
        for slot in (0, 9, 12):
            self.schema.line(f'IOS_CAMERA_STATE slot={slot}')

    def test_same_id_replay_idempotent_and_conflicting_content_never_overwritten(self):
        _, first = self.receive()
        before = sorted(p.name for p in self.config.storage.iterdir())
        status, again = self.receive()
        self.assertEqual((status, again), (200, first))
        self.assertEqual(sorted(p.name for p in self.config.storage.iterdir()), before)
        self.bundle['sessions'][0]['excluded_lines'] += 1
        with self.assertRaisesRegex(d.Rejected, 'report_id_conflict') as error:
            self.receive()
        self.assertEqual(error.exception.status, 409)
        self.assertEqual(self.receiver.existing(first['report_id'])['sha256'], first['sha256'])

    def test_identity_token_expiry_routes_and_duplicate_headers(self):
        for route in d.ROUTES:
            self.assertEqual(self.receiver.headers(route, self.headers())[0], len(self.encoded()))
        for key, value in (('Authorization', 'Bearer wrong'), ('Tailscale-User-Login', 'other@example.invalid')):
            headers = self.headers(); headers.replace_header(key, value)
            with self.assertRaisesRegex(d.Rejected, 'not_found'):
                self.receiver.headers(d.ROUTES[0], headers)
        for path in ('/skyrim-ios/v1/reports', '/v1/reports?token=secret', '/v1/reports/../reports',
                     'https://elsewhere.invalid/v1/reports'):
            with self.assertRaisesRegex(d.Rejected, 'not_found'):
                self.receiver.headers(path, self.headers())
        for key in ('Authorization', 'Content-Length', 'X-Content-SHA256', 'Content-Type'):
            headers = self.headers(); headers[key] = headers[key]
            with self.assertRaises(d.Rejected):
                self.receiver.headers(d.ROUTES[0], headers)
        with patch.object(d.time, 'time', return_value=self.config.expires):
            with self.assertRaisesRegex(d.Rejected, 'authorization_expired'):
                self.receiver.headers(d.ROUTES[0], self.headers())
        self.assert_no_published_or_partial()

    def test_token_bounds_and_current_session_identity_contract(self):
        configuration = {'storage_dir': str(self.config.storage), 'tailscale_login': self.config.login,
                         'token': self.config.token, 'expires_unix': self.config.expires}
        for length in (43, 128):
            self.assertEqual(len(d.Config.from_dict({**configuration, 'token': 't' * length}).token), length)
        for length in (32, 42, 129):
            with self.assertRaisesRegex(d.Rejected, 'receiver_token_configuration'):
                d.Config.from_dict({**configuration, 'token': 't' * length})
        variants = []
        candidate = copy.deepcopy(self.bundle); candidate['sessions'] = candidate['sessions'][1:]
        variants.append((candidate, 'current_session_required'))
        candidate = copy.deepcopy(self.bundle); candidate['sessions'][0]['build'] = 11
        variants.append((candidate, 'current_build_mismatch'))
        for index, capture in ((0, 'closed'), (1, 'live_prefix')):
            candidate = copy.deepcopy(self.bundle); candidate['sessions'][index]['capture'] = capture
            variants.append((candidate, 'session_capture'))
        for candidate, reason in variants:
            with self.subTest(reason=reason), self.assertRaisesRegex(d.Rejected, reason):
                self.receive(self.encoded(candidate))
            self.assert_no_published_or_partial()

    def test_encoding_sizes_and_no_body_read_before_auth(self):
        for key, value in (('Transfer-Encoding', 'chunked'), ('Content-Encoding', 'gzip'),
                           ('Content-Length', str(d.MAX_BODY + 1)), ('Content-Type', 'application/zip')):
            headers = self.headers()
            if key in headers: headers.replace_header(key, value)
            else: headers[key] = value
            with self.assertRaises(d.Rejected):
                self.receiver.headers(d.ROUTES[0], headers)
        handler = d.handler_for(self.receiver).__new__(d.handler_for(self.receiver))
        handler.path = d.ROUTES[0]
        handler.headers = self.headers(); del handler.headers['Authorization']
        class NoRead:
            def read(self, _): raise AssertionError('body was read before authentication')
        handler.rfile = NoRead()
        replied = []
        handler.reply = lambda status, body: replied.append((status, body))
        handler.do_POST()
        self.assertEqual(replied, [(404, {'schema': 1, 'error': 'not_found'})])
        replied.clear()
        handler.send_error(400, 'authored private request text', 'authored private details')
        self.assertEqual(replied, [(400, {'schema': 1, 'error': 'request_rejected'})])
        self.assert_no_published_or_partial()

    def test_partial_hash_invalid_json_timeout_leave_no_completion(self):
        raw = self.encoded()
        operations = (
            lambda: self.receiver.receive(io.BytesIO(raw[:25]), len(raw), hashlib.sha256(raw).hexdigest()),
            lambda: self.receiver.receive(io.BytesIO(raw), len(raw), '0' * 64),
            lambda: self.receive(b'{"schema":1,"schema":1}'),
            lambda: self.receive(b'{"schema":NaN}'),
            lambda: self.receiver.receive(io.BytesIO(raw), len(raw), hashlib.sha256(raw).hexdigest(), deadline=0))
        for operation in operations:
            with self.subTest(operation=operation):
                with self.assertRaises(d.Rejected): operation()
                self.assert_no_published_or_partial()
                self.assertTrue(self.receiver.lock.acquire(blocking=False))
                self.receiver.lock.release()

    def test_unknown_text_paths_fields_control_characters_and_semantic_errors_rejected(self):
        bad_lines = ['Game input: /private/game', 'IOS_TIMING secret=private',
                     'IOS_PRESENT failure=0x88760868 extra=secret', 'IOS_DIAG_SETTING key=19 value=1',
                     'IOS_DIAG_SETTING key=1 value=1000000001', 'IOS_DIAG_SETTING key=1 value=nan',
                     'IOS_DIAG_ERROR code=0', 'IOS_DIAG_ERROR code=1\nGame input: private',
                     'IOS_STAGE private-directory monotonic=1.000000000', 'IOS_CAMERA_STATE slot=13',
                     'IOS_CAMERA_STATE slot=12 camera=/private/address',
                     timing().replace('samples=1', 'samples=2'),
                     self.bundle['sessions'][0]['lines'][-1].replace('result=-1', 'result=2147483648')]
        for line in bad_lines:
            candidate = copy.deepcopy(self.bundle)
            candidate['sessions'][0]['lines'] = [line]
            with self.subTest(line=line.split(' ', 1)[0]), self.assertRaises(d.Rejected):
                self.receive(self.encoded(candidate))
            self.assert_no_published_or_partial()
        candidate = copy.deepcopy(self.bundle); candidate['filename'] = '../not-allowed'
        with self.assertRaisesRegex(d.Rejected, 'bundle_fields'):
            self.receive(self.encoded(candidate))
        candidate = copy.deepcopy(self.bundle); candidate['sessions'][0]['slot'] = '../escape'
        with self.assertRaisesRegex(d.Rejected, 'session_slot'):
            self.receive(self.encoded(candidate))
        self.assert_no_published_or_partial()

    def test_exact_state_policy_and_wait_are_accepted_without_private_fields(self):
        row = timing().replace('kind=frame_gap', 'kind=graphics_state_wait')
        context = row.replace('IOS_TIMING ', 'IOS_TIMING_CONTEXT ') + ' role=1 source=0 result=1'
        candidate = copy.deepcopy(self.bundle)
        candidate['sessions'][0]['lines'] = [row, context,
            'IOS_SHADER_OPTION option=5 enabled=1',
            'IOS_GRAPHICS_POLICY schema=1 exact_state=1 wait_scope=1']
        self.schema.bundle(self.encoded(candidate))
        for bad in (context.replace('source=0', 'source=1'),
                    'IOS_SHADER_OPTION option=6 enabled=1',
                    'IOS_GRAPHICS_POLICY schema=1 exact_state=1 wait_scope=0',
                    'IOS_GRAPHICS_POLICY schema=1 exact_state=1 wait_scope=1 key=private'):
            with self.subTest(row=bad), self.assertRaises(d.Rejected):
                self.schema.line(bad)

    def test_two_session_line_and_byte_limits_are_not_silently_truncated(self):
        candidate = copy.deepcopy(self.bundle)
        candidate['sessions'].append(copy.deepcopy(candidate['sessions'][0]))
        with self.assertRaisesRegex(d.Rejected, 'session_count'): self.schema.bundle(self.encoded(candidate))
        candidate = copy.deepcopy(self.bundle); candidate['sessions'][0]['lines'] = ['IOS_DIAG_ERROR code=1'] * (d.MAX_LINES + 1)
        with self.assertRaisesRegex(d.Rejected, 'line_count'): self.schema.bundle(self.encoded(candidate))
        candidate = copy.deepcopy(self.bundle)
        candidate['sessions'][0]['lines'] = [timing()] * (d.MAX_SESSION_BYTES // (len(timing()) + 1) + 1)
        with self.assertRaisesRegex(d.Rejected, 'session_byte_limit'): self.schema.bundle(self.encoded(candidate))
        self.assert_no_published_or_partial()

    def test_publication_failure_cleans_only_request_owned_files(self):
        real_link = d.os.link
        def fail_manifest(source, target):
            if Path(target).name.startswith('report-'):
                raise OSError('authored publication error')
            return real_link(source, target)
        with patch.object(d.os, 'link', side_effect=fail_manifest), self.assertRaises(OSError):
            self.receive()
        self.assert_no_published_or_partial()
        self.assertEqual(self.receive()[0], 201)

    def test_lost_publication_reply_preserves_committed_bundle_and_replays(self):
        real_link = d.os.link
        def link_then_interrupt(source, target):
            real_link(source, target)
            if Path(target).name.startswith('report-'):
                raise InterruptedError('authored lost completion')
        with patch.object(d.os, 'link', side_effect=link_then_interrupt), self.assertRaises(InterruptedError):
            self.receive()
        self.assertEqual(self.receive()[0], 200)
        self.assertEqual(len(list(self.config.storage.glob('bundle-*'))), 1)

    def test_concurrent_same_id_publication_has_one_immutable_winner(self):
        barrier, results, errors = threading.Barrier(2), [], []
        real_link = d.os.link
        def competing_link(source, target):
            if Path(target).name.startswith('report-'):
                barrier.wait(timeout=5)
            return real_link(source, target)
        def work():
            try: results.append(self.receive(receiver=d.Receiver(self.config, self.schema)))
            except BaseException as error: errors.append(error)
        with patch.object(d.os, 'link', side_effect=competing_link):
            workers = [threading.Thread(target=work) for _ in range(2)]
            for worker in workers: worker.start()
            for worker in workers: worker.join(timeout=10)
        self.assertEqual(errors, [])
        self.assertEqual(sorted(status for status, _ in results), [200, 201])
        self.assertEqual(results[0][1], results[1][1])
        self.assertEqual(len(list(self.config.storage.glob('report-*.json'))), 1)
        self.assertEqual(len(list(self.config.storage.glob('bundle-*'))), 1)

    def test_quota_or_stored_corruption_fail_without_overwrite(self):
        with patch.object(d, 'MAX_STORAGE', 32), self.assertRaisesRegex(d.Rejected, 'receiver_full'):
            self.receive()
        self.assert_no_published_or_partial()
        self.receive()
        record = self.receiver.existing(self.bundle['report_id'])
        path = self.config.storage / record['directory'] / 'current.log'
        original = path.read_bytes(); path.write_bytes(b'x' + original[1:])
        with self.assertRaisesRegex(d.Rejected, 'stored_file_hash'):
            self.receive()
        self.assertEqual(path.read_bytes(), b'x' + original[1:])

    def test_storage_scope_links_and_second_process_lock(self):
        with self.assertRaisesRegex(d.Rejected, 'private_storage_scope'):
            d.private_directory(Path(self.temp.name) / 'outside', create=True)
        with d.storage_process_lock(self.config.storage):
            with self.assertRaisesRegex(d.Rejected, 'receiver_already_running'):
                with d.storage_process_lock(self.config.storage): pass
        with d.storage_process_lock(self.config.storage): pass
        bad = self.config.storage / ('report-' + 'b' * 32 + '.json')
        try:
            bad.symlink_to(self.config.storage / '.receiver.lock')
        except OSError:
            self.skipTest('Symlink fixture requires local symlink capability')
        with self.assertRaisesRegex(d.Rejected, 'storage_link_or_type'):
            self.receiver.existing('b' * 32)


if __name__ == '__main__':
    unittest.main()
