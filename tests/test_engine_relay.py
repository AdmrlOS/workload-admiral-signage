"""Engine relay + HTTP endpoints against a scripted engine on a real unix socket."""
import hashlib
import json
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
import server  # noqa: E402
import video_engine  # noqa: E402


class FakeEngine:
    """Speaks the engine line protocol: acks every command, records them."""

    def __init__(self, path):
        self.path = path
        self.commands = []
        self.conns = []
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(4)
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.conns.append(conn)
            conn.sendall(b'{"event":"engine-ready","warm":true,"alloc_queries":1}\n')
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        buffer = b''
        while True:
            try:
                chunk = conn.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buffer += chunk
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                cmd = json.loads(line)
                self.commands.append(cmd)
                ok = cmd['cmd'] != 'play' or cmd.get('asset', {}).get('id') != 'refuse'
                ack = {'event': 'ack', 'seq': cmd['seq'], 'cmd': cmd['cmd'], 'client': cmd.get('client'), 'ok': ok}
                if not ok:
                    ack['code'] = 'incompatible'
                conn.sendall((json.dumps(ack) + '\n').encode())
                if cmd['cmd'] == 'status':
                    conn.sendall((json.dumps({'event': 'status', 'seq': cmd['seq'], 'state': 'ready'}) + '\n').encode())
                if cmd['cmd'] == 'play' and ok:
                    conn.sendall((json.dumps({'event': 'started', 'asset': cmd['asset']['id'], 'play_id': 7}) + '\n').encode())

    def drop_all(self):
        for conn in self.conns:
            try:
                conn.shutdown(socket.SHUT_RDWR)  # Linux: close() alone does not wake the peer reader
            except OSError:
                pass
            conn.close()
        self.conns.clear()

    def close(self):
        try:
            self.listener.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.listener.close()
        self.drop_all()


def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return False


class EngineRelayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.engine = FakeEngine(str(self.root / 'e.sock'))
        self.relay = video_engine.EngineRelay(str(self.root / 'e.sock'), reconnect=0.05).start()
        self.assertTrue(wait_for(lambda: self.relay.connected and self.relay.ready))

    def tearDown(self):
        self.relay.stop()
        self.engine.close()
        self.temp.cleanup()

    def test_command_roundtrip_matches_seq_and_returns_status(self):
        ack = self.relay.command('page-1', 'play', asset={'id': 'promo-a'}, at='boundary')
        self.assertTrue(ack['ok'])
        self.assertEqual(self.engine.commands[-1]['client'], 'page-1')
        self.assertEqual(self.relay.command('page-1', 'play', asset={'id': 'refuse'})['code'], 'incompatible')
        status = self.relay.command('page-1', 'status')
        self.assertEqual(status['status']['state'], 'ready')

    def test_events_fan_out_and_last_stream_close_sends_client_gone(self):
        q1 = self.relay.subscribe('page-1')
        q2 = self.relay.subscribe('page-1')
        self.assertEqual(q1.get(timeout=1)['event'], 'engine-ready')  # late subscribers learn readiness
        self.relay.command('page-1', 'play', asset={'id': 'x'})
        self.assertEqual(q2.get(timeout=1)['event'], 'engine-ready')
        self.assertEqual(q2.get(timeout=1)['event'], 'started')
        self.relay.unsubscribe('page-1', q1)
        self.assertNotIn('client-gone', [c['cmd'] for c in self.engine.commands])
        self.relay.unsubscribe('page-1', q2)
        self.assertEqual(self.engine.commands[-1], {'cmd': 'client-gone', 'seq': self.engine.commands[-1]['seq'], 'client': 'page-1'})

    def test_engine_restart_fails_pending_and_reconnects(self):
        q = self.relay.subscribe('p')
        self.engine.drop_all()
        self.assertTrue(wait_for(lambda: any(e.get('event') == 'engine-disconnected' for e in list(q.queue))))
        self.assertTrue(wait_for(lambda: self.relay.connected and self.relay.ready))
        self.assertTrue(self.relay.command('p', 'pause')['ok'])

    def test_unavailable_engine_is_explicit(self):
        relay = video_engine.EngineRelay(str(self.root / 'missing.sock'))
        self.assertEqual(relay.command('p', 'play')['code'], 'engine-unavailable')


class VariantTests(unittest.TestCase):
    def test_variant_requires_current_source_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'app/media/engine').mkdir(parents=True)
            (root / 'data/media').mkdir(parents=True)
            original = root / 'data/media/promo-a.mp4'
            original.write_bytes(b'original')
            manifest = root / 'app/media/engine/promo-a.engine.json'
            manifest.write_text(json.dumps({'engine_protocol': 1, 'source_sha256': hashlib.sha256(b'original').hexdigest(),
                                            'frames': 600, 'duration_ns': 10_000_000_000}))
            found, why = video_engine.find_variant(original, root / 'data', root / 'app')
            self.assertEqual((found['id'], found['frames'], why), ('promo-a', 600, None))
            time.sleep(0.01)
            original.write_bytes(b'customer replaced the data copy')  # stale builtin variant must not be used
            found, why = video_engine.find_variant(original, root / 'data', root / 'app')
            self.assertEqual((found, why), (None, 'no-prepared-variant'))


class EngineEndpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        server.APP_ROOT = root / 'bundled'
        server.DATA_ROOT = root / 'data'
        for source in (server.APP_ROOT, server.DATA_ROOT):
            (source / 'www').mkdir(parents=True)
            (source / 'media').mkdir()
        (server.APP_ROOT / 'media/engine').mkdir()
        (server.APP_ROOT / 'media/promo-a.mp4').write_bytes(b'clip-a')
        (server.APP_ROOT / 'media/legacy.mp4').write_bytes(b'h264 only')
        (server.APP_ROOT / 'media/engine/promo-a.engine.json').write_text(json.dumps(
            {'engine_protocol': 1, 'source_sha256': hashlib.sha256(b'clip-a').hexdigest(), 'frames': 600, 'duration_ns': 10**10}))
        cls.engine = FakeEngine(str(root / 'e.sock'))
        server.ENGINE = video_engine.EngineRelay(str(root / 'e.sock'), reconnect=0.05).start()
        assert wait_for(lambda: server.ENGINE.ready)
        cls.http = server.ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
        cls.thread = threading.Thread(target=cls.http.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f'http://127.0.0.1:{cls.http.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()
        server.ENGINE.stop()
        server.ENGINE = None
        cls.engine.close()
        cls.temp.cleanup()

    def post(self, body, headers=None):
        request = Request(self.base + '/api/video/command', data=json.dumps(body).encode(), method='POST',
                          headers={'Content-Type': 'application/json', 'X-Signage-Request': 'player', **(headers or {})})
        try:
            with urlopen(request) as response:
                return response.status, json.loads(response.read())
        except HTTPError as error:
            return error.code, json.loads(error.read())

    def test_play_resolves_url_to_engine_manifest_never_a_page_path(self):
        status, ack = self.post({'client': 'c1', 'cmd': 'play', 'asset': {'url': '/media/promo-a.mp4'}, 'at': 'boundary'})
        self.assertEqual((status, ack['ok']), (200, True))
        sent = self.engine.commands[-1]
        self.assertEqual(sent['asset']['id'], 'promo-a')
        self.assertTrue(sent['asset']['manifest'].endswith('media/engine/promo-a.engine.json'))
        status, ack = self.post({'client': 'c1', 'cmd': 'play', 'asset': {'url': '/media/promo-a.mp4', 'manifest': '/etc/passwd'}})
        self.assertNotEqual(self.engine.commands[-1]['asset']['manifest'], '/etc/passwd')

    def test_legacy_asset_is_reported_not_engine_playable(self):
        status, ack = self.post({'client': 'c1', 'cmd': 'play', 'asset': {'url': '/media/legacy.mp4'}})
        self.assertEqual((status, ack['code']), (409, 'no-prepared-variant'))

    def test_cross_origin_and_unknown_commands_rejected(self):
        status, _ = self.post({'client': 'c1', 'cmd': 'pause'}, headers={'X-Signage-Request': ''})
        self.assertEqual(status, 403)
        status, _ = self.post({'client': 'c1', 'cmd': 'client-gone'})
        self.assertEqual(status, 400)
        status, _ = self.post({'client': '', 'cmd': 'pause'})
        self.assertEqual(status, 400)

    def test_playlist_annotates_engine_variants(self):
        content_dir = server.DATA_ROOT
        (content_dir / 'library.json').write_text(json.dumps({'revision': 1, 'demo_catalog': 99, 'assets': [
            {'id': 'promo-a', 'name': 'A', 'kind': 'video', 'url': '/media/promo-a.mp4', 'size': 6},
            {'id': 'legacy', 'name': 'L', 'kind': 'video', 'url': '/media/legacy.mp4', 'size': 9}],
            'entries': [{'id': 'e1', 'asset_id': 'promo-a', 'duration': 0, 'enabled': True, 'start': None, 'end': None},
                        {'id': 'e2', 'asset_id': 'legacy', 'duration': 0, 'enabled': True, 'start': None, 'end': None}]}))
        with urlopen(self.base + '/api/playlist') as response:
            data = json.loads(response.read())
        engine = {e['asset']['id']: e['asset'].get('engine') for e in data['entries']}
        self.assertEqual(set(engine), {'promo-a', 'legacy'})
        self.assertEqual(engine.get('promo-a', {}).get('id'), 'promo-a')
        self.assertIsNone(engine.get('legacy'))

    def test_sse_stream_delivers_events_and_disconnect_releases_owner(self):
        sock = socket.create_connection(('127.0.0.1', self.http.server_port))
        sock.sendall(b'GET /api/video/events?client=sse-1 HTTP/1.1\r\nHost: x\r\nX-Signage-Request: player\r\n\r\n')
        sock.settimeout(3)
        data = b''
        while b'engine-ready' not in data:
            data += sock.recv(4096)
        self.assertIn(b'text/event-stream', data)
        self.post({'client': 'sse-1', 'cmd': 'play', 'asset': {'url': '/media/promo-a.mp4'}})
        while b'"started"' not in data:
            data += sock.recv(4096)
        sock.close()
        self.assertTrue(wait_for(lambda: any(c['cmd'] == 'client-gone' and c['client'] == 'sse-1' for c in self.engine.commands)))


if __name__ == '__main__':
    unittest.main()
