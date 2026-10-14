"""Engine asset preparation: real local ffmpeg/ffprobe, small synthetic sources.

The full-size normalization of the two shipped promo clips is exercised by
tests/test_engine_media.py (SIGNAGE_SLOW=1)."""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/prepare-engine-asset.py'
SMALL = ['--width', '320', '--height', '180', '--fps', '60', '--preset', 'ultrafast']


def have_tools():
    return shutil.which('ffmpeg') and shutil.which('ffprobe')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def make_source(path, *, seconds=2, rate=24, size='640x360', audio=False, pattern='testsrc2'):
    cmd = ['ffmpeg', '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i',
           f'{pattern}=size={size}:rate={rate}', '-t', str(seconds)]
    if audio:
        cmd += ['-f', 'lavfi', '-i', f'sine=frequency=440:duration={seconds}', '-c:a', 'aac', '-shortest']
    cmd += ['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-g', '48', str(path)]
    subprocess.run(cmd, check=True)


def prepare(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


@unittest.skipUnless(have_tools(), 'ffmpeg/ffprobe required')
class PrepareEngineAssetTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='engine-prep-'))
        self.addCleanup(shutil.rmtree, self.dir, True)

    def test_transcode_produces_validated_manifest_and_es(self):
        source = self.dir / 'a.mp4'
        make_source(source)
        before = sha(source)
        out = self.dir / 'out'
        result = prepare(source, '--out-dir', out, '--name', 'a', '--reference', *SMALL)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sha(source), before, 'original must never be modified')
        manifest = json.loads((out / 'a.engine.json').read_text())
        self.assertEqual(manifest['engine_protocol'], 1)
        self.assertEqual((manifest['codec'], manifest['profile'], manifest['bit_depth']), ('hevc', 'Main', 8))
        self.assertEqual((manifest['width'], manifest['height']), (320, 180))
        self.assertEqual((manifest['fps_n'], manifest['fps_d']), (60, 1))
        self.assertEqual(manifest['frames'], 120)
        self.assertEqual(manifest['frames'], len(manifest['access_units']))
        self.assertEqual(manifest['audio'], 'none')
        self.assertEqual(manifest['max_num_reorder'], 0)
        self.assertEqual(manifest['source_sha256'], before)
        es = (out / 'a.hevc').read_bytes()
        self.assertEqual(hashlib.sha256(es).hexdigest(), manifest['es_sha256'])
        self.assertEqual(manifest['es_size'], len(es))
        units = manifest['access_units']
        self.assertEqual(units[0]['offset'], 0)
        for a, b in zip(units, units[1:]):
            self.assertEqual(a['offset'] + a['size'], b['offset'])
            self.assertGreater(b['pts'], a['pts'])
        self.assertEqual(units[-1]['offset'] + units[-1]['size'], len(es))
        # Closed GOPs: an IDR with in-band headers starts every GOP, one per second.
        self.assertEqual(manifest['gops'], [0, 60])
        self.assertTrue(all(units[i]['key'] for i in manifest['gops']))
        self.assertEqual(sum(1 for u in units if u['key']), 2)
        self.assertEqual(units[1]['pts'], 16666666)
        self.assertEqual(manifest['duration_ns'], 2_000_000_000)
        ref = (out / 'a.framemd5').read_text().splitlines()
        self.assertEqual(sum(1 for line in ref if line and not line.startswith('#')), 120)
        self.assertRegex(manifest['compat_key'], '^[0-9a-f]{64}$')
        self.assertIn('cpu_seconds', manifest['prepare'])

    def test_two_different_sources_share_compat_key(self):
        keys = []
        for name, pattern in (('x', 'testsrc2'), ('y', 'mandelbrot')):
            make_source(self.dir / f'{name}.mp4', pattern=pattern, rate=30)
            result = prepare(self.dir / f'{name}.mp4', '--out-dir', self.dir / 'out', '--name', name, *SMALL)
            self.assertEqual(result.returncode, 0, result.stderr)
            keys.append(json.loads((self.dir / 'out' / f'{name}.engine.json').read_text())['compat_key'])
        self.assertEqual(keys[0], keys[1])

    def test_audio_requires_explicit_policy(self):
        source = self.dir / 'audio.mp4'
        make_source(source, audio=True)
        result = prepare(source, '--out-dir', self.dir / 'out', *SMALL)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--audio drop', result.stderr)
        self.assertFalse((self.dir / 'out' / 'audio.engine.json').exists())
        result = prepare(source, '--out-dir', self.dir / 'out', '--audio', 'drop', *SMALL)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.dir / 'out' / 'audio.engine.json').read_text())['audio'], 'dropped')

    def test_never_overwrites_input_or_existing_outputs(self):
        source = self.dir / 'same.hevc'
        make_source(self.dir / 'tmp.mp4')
        subprocess.run(['ffmpeg', '-v', 'error', '-i', self.dir / 'tmp.mp4', '-c:v', 'libx265', '-x265-params', 'log-level=error',
                        '-f', 'hevc', source], check=True)
        before = sha(source)
        result = prepare(source, '--out-dir', self.dir, '--name', 'same', *SMALL)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('input', result.stderr.lower())
        self.assertEqual(sha(source), before)
        ok = prepare(self.dir / 'tmp.mp4', '--out-dir', self.dir / 'o', '--name', 'n', *SMALL)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        again = prepare(self.dir / 'tmp.mp4', '--out-dir', self.dir / 'o', '--name', 'n', *SMALL)
        self.assertNotEqual(again.returncode, 0)
        self.assertIn('exists', again.stderr)

    def test_validate_rejects_incompatible_streams(self):
        make_source(self.dir / 'src.mp4')
        bad = {
            'open-gop-bframes': 'keyint=60:min-keyint=60:scenecut=0:open-gop=1:bframes=3:repeat-headers=1',
            'no-repeated-headers': 'keyint=60:min-keyint=60:scenecut=0:open-gop=0:bframes=0:repeat-headers=0',
        }
        for name, params in bad.items():
            es = self.dir / f'{name}.hevc'
            subprocess.run(['ffmpeg', '-v', 'error', '-i', self.dir / 'src.mp4', '-vf', 'scale=320:180,fps=60',
                            '-c:v', 'libx265', '-x265-params', params + ':log-level=error', '-f', 'hevc', es], check=True)
            result = prepare('--validate-only', es, *SMALL)
            self.assertNotEqual(result.returncode, 0, name)
            self.assertIn('incompatible', result.stderr, name)
        wrong_rate = self.dir / 'rate.hevc'
        subprocess.run(['ffmpeg', '-v', 'error', '-i', self.dir / 'src.mp4', '-vf', 'scale=320:180,fps=30',
                        '-c:v', 'libx265', '-x265-params', 'keyint=30:scenecut=0:open-gop=0:bframes=0:repeat-headers=1:log-level=error',
                        '-f', 'hevc', wrong_rate], check=True)
        result = prepare('--validate-only', wrong_rate, *SMALL)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('fps', result.stderr)

    def test_remux_mode_keeps_compatible_stream_bit_exact(self):
        make_source(self.dir / 'src.mp4')
        first = prepare(self.dir / 'src.mp4', '--out-dir', self.dir / 'p', '--name', 'p', *SMALL)
        self.assertEqual(first.returncode, 0, first.stderr)
        mp4 = self.dir / 'p.mp4'
        subprocess.run(['ffmpeg', '-v', 'error', '-r', '60', '-i', self.dir / 'p/p.hevc', '-c:v', 'copy', '-tag:v', 'hvc1', mp4], check=True)
        result = prepare(mp4, '--out-dir', self.dir / 'r', '--name', 'r', '--mode', 'remux', *SMALL)
        self.assertEqual(result.returncode, 0, result.stderr)
        a = json.loads((self.dir / 'p/p.engine.json').read_text())
        b = json.loads((self.dir / 'r/r.engine.json').read_text())
        self.assertEqual(b['prepare']['mode'], 'remux')
        self.assertEqual(a['compat_key'], b['compat_key'])
        self.assertEqual(a['frames'], b['frames'])


if __name__ == '__main__':
    unittest.main()
