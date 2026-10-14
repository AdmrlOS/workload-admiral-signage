"""Relay between pages and the persistent video engine (signage-video-engine).

Pages never name files: they reference a library/media URL; the relay resolves it to a
prepared engine variant whose source_sha256 matches the *current* original (image
builtins in /opt/signage/media/engine, data variants in /data/engine). One persistent
control connection to the engine; each page instance subscribes with an SSE stream and
its own client id. When a page's last stream closes (pagehide, navigation, crash) the
relay sends client-gone, and the engine hides+pauses if that page owned the video.
"""
import hashlib
import json
import os
import queue
import socket
import threading
import time
from pathlib import Path

ENGINE_SOCKET = os.environ.get('SIGNAGE_ENGINE_SOCKET', '/run/admiral-signage/video/engine.sock')
OWNER_COMMANDS = {'acquire', 'play', 'prepare', 'pause', 'resume', 'seek', 'show', 'hide', 'rect', 'stop', 'cancel', 'status'}
_SHA_CACHE = {}
_SHA_LOCK = threading.Lock()


def file_sha256(path):
    """SHA-256 cached by (path, size, mtime) so playlist annotation stays cheap."""
    stat = path.stat()
    key = (str(path), stat.st_size, stat.st_mtime_ns)
    with _SHA_LOCK:
        if key in _SHA_CACHE:
            return _SHA_CACHE[key]
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    value = digest.hexdigest()
    with _SHA_LOCK:
        if len(_SHA_CACHE) > 256:
            _SHA_CACHE.clear()
        _SHA_CACHE[key] = value
    return value


def variant_roots(data_root, app_root):
    return [Path(data_root) / 'engine', Path(app_root) / 'media' / 'engine']


def find_variant(original, data_root, app_root):
    """Return {'id','manifest'} for a prepared variant of `original`, or None with reason."""
    if original is None or not original.is_file():
        return None, 'missing'
    try:
        source = file_sha256(original)
    except OSError:
        return None, 'unreadable'
    for root in variant_roots(data_root, app_root):
        manifest = root / f'{original.stem}.engine.json'
        if not manifest.is_file():
            continue
        try:
            meta = json.loads(manifest.read_text())
        except (OSError, ValueError):
            continue
        if meta.get('source_sha256') == source and meta.get('engine_protocol') == 1:
            return {'id': manifest.name[:-len('.engine.json')], 'manifest': str(manifest),
                    'frames': meta.get('frames'), 'duration_ms': (meta.get('duration_ns') or 0) // 1_000_000}, None
    return None, 'no-prepared-variant'


class EngineRelay:
    def __init__(self, path=ENGINE_SOCKET, reconnect=0.5):
        self.path = path
        self.reconnect = reconnect
        self.lock = threading.Lock()
        self.sock = None
        self.seq = 0
        self.waiters = {}
        self.subscribers = {}
        self.ready = None
        self.stopped = False
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stopped = True
        with self.lock:
            if self.sock:
                try:
                    self.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    @property
    def connected(self):
        return self.sock is not None

    def _run(self):
        while not self.stopped:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(self.path)
            except OSError:
                time.sleep(self.reconnect)
                continue
            with self.lock:
                self.sock = sock
            buffer = b''
            try:
                while not self.stopped:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    buffer += chunk
                    while b'\n' in buffer:
                        line, buffer = buffer.split(b'\n', 1)
                        if line:
                            try:
                                self._dispatch(json.loads(line))
                            except ValueError:
                                pass
            except OSError:
                pass
            with self.lock:
                self.sock = None
                self.ready = None
                for waiter in self.waiters.values():
                    waiter['result'] = {'event': 'ack', 'ok': False, 'code': 'engine-disconnected'}
                    waiter['done'].set()
                self.waiters.clear()
            sock.close()
            self._broadcast({'event': 'engine-disconnected'})

    def _dispatch(self, event):
        if event.get('event') == 'engine-ready':
            self.ready = event
        if event.get('event') in ('ack', 'status') and isinstance(event.get('seq'), int):
            with self.lock:
                waiter = self.waiters.get(event['seq'])
            if waiter:
                if event['event'] == 'ack':
                    waiter['result'] = event
                    if not waiter['want_status'] or not event.get('ok'):
                        waiter['done'].set()
                else:
                    waiter['result'] = dict(waiter.get('result') or {}, status=event)
                    waiter['done'].set()
            if event['event'] == 'ack':
                return
        self._broadcast(event)

    def _broadcast(self, event):
        with self.lock:
            queues = [q for qs in self.subscribers.values() for q in qs]
        for q in queues:
            try:
                q.put_nowait(event)
            except queue.Full:
                pass  # a stalled page loses events; its stream is closed by the writer timeout

    def command(self, client, cmd, timeout=3.0, **fields):
        with self.lock:
            sock = self.sock
            if sock is None:
                return {'event': 'ack', 'ok': False, 'code': 'engine-unavailable', 'cmd': cmd}
            self.seq += 1
            seq = self.seq
            waiter = {'done': threading.Event(), 'result': None, 'want_status': cmd == 'status'}
            self.waiters[seq] = waiter
            line = json.dumps({**fields, 'cmd': cmd, 'seq': seq, 'client': client}) + '\n'
            try:
                sock.sendall(line.encode())
            except OSError:
                self.waiters.pop(seq, None)
                return {'event': 'ack', 'ok': False, 'code': 'engine-unavailable', 'cmd': cmd}
        if not waiter['done'].wait(timeout):
            result = {'event': 'ack', 'ok': False, 'code': 'timeout', 'cmd': cmd}
        else:
            result = waiter['result']
        with self.lock:
            self.waiters.pop(seq, None)
        return result

    def subscribe(self, client):
        q = queue.Queue(maxsize=512)
        with self.lock:
            self.subscribers.setdefault(client, []).append(q)
        if self.ready:
            q.put_nowait(self.ready)
        return q

    def unsubscribe(self, client, q):
        with self.lock:
            queues = self.subscribers.get(client, [])
            if q in queues:
                queues.remove(q)
            last = not queues
            if last:
                self.subscribers.pop(client, None)
        if last:
            self.command(client, 'client-gone', timeout=1.0)
