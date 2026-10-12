import io
import json
import stat
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
import content


class ContentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'data'
        self.app = Path(self.temp.name) / 'app'
        for name in ('welcome', 'menuboard'):
            (self.app / 'www' / name).mkdir(parents=True)
            (self.app / 'www' / name / 'index.html').write_text('<h1>Demo</h1>')
        (self.app / 'media').mkdir()
        self.state = content.read(self.root, self.app)

    def tearDown(self):
        self.temp.cleanup()

    def zip(self, files):
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w') as archive:
            for name, value in files:
                archive.writestr(name, value)
        return stream.getvalue()

    def upload(self, name, data):
        return content.upload(self.root, self.app, name, io.BytesIO(data), len(data))

    def test_seeds_copy_to_persistent_volume_and_do_not_reset(self):
        self.assertTrue((self.root / 'content/menuboard/index.html').exists())
        self.assertEqual(self.state['assets'][0]['url'], '/content/welcome/index.html')
        self.state['entries'] = []
        content.update(self.root, self.app, self.state)
        self.assertEqual(content.read(self.root, self.app)['entries'], [])

    def test_picture_in_picture_is_an_explicit_default_slot(self):
        assets = {a['id']: a for a in self.state['assets']}
        self.assertIn('?scene=C', assets['menuboard-pip']['url'])
        self.assertIn('?scene=A', assets['menuboard-main']['url'])
        self.assertIn('menuboard', assets)  # The complete original demo remains available.
        pip = next(e for e in self.state['entries'] if e['asset_id'] == 'menuboard-pip')
        self.assertEqual(pip['duration'], 20)
        self.assertTrue(pip['enabled'])

    def test_old_default_upgrades_but_edited_schedules_are_preserved(self):
        for edited in (False, True):
            state = json.loads(json.dumps(self.state))
            state.pop('demo_catalog')
            state['assets'] = [a for a in state['assets'] if a['id'] not in ('menuboard-main', 'menuboard-pip')]
            state['entries'] = [dict(id=identity, asset_id=identity, duration=duration, enabled=True, start=None, end=None)
                                for identity,duration in [('welcome',12),('menuboard',48),('promo-a',0),('promo-b',0)]]
            if edited:
                state['entries'][0]['duration'] = 7
            original = json.loads(json.dumps(state['entries']))
            self.assertTrue(content.upgrade_demos(state))
            self.assertFalse(content.upgrade_demos(state))
            if edited:
                self.assertEqual(state['entries'], original)
            else:
                self.assertIn('menuboard-pip', [e['asset_id'] for e in state['entries']])

    def test_seed_videos_prepared_once_and_preserved(self):
        (self.root / 'library.json').unlink()
        (self.app / 'media/promo-a.mp4').write_bytes(b'video')
        with patch.object(content, 'video_details', return_value={'video_duration': 12}) as probe:
            state = content.read(self.root, self.app)
            content.read(self.root, self.app)
            probe.assert_called_once()
        self.assertEqual((self.root / 'media/promo-a.mp4').read_bytes(), b'video')
        self.assertIn('promo-a', [e['asset_id'] for e in state['entries']])

    def test_bundled_4k_demo_is_a_library_asset_but_not_in_the_loop(self):
        (self.root / 'library.json').unlink()
        (self.app / 'media/demo-4k-hevc.mp4').write_bytes(b'video')
        details = {'video_duration': 10, 'poster': '/media/demo-4k-hevc.mp4.poster.jpg',
                   'end_poster': '/media/demo-4k-hevc.mp4.end_poster.jpg'}
        with patch.object(content, 'video_details', return_value=details):
            state = content.read(self.root, self.app)
        asset = next(a for a in state['assets'] if a['id'] == 'demo-4k-hevc')
        self.assertEqual((asset['name'], asset['kind'], asset['url']), ('4K HEVC demo', 'video', '/media/demo-4k-hevc.mp4'))
        self.assertEqual(asset['end_poster'], details['end_poster'])
        self.assertEqual((self.root / 'media/demo-4k-hevc.mp4').read_bytes(), b'video')
        self.assertNotIn('demo-4k-hevc', [e['asset_id'] for e in state['entries']])
        self.assertEqual(state['demo_catalog'], 3)

    def test_catalog_2_upgrade_appends_4k_asset_and_keeps_entries(self):
        (self.app / 'media/demo-4k-hevc.mp4').write_bytes(b'video')
        state = json.loads(json.dumps(self.state))
        state['demo_catalog'] = 2
        state['assets'] = [a for a in state['assets'] if a['id'] != 'demo-4k-hevc']
        state['entries'][0]['duration'] = 7  # An intentionally edited loop.
        before = json.loads(json.dumps(state['entries']))
        revision = state['revision']
        details = {'video_duration': 10, 'poster': 'p', 'end_poster': 'e'}
        with patch.object(content, 'video_details', return_value=details) as probe:
            self.assertTrue(content.upgrade_demos(state, self.root, self.app))
            self.assertFalse(content.upgrade_demos(state, self.root, self.app))
            probe.assert_called_once()
        self.assertEqual(state['entries'], before)
        self.assertEqual(state['demo_catalog'], 3)
        self.assertEqual(state['revision'], revision + 1)
        self.assertEqual([a['id'] for a in state['assets']].count('demo-4k-hevc'), 1)
        self.assertEqual(next(a for a in state['assets'] if a['id'] == 'demo-4k-hevc')['end_poster'], 'e')

    def test_catalog_3_upgrade_without_clip_in_image_stays_retryable(self):
        state = json.loads(json.dumps(self.state))
        state['demo_catalog'] = 2
        state['assets'] = [a for a in state['assets'] if a['id'] != 'demo-4k-hevc']
        self.assertFalse(content.upgrade_demos(state, self.root, self.app))
        self.assertEqual(state['demo_catalog'], 2)
        self.assertNotIn('demo-4k-hevc', [a['id'] for a in state['assets']])

    def test_video_stills_fit_4k_without_upscaling_and_end_poster_uses_last_frame(self):
        file = self.app / 'media/clip.mp4'
        file.write_bytes(b'video')
        calls = []
        def fake_run(args, **kwargs):
            if args[0] == 'ffprobe':
                return type('R', (), {'stdout': json.dumps({'format': {'duration': '10.0'}})})()
            calls.append(args)
            Path(args[-1]).write_bytes(b'jpg')
            return type('R', (), {'stdout': ''})()
        with patch.object(content.subprocess, 'run', side_effect=fake_run):
            details = content.video_details(file, '/media/clip.mp4')
        self.assertEqual(details['end_poster'], '/media/clip.mp4.end_poster.jpg')
        for args in calls:
            vf = args[args.index('-vf') + 1]
            self.assertEqual(vf, "scale='min(3840,iw)':'min(2160,ih)':force_original_aspect_ratio=decrease")
            self.assertNotIn('1920', vf)
        end = calls[1]
        self.assertEqual(end[end.index('-sseof') + 1], '-1')
        # -update is an image2 output option; before -i, ffmpeg 7 fails the whole library start.
        self.assertGreater(end.index('-update'), end.index('-i'))
        self.assertEqual(end[end.index('-update') + 1], '1')

    def test_schedule_inclusive_start_exclusive_end_and_timezone(self):
        item = self.state['entries'][0]
        item.update(start='2026-10-10T10:00:00+11:00', end='2026-10-10T11:00:00+11:00')
        self.state['entries'] = [item]
        saved = content.update(self.root, self.app, self.state)
        start = content.seconds(saved['entries'][0]['start'])
        self.assertEqual(content.playlist(saved, start - 1)['entries'], [])
        self.assertEqual(len(content.playlist(saved, start)['entries']), 1)
        self.assertEqual(content.playlist(saved, start + 3600)['entries'], [])
        self.assertEqual(content.playlist(saved, start - 1)['next_change'], start)
        saved['entries'][0]['enabled'] = False
        self.assertEqual(content.playlist(saved, start)['entries'], [])

    def test_reject_bad_schedule_duration_and_stale_edits(self):
        for change in ({'duration':float('nan')}, {'duration':0}, {'duration':True}, {'start':'not-a-date'},
                       {'start':'2026-01-01T00:00'}, {'start':1}, {'start':'2026-10-11T00:00Z','end':'2026-10-10T00:00Z'}):
            state = json.loads(json.dumps(self.state))
            state['entries'][0].update(change)
            with self.assertRaises(content.ContentError):
                content.update(self.root, self.app, state)
        content.update(self.root, self.app, self.state)
        with self.assertRaises(content.ContentError) as error:
            content.update(self.root, self.app, self.state)
        self.assertEqual(error.exception.status, 409)

    def test_nested_html_package_with_relative_assets(self):
        asset = self.upload('campaign.zip', self.zip([('campaign/index.html','<img src="photo.png">'), ('campaign/photo.png',b'image')]))
        self.assertTrue(asset['url'].endswith('/package/campaign/index.html'))
        self.assertEqual((self.root / 'content' / asset['id'] / 'package/campaign/photo.png').read_bytes(), b'image')
        self.assertFalse((self.root / 'content' / asset['id'] / 'upload.zip').exists())

    def test_unsafe_and_invalid_packages_do_not_leave_files(self):
        symlink = zipfile.ZipInfo('index.html'); symlink.create_system=3; symlink.external_attr=(stat.S_IFLNK | 0o777) << 16
        cases = [self.zip([('../outside','bad'),('index.html','ok')]), self.zip([('/absolute','bad')]),
                 self.zip([('..\\outside','bad')]), self.zip([('text.txt','no index')]),
                 self.zip([(symlink,'/etc/passwd')]), b'not a zip']
        before = set((self.root / 'content').iterdir())
        for data in cases:
            with self.assertRaises(content.ContentError):
                self.upload('package.zip', data)
            self.assertEqual(set((self.root / 'content').iterdir()), before)

    def test_upload_interrupt_and_size_limit(self):
        with self.assertRaises(content.ContentError):
            content.upload(self.root, self.app, 'image.png', io.BytesIO(b'abc'), 10)
        for length in (0, content.MAX_UPLOAD + 1):
            with self.assertRaises(content.ContentError):
                content.upload(self.root, self.app, 'image.png', io.BytesIO(), length)
        with self.assertRaises(content.ContentError):
            self.upload('program.exe', b'no')

    def test_delete_requires_removal_from_published_loop(self):
        asset = self.upload('hello.html', b'<h1>Hello</h1>')
        state = content.read(self.root, self.app)
        state['entries'].append(dict(id='uploaded',asset_id=asset['id'],duration=5,enabled=True,start=None,end=None))
        state = content.update(self.root, self.app, state)
        with self.assertRaises(content.ContentError):
            content.delete(self.root, self.app, asset['id'])
        state['entries'] = []
        content.update(self.root, self.app, state)
        content.delete(self.root, self.app, asset['id'])
        self.assertFalse((self.root / 'content' / asset['id']).exists())
        with self.assertRaises(content.ContentError):
            content.delete(self.root, self.app, 'welcome')


if __name__ == '__main__':
    unittest.main()
