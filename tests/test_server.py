import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app"))
import server


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        server.APP_ROOT = root / "bundled"
        server.DATA_ROOT = root / "data"
        for source in (server.APP_ROOT, server.DATA_ROOT):
            (source / "www").mkdir(parents=True)
            (source / "media").mkdir()
        (server.APP_ROOT / "www/index.html").write_text("bundled page")
        (server.APP_ROOT / "media/test.mp4").write_bytes(b"0123456789")
        (root / "secret.txt").write_text("must not leak")
        (server.DATA_ROOT / "www/escape.txt").symlink_to(root / "secret.txt")
        cls.http = server.ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        cls.thread = threading.Thread(target=cls.http.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.http.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def test_native_builtin_ignores_persistent_seed_and_cache(self):
        folder = server.APP_ROOT / 'www/menuboard'
        folder.mkdir(exist_ok=True)
        (folder / 'menuboard.js').write_text('new native lifecycle')
        with urlopen(self.base + '/native-builtin/menuboard/menuboard.js') as response:
            self.assertEqual(response.read(), b'new native lifecycle')
            self.assertEqual(response.headers['Cache-Control'], 'no-store')

    def test_native_builtin_welcome_route_serves_image_copy_no_store(self):
        folder = server.APP_ROOT / 'www/welcome'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'index.html').write_text('image welcome')
        with urlopen(self.base + '/native-builtin/welcome/index.html?native_stage=1') as response:
            self.assertEqual(response.read(), b'image welcome')
            self.assertEqual(response.headers['Cache-Control'], 'no-store')

    def test_media_range_and_suffix(self):
        for header, body, content_range in (("bytes=2-4", b"234", "bytes 2-4/10"),
                ("bytes=-3", b"789", "bytes 7-9/10"), ("bytes=8-", b"89", "bytes 8-9/10")):
            with urlopen(Request(self.base + "/media/test.mp4", headers={"Range": header})) as response:
                self.assertEqual(response.status, 206)
                self.assertEqual(response.read(), body)
                self.assertEqual(response.headers["Content-Range"], content_range)

    def test_unsatisfiable_range_and_head(self):
        with self.assertRaises(HTTPError) as error:
            urlopen(Request(self.base + "/media/test.mp4", headers={"Range": "bytes=11-"}))
        self.assertEqual(error.exception.code, 416)
        error.exception.close()
        with urlopen(Request(self.base + "/media/test.mp4", method="HEAD", headers={"Range": "bytes=0-1"})) as response:
            self.assertEqual(response.headers["Content-Length"], "2")
            self.assertEqual(response.read(), b"")

    def test_live_override_and_bundled_fallback(self):
        with urlopen(self.base + "/") as response:
            self.assertEqual(response.read(), b"bundled page")
        override = server.DATA_ROOT / "www/index.html"
        override.write_text("edited page")
        try:
            with urlopen(self.base + "/") as response:
                self.assertEqual(response.read(), b"edited page")
        finally:
            override.unlink()

    def test_subdirectory_index(self):
        (server.APP_ROOT / "www/sub").mkdir()
        (server.APP_ROOT / "www/sub/index.html").write_text("sub page")
        for path in ("/sub/", "/sub/index.html"):
            with urlopen(self.base + path) as response:
                self.assertEqual(response.read(), b"sub page")
        with self.assertRaises(HTTPError) as error:
            urlopen(self.base + "/missing/")
        self.assertEqual(error.exception.code, 404)

    def test_traversal_and_symlink_escape(self):
        for path in ("/%2e%2e/secret.txt", "/escape.txt", "/media/%2e%2e/%2e%2e/secret.txt"):
            with self.assertRaises(HTTPError) as error:
                urlopen(self.base + path)
            self.assertEqual(error.exception.code, 404)
            error.exception.close()

    def test_content_upload_sandbox_and_publish(self):
        headers = {'X-Signage-Request': 'studio', 'Content-Type': 'application/octet-stream'}
        request = Request(self.base + '/api/assets?name=hello.html', data=b'<html><head></head><body>Hello</body></html>', headers=headers)
        with urlopen(request) as response:
            asset = json.load(response)
        with urlopen(self.base + asset['url']) as response:
            body = response.read()
            self.assertIn(b'/player/bridge.js', body)
            self.assertIn("sandbox allow-scripts", response.headers['Content-Security-Policy'])
        with urlopen(self.base + '/api/library') as response:
            state = json.load(response)
        state['entries'] = [dict(id='html', asset_id=asset['id'], duration=2, enabled=True, start=None, end=None)]
        with urlopen(Request(self.base + '/api/playlist', method='PUT', data=json.dumps(state).encode(), headers=headers)) as response:
            self.assertEqual(response.status, 200)
        with urlopen(self.base + '/api/playlist') as response:
            self.assertEqual(json.load(response)['entries'][0]['asset']['id'], asset['id'])
        with self.assertRaises(HTTPError) as error:
            urlopen(Request(self.base + '/api/playlist', method='PUT', data=json.dumps(state).encode(), headers=headers))
        self.assertEqual(error.exception.code, 409)
        error.exception.close()

    def test_mutations_reject_cross_origin_and_missing_header(self):
        for headers in ({}, {'Origin':'null','X-Signage-Request':'studio'},
                        {'Origin':'https://other.test','X-Signage-Request':'studio'}):
            with self.assertRaises(HTTPError) as error:
                urlopen(Request(self.base + '/api/assets?name=test.html', data=b'test', headers=headers))
            self.assertEqual(error.exception.code, 403)
            error.exception.close()


    def test_config_reports_effective_start_page(self):
        with patch.dict(os.environ, {'SIGNAGE_PAGE': 'player'}, clear=True):
            self.assertEqual(server.configuration()['browser_url'], 'http://127.0.0.1:8080/player/')
        with patch.dict(os.environ, {'SIGNAGE_PAGE': 'player', 'SIGNAGE_URL': 'http://127.0.0.1:8080/menuboard/'}, clear=True):
            self.assertEqual(server.configuration()['browser_url'], 'http://127.0.0.1:8080/menuboard/')

    def test_ram_window_and_range_delivery(self):
        from ram_cache import RamCache
        with patch.object(server, 'RAM_CACHE', RamCache(1024, 0, available=lambda: 4096)):
            payload = json.dumps({'urls': ['/media/test.mp4'], 'active': '/media/test.mp4'}).encode()
            with urlopen(Request(self.base + '/api/preload', data=payload,
                                 headers={'X-Signage-Request': 'player', 'Content-Type': 'application/json'})) as response:
                self.assertEqual(response.status, 200)
            server.PRELOADER.join()
            with urlopen(Request(self.base + '/media/test.mp4', headers={'Range': 'bytes=2-4'})) as response:
                self.assertEqual(response.read(), b'234')
                self.assertEqual(response.headers['X-Signage-Cache'], 'ram')
            server.RAM_CACHE.close()

    def test_preload_rejects_cross_origin_and_nonobject_payload(self):
        for data, headers, code in ((b'[]', {'X-Signage-Request': 'player'}, 400),
                (b'{"urls":[]}', {'X-Signage-Request': 'player', 'Origin': 'http://evil.test'}, 403)):
            with self.assertRaises(HTTPError) as error:
                urlopen(Request(self.base + '/api/preload', data=data, headers=headers))
            self.assertEqual(error.exception.code, code)
            error.exception.close()

    def test_package_dependencies_and_staging_csp(self):
        from ram_cache import RamCache
        package = server.DATA_ROOT / 'content' / 'safe'
        package.mkdir(parents=True, exist_ok=True)
        (package / 'index.html').write_text('<head><meta name="signage-preload" content="static"></head><img src="image.png">')
        (package / 'font.woff2').write_bytes(b'font')
        with patch.object(server, 'RAM_CACHE', RamCache(1024, 0, available=lambda: 4096)):
            server.preload_window(['/content/safe/index.html'])
            server.PRELOADER.join()
            with server.RAM_CACHE.acquire(package / 'font.woff2') as data:
                self.assertEqual(bytes(data), b'font')
            with urlopen(self.base + '/content/safe/index.html?signage_stage=1') as response:
                self.assertIn("media-src 'none'", response.headers['Content-Security-Policy'])
                self.assertIn(b'signage_stage', response.read())
            server.RAM_CACHE.close()

    def test_config_and_health(self):
        with urlopen(self.base + "/healthz") as response:
            self.assertTrue(json.load(response)["ok"])
        with urlopen(self.base + "/api/config") as response:
            self.assertIn("video_url", json.load(response))


if __name__ == "__main__":
    unittest.main()
