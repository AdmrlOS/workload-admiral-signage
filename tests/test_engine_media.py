"""Shipped engine variants of the existing clips: full-size normalization evidence.

Fast checks (always): manifests match the original files (SHA-256), share one SPS and
describe 600-frame 3840x2160 CFR60 HEVC Main streams whose ES hash matches.
Slow checks (SIGNAGE_SLOW=1): independent ffprobe frame count + structural
re-validation, and (with SIGNAGE_ENGINE_BIN) the real engine decodes promo-a -> promo-b ->
promo-a at native size through one decoder with every frame's pixels equal to FFmpeg's
reference decode, continuous timestamps and a single decoder pool negotiation.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE_DIR = ROOT / 'media/engine'
CLIPS = ('promo-a', 'promo-b', 'demo-4k-hevc')
SLOW = os.environ.get('SIGNAGE_SLOW') == '1'


def manifest(name):
    return json.loads((ENGINE_DIR / f'{name}.engine.json').read_text())


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def reference(name):
    return [line.rsplit(',', 1)[1].strip() for line in (ENGINE_DIR / f'{name}.framemd5').read_text().splitlines()
            if line and not line.startswith('#')]


class ShippedEngineMediaTests(unittest.TestCase):
    def test_manifests_describe_native_4k60_and_match_originals(self):
        keys = set()
        for name in CLIPS:
            m = manifest(name)
            self.assertEqual((m['width'], m['height'], m['fps_n'], m['fps_d']), (3840, 2160, 60, 1), name)
            self.assertEqual((m['codec'], m['profile'], m['bit_depth'], m['max_num_reorder']), ('hevc', 'Main', 8, 0))
            self.assertEqual(m['frames'], 600)
            self.assertEqual(len(m['access_units']), 600)
            self.assertEqual(m['gops'], list(range(0, 600, 60)))
            self.assertEqual(m['audio'], 'none')
            self.assertEqual(m['source_sha256'], sha(ROOT / 'media' / f'{name}.mp4'), f'{name}: original changed or variant stale')
            self.assertEqual(m['es_sha256'], sha(ENGINE_DIR / m['es']))
            self.assertEqual(len(reference(name)), 600)
            keys.add(m['compat_key'])
        self.assertEqual(len(keys), 1, 'all shipped variants must share one SPS so the engine never renegotiates')

    @unittest.skipUnless(SLOW and shutil.which('ffprobe'), 'SIGNAGE_SLOW=1')
    def test_independent_ffprobe_and_structure(self):
        for name in CLIPS:
            m = manifest(name)
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/prepare-engine-asset.py'), '--validate-only',
                                     str(ENGINE_DIR / m['es'])], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            info = json.loads(result.stdout)
            self.assertEqual((info['frames'], info['compat_key']), (600, m['compat_key']))
            probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-count_frames', '-of', 'json', '-show_entries',
                                                        'stream=nb_read_frames,r_frame_rate,width,height,profile,pix_fmt',
                                                        str(ENGINE_DIR / m['es'])]))['streams'][0]
            self.assertEqual((probe['nb_read_frames'], probe['r_frame_rate'], probe['width'], probe['height'], probe['profile'],
                              probe['pix_fmt']), ('600', '60/1', 3840, 2160, 'Main', 'yuv420p'))


@unittest.skipUnless(SLOW and os.environ.get('SIGNAGE_ENGINE_BIN'), 'SIGNAGE_SLOW=1 with SIGNAGE_ENGINE_BIN')
class NativeSizeEngineTests(unittest.TestCase):
    def test_promo_a_b_a_through_one_decoder_is_pixel_exact(self):
        sys.path.insert(0, str(ROOT / 'tests'))
        from test_video_engine import Engine

        class Assets:
            dir = ENGINE_DIR
            a = ENGINE_DIR / 'promo-a.engine.json'

        work = Path(tempfile.mkdtemp(prefix='engine-4k-'))
        engine = Engine(work, Assets, SIGNAGE_ENGINE_WIDTH=3840, SIGNAGE_ENGINE_HEIGHT=2160, SIGNAGE_ENGINE_TEST_SYNC=0,
                        SIGNAGE_ENGINE_CACHE_MB=160)
        try:
            ready = engine.event('engine-ready', timeout=120)
            self.assertTrue(ready['warm'])
            engine.send('acquire', client='p')
            started = time.monotonic()
            for count, name in enumerate(('promo-a', 'promo-b', 'promo-a'), 1):
                ack = engine.send('play', client='p', asset={'id': name, 'manifest': str(ENGINE_DIR / f'{name}.engine.json')}, at='boundary')
                self.assertTrue(ack['ok'], ack)
                engine.wait(lambda e, c=count: sum(1 for x in engine.events if x.get('event') == 'started') >= c, 300)
            engine.wait(lambda e: e.get('event') == 'ended' and e.get('reason') == 'complete', 600)
            elapsed = time.monotonic() - started
            hashes = engine.hashes()
            expected = reference('promo-a') + reference('promo-b') + reference('promo-a')
            self.assertEqual(len(hashes), 1800)
            self.assertEqual([h for _, h in hashes], expected)
            pts = [int(p) for p, _ in hashes]
            self.assertTrue({b - a for a, b in zip(pts, pts[1:])} <= {16666666, 16666667})
            status = engine.status()
            self.assertEqual((status['alloc_queries'], status['caps_events']), (1, 1))
            self.assertLessEqual(status['cache']['used'], status['cache']['budget'])
            print(f'\n4K A->B->A: 1800/1800 frames pixel-exact, software decode wall {elapsed:.1f}s, '
                  f"cache {status['cache']['used']} B", file=sys.stderr)
        finally:
            self.assertEqual(engine.close(), 0)
            shutil.rmtree(work, True)


if __name__ == '__main__':
    unittest.main()
