"""Persistent single-decoder engine: real engine binary, real h265parse + decoder.

Runs where GStreamer is available (tests/run-native-tests.sh runs it in the ARM64
toolchain image with SIGNAGE_ENGINE_BIN set; it is skipped elsewhere). The decoder is
avdec_h265 and the sink the engine's TEST ONLY hashing appsink: acknowledgements here
prove pipeline/timeline/pixel correctness, never physical scanout.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE = os.environ.get('SIGNAGE_ENGINE_BIN')
SMALL = ['--width', '320', '--height', '180', '--fps', '60', '--preset', 'ultrafast']
FRAME = 1_000_000_000 / 60


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-y', *map(str, args)], check=True)


def prepare(source, out, name, *extra):
    subprocess.run([sys.executable, str(ROOT / 'scripts/prepare-engine-asset.py'), str(source), '--out-dir', str(out),
                    '--name', name, '--reference', *SMALL, *extra], check=True, capture_output=True)
    ref = [line.rsplit(',', 1)[1].strip() for line in (out / f'{name}.framemd5').read_text().splitlines()
           if line and not line.startswith('#')]
    return out / f'{name}.engine.json', ref


class Assets:
    dir = None

    @classmethod
    def build(cls):
        if cls.dir:
            return cls
        cls.dir = Path(tempfile.mkdtemp(prefix='engine-assets-'))
        d = cls.dir
        ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30', '-t', '2', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', d / 'a.mp4')
        ffmpeg('-f', 'lavfi', '-i', 'mandelbrot=size=640x360:rate=25', '-t', '2', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', d / 'b.mp4')
        ffmpeg('-f', 'lavfi', '-i', 'smptebars=size=352x288:rate=60', '-t', '1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', d / 'c.mp4')
        cls.a, cls.ref_a = prepare(d / 'a.mp4', d, 'a')
        cls.b, cls.ref_b = prepare(d / 'b.mp4', d, 'b')
        cls.c, _ = prepare(d / 'c.mp4', d, 'c', '--width', '352', '--height', '288')
        assert len(cls.ref_a) == 120 and len(cls.ref_b) == 120 and set(cls.ref_a).isdisjoint(cls.ref_b)
        return cls


class Engine:
    def __init__(self, workdir, assets, **env):
        self.dir = workdir
        self.sock_path = str(workdir / 'engine.sock')
        self.hash_path = workdir / 'hashes.txt'
        environ = dict(os.environ, SIGNAGE_ENGINE_SOCKET=self.sock_path, SIGNAGE_ENGINE_SINK='test',
                       SIGNAGE_ENGINE_DECODER='avdec_h265', SIGNAGE_ENGINE_TEST_HASH=str(self.hash_path),
                       SIGNAGE_ENGINE_WIDTH='320', SIGNAGE_ENGINE_HEIGHT='180', SIGNAGE_ENGINE_FPS='60',
                       SIGNAGE_ENGINE_ASSET_ROOTS=str(assets.dir), SIGNAGE_ENGINE_WARMUP=str(assets.a),
                       GST_DEBUG=os.environ.get('GST_DEBUG', '1'))
        environ.update({k: str(v) for k, v in env.items()})
        self.log = open(workdir / 'engine.log', 'w')
        self.proc = subprocess.Popen([ENGINE], env=environ, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 20
        while not os.path.exists(self.sock_path):
            if self.proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('engine did not start: ' + (workdir / 'engine.log').read_text()[-3000:])
            time.sleep(0.02)
        self.connect()
        self.seq = 0

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.sock_path)
        self.sock.settimeout(0.05)
        self.buf = b''
        self.events = []

    def pump(self, timeout=0.05):
        end = time.monotonic() + timeout
        while True:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.buf += chunk
            except socket.timeout:
                pass
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                if line:
                    self.events.append(json.loads(line))
            if time.monotonic() >= end:
                break

    def send(self, cmd, **fields):
        self.seq += 1
        message = {'cmd': cmd, 'seq': self.seq, **fields}
        self.sock.sendall((json.dumps(message) + '\n').encode())
        return self.wait(lambda e: e.get('event') == 'ack' and e.get('seq') == self.seq, 10)

    def wait(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            for event in self.events[seen:]:
                if predicate(event):
                    return event
            seen = len(self.events)
            self.pump()
        raise AssertionError(f'timeout; events={self.events[-12:]}\nlog={(self.dir / "engine.log").read_text()[-2000:]}')

    def event(self, name, timeout=10, **match):
        return self.wait(lambda e: e.get('event') == name and all(e.get(k) == v for k, v in match.items()), timeout)

    def status(self):
        self.send('status')
        return self.event('status', seq=self.seq)

    def hashes(self):
        if not self.hash_path.exists():
            return []
        return [tuple(line.split()) for line in self.hash_path.read_text().splitlines() if line]

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                raise AssertionError('engine did not exit on SIGTERM')
        self.log.close()
        return self.proc.returncode


def asset(path, name):
    return {'id': name, 'manifest': str(path)}


class EngineCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.assets = Assets.build()

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix='engine-run-'))
        self.engine = Engine(self.work, self.assets, **getattr(self, 'env', {}))
        self.ready = self.engine.event('engine-ready', timeout=30)
        self.engine.send('acquire', client='page-1')

    def tearDown(self):
        code = self.engine.close()
        log = (self.work / 'engine.log').read_text()
        self.assertNotIn('ERROR: AddressSanitizer', log)
        self.assertNotIn('ERROR: LeakSanitizer', log)
        self.assertNotIn('runtime error', log)
        self.assertEqual(code, 0, log[-3000:])
        if not os.environ.get('SIGNAGE_KEEP_ENGINE_RUNS'):
            shutil.rmtree(self.work, True)

    def play(self, name, **fields):
        path = {'a': self.assets.a, 'b': self.assets.b, 'c': self.assets.c}[name]
        return self.engine.send('play', client='page-1', asset=asset(path, name), **fields)

    def assert_continuous(self, hashes):
        pts = [int(p) for p, _ in hashes]
        steps = {b - a for a, b in zip(pts, pts[1:])}
        self.assertTrue(steps <= {16666666, 16666667}, f'timeline gaps/repeats: {sorted(steps)[:8]}')

    def play_as(self, client, name):
        return self.engine.send('play', client=client, asset=asset(getattr(self.assets, name), name))


@unittest.skipUnless(ENGINE and shutil.which('ffmpeg'), 'set SIGNAGE_ENGINE_BIN (tests/run-native-tests.sh)')
class VideoEngineTests(EngineCase):
    # ------------------------------------------------------------------ startup
    def test_startup_preflight_warms_decoder_invisibly(self):
        self.assertTrue(self.ready['warm'])
        self.assertEqual(self.ready['alloc_queries'], 1)
        self.assertEqual(self.ready['caps_events'], 1)
        time.sleep(0.3)
        self.assertEqual(self.engine.hashes(), [], 'warm-up frames must never be shown')
        status = self.engine.status()
        self.assertFalse(status['visible'])
        self.assertEqual(status['cache']['used'], (self.assets.dir / 'a.hevc').stat().st_size)

    # ------------------------------------------------------------------ gapless timeline
    def test_dynamic_gapless_handoffs_preserve_every_frame_and_one_pool(self):
        e = self.engine
        self.assertTrue(self.play('a')['ok'])
        e.event('started', asset='a')
        self.assertTrue(self.play('b', at='boundary')['ok'])  # decided while A plays: not a fixed sequence
        e.event('started', asset='b', timeout=5)
        self.assertTrue(self.play('a', at='boundary')['ok'])
        end = e.event('ended', asset='a', reason='complete', timeout=8)
        hashes = e.hashes()
        self.assertEqual([h for _, h in hashes], self.assets.ref_a + self.assets.ref_b + self.assets.ref_a)
        self.assert_continuous(hashes)
        ended = [ev for ev in e.events if ev.get('event') == 'ended']
        self.assertEqual([(ev['asset'], ev['presented'], ev['frames']) for ev in ended],
                         [('a', 120, 120), ('b', 120, 120), ('a', 120, 120)])
        self.assertEqual(end['fed'], 120)
        status = e.status()
        self.assertEqual((status['alloc_queries'], status['caps_events']), (1, 1), 'decoder pool must never renegotiate')
        self.assertEqual(status['presented_total'], 360)

    def test_same_clip_loop_has_no_end_and_no_gap(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('looped', asset='a', loop_index=2, timeout=8)
        hashes = e.hashes()
        self.assertGreaterEqual(len(hashes), 241)
        self.assertEqual([h for _, h in hashes[:240]], self.assets.ref_a * 2)
        self.assertEqual(hashes[240][1], self.assets.ref_a[0])
        self.assert_continuous(hashes)
        self.assertFalse([ev for ev in e.events if ev.get('event') == 'ended' and ev.get('reason') == 'complete'])

    def test_gop_switch_lands_on_next_idr(self):
        e = self.engine
        self.play('a')
        e.event('started', asset='a')
        time.sleep(0.1)
        self.play('b', at='gop')
        e.event('started', asset='b', timeout=5)
        e.event('ended', asset='b', reason='complete', timeout=5)
        seq = [h for _, h in e.hashes()]
        cut = seq.index(self.assets.ref_b[0])
        self.assertEqual(cut % 60, 0, 'switch must happen at a GOP boundary of the outgoing clip')
        self.assertEqual(seq, self.assets.ref_a[:cut] + self.assets.ref_b)
        self.assert_continuous(e.hashes())

    def test_now_switch_flushes_inside_the_same_pipeline(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        for _ in range(5):
            time.sleep(0.15)
            self.play('b', at='now')
            e.event('started', asset='b', timeout=5)
            time.sleep(0.15)
            self.play('a', at='now', loop=True)
            e.event('started', asset='a', timeout=5)
        known = set(self.assets.ref_a) | set(self.assets.ref_b)
        self.assertTrue(all(h in known for _, h in e.hashes()), 'no corrupt/partial frame after flush')
        status = e.status()
        self.assertEqual((status['alloc_queries'], status['caps_events']), (1, 1))

    # ------------------------------------------------------------------ controls
    def test_pause_resume_continues_with_the_next_frame(self):
        e = self.engine
        self.play('a')
        e.event('started', asset='a')
        time.sleep(0.5)
        e.send('pause', client='page-1')
        time.sleep(0.2)
        paused = len(e.hashes())
        time.sleep(0.5)
        self.assertEqual(len(e.hashes()), paused, 'frames advanced while paused')
        e.send('resume', client='page-1')
        e.event('ended', asset='a', reason='complete', timeout=5)
        self.assertEqual([h for _, h in e.hashes()], self.assets.ref_a)

    def test_seek_is_frame_accurate_from_the_gop(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        time.sleep(0.2)
        for position_ms, frame in ((1000, 60), (500, 30), (1750, 105)):
            before = len(e.hashes())
            self.assertTrue(e.send('seek', client='page-1', position_ms=position_ms)['ok'])
            # The seeked instance starts a new, strictly later timeline: the first frame after
            # the PTS jump must be exactly the requested frame (frames before it in the GOP are
            # decoded as references but clipped), and nothing older may follow it.
            deadline, jump = time.monotonic() + 5, None
            while jump is None and time.monotonic() < deadline:
                hashes = e.hashes()
                pts = [int(p) for p, _ in hashes]
                jump = next((i for i in range(max(before, 1), len(pts)) if pts[i] - pts[i - 1] > 20_000_000), None)
                time.sleep(0.02)
            self.assertIsNotNone(jump, f'no post-seek frame for {position_ms} ms')
            self.assertEqual(hashes[jump][1], self.assets.ref_a[frame], f'seek {position_ms} ms')
            time.sleep(0.15)
            later = [int(p) for p, _ in e.hashes()[jump:]]
            self.assertEqual(later, sorted(later))
            self.assertTrue(all(p >= later[0] for p in later), 'pre-seek frame shown after the flush')
        self.assertEqual(e.status()['alloc_queries'], 1)

    def test_hide_show_and_rect(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        self.assertTrue(e.send('rect', client='page-1', x=100, y=50, w=1280, h=720)['ok'])
        self.assertFalse(e.send('rect', client='page-1', x=0, y=0, w=0, h=10)['ok'])
        e.send('hide', client='page-1')
        time.sleep(0.15)
        hidden = len(e.hashes())
        time.sleep(0.4)
        self.assertEqual(len(e.hashes()), hidden)
        status = e.status()
        self.assertEqual((status['visible'], status['playing'], status['rect']), (False, False, [100, 50, 1280, 720]))
        e.send('show', client='page-1')
        time.sleep(0.4)
        self.assertGreater(len(e.hashes()), hidden)
        seq = [h for _, h in e.hashes()]
        n = len(seq)
        self.assertEqual(seq, (self.assets.ref_a * 4)[:n])

    # ------------------------------------------------------------------ ownership / lifecycle
    def test_preemption_and_page_gone(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        ack = e.send('acquire', client='page-2')
        self.assertTrue(ack['ok'])
        e.event('preempted', client='page-1')
        self.assertEqual(e.send('pause', client='page-1')['code'], 'not-owner')
        self.assertTrue(e.send('client-gone', client='page-1')['ok'])
        self.assertTrue(e.status()['playing'])
        e.send('client-gone', client='page-2')
        e.event('owner-gone')
        status = e.status()
        self.assertEqual((status['visible'], status['playing'], status['owner']), (False, False, ''))
        self.assertEqual(status['alloc_queries'], 1, 'pagehide keeps the decoder')

    def test_controller_disconnect_hides_and_keeps_engine(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        e.sock.close()
        time.sleep(0.3)
        e.connect()
        status = e.status()
        self.assertEqual((status['visible'], status['playing']), (False, False))
        e.send('acquire', client='page-9')
        self.play_as('page-9', 'b')
        e.event('started', asset='b', timeout=5)

    def test_explicit_release_frees_decoder_and_acquire_rewarms(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        self.assertTrue(e.send('release', client='page-1')['ok'])
        e.event('released')
        status = e.status()
        self.assertEqual((status['state'], status['connected']), ('released', False))
        self.assertEqual(e.send('play', client='page-1', asset=asset(self.assets.a, 'a'))['code'], 'not-ready')
        before = len(e.events)
        self.assertTrue(e.send('acquire', client='page-1')['ok'])
        e.wait(lambda ev: ev.get('event') == 'engine-ready' and e.events.index(ev) >= before, 30)
        self.assertTrue(self.play('b')['ok'])
        e.event('started', asset='b', timeout=5)

    # ------------------------------------------------------------------ asset validation
    def test_incompatible_forbidden_corrupt_assets_are_rejected_without_crash(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        ack = self.play('c', at='boundary')
        self.assertEqual((ack['ok'], ack['code']), (False, 'incompatible'))
        outside = self.work / 'x.engine.json'
        shutil.copy(self.assets.a, outside)
        self.assertEqual(e.send('prepare', client='page-1', asset={'id': 'x', 'manifest': str(outside)})['code'], 'forbidden')
        bad = self.assets.dir / 'corrupt'
        bad.mkdir(exist_ok=True)
        manifest = json.loads(self.assets.b.read_text())
        data = bytearray((self.assets.dir / 'b.hevc').read_bytes())
        data[5000] ^= 0xFF
        (bad / 'b.hevc').write_bytes(bytes(data))
        (bad / 'b.engine.json').write_text(json.dumps(manifest))
        self.assertTrue(e.send('prepare', client='page-1', asset={'id': 'bad', 'manifest': str(bad / 'b.engine.json')})['ok'])
        self.assertEqual(e.event('error', asset='bad')['code'], 'integrity')
        time.sleep(0.3)
        self.assertTrue(e.status()['playing'])

    def test_cache_budget_is_enforced(self):
        e = self.engine
        e.send('prepare', client='page-1', asset=asset(self.assets.b, 'b'))
        e.event('ready', asset='b')
        status = e.status()
        self.assertLessEqual(status['cache']['used'], status['cache']['budget'])


@unittest.skipUnless(ENGINE and shutil.which('ffmpeg'), 'set SIGNAGE_ENGINE_BIN (tests/run-native-tests.sh)')
class VideoEngineLateAssetTests(EngineCase):
    """Slow storage (TEST ONLY load delay): the next clip is not ready at the boundary."""
    env = {'SIGNAGE_ENGINE_TEST_LOAD_DELAY_MS': 2600}

    def test_late_asset_continues_current_then_switches_at_boundary(self):
        e = self.engine
        self.play('a')
        e.event('started', asset='a')
        self.play('b', at='boundary')
        e.event('waiting', asset='b')
        e.event('started', asset='b', timeout=8)
        e.event('ended', asset='b', reason='complete', timeout=5)
        seq = [h for _, h in e.hashes()]
        self.assertEqual(seq, self.assets.ref_a * 2 + self.assets.ref_b, 'current clip continues, no freeze, no partial clip')
        self.assert_continuous(e.hashes())

    def test_cancelled_late_asset_never_plays_and_releases_cache(self):
        e = self.engine
        self.play('a', loop=True)
        e.event('started', asset='a')
        self.play('b', at='boundary')
        time.sleep(0.2)
        self.assertEqual(e.send('cancel', client='page-1', asset={'id': 'b'})['message'], 'cancelled')
        e.event('cancelled', asset='b')
        time.sleep(3.2)
        self.assertNotIn(self.assets.ref_b[0], [h for _, h in e.hashes()])
        self.assertFalse([ev for ev in e.events if ev.get('event') in ('ready', 'error') and ev.get('asset') == 'b'])
        status = e.status()
        self.assertEqual([a['id'] for a in status['cache']['assets']], ['a'])


@unittest.skipUnless(ENGINE and shutil.which('ffmpeg'), 'set SIGNAGE_ENGINE_BIN (tests/run-native-tests.sh)')
class VideoEngineBudgetTests(unittest.TestCase):
    def test_over_budget_asset_is_refused(self):
        assets = Assets.build()
        work = Path(tempfile.mkdtemp(prefix='engine-run-'))
        engine = Engine(work, assets, SIGNAGE_ENGINE_CACHE_MB=0)
        try:
            ready = engine.event('engine-ready', timeout=30)
            self.assertFalse(ready['warm'], 'warm-up cannot fit a zero budget')
            ack = engine.send('prepare', client='p', asset=asset(assets.b, 'b'))
            self.assertEqual((ack['ok'], ack['code']), (False, 'over-budget'))
        finally:
            self.assertEqual(engine.close(), 0)
            shutil.rmtree(work, True)


if __name__ == '__main__':
    unittest.main()
