import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
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

    def test_config_and_health(self):
        with urlopen(self.base + "/healthz") as response:
            self.assertTrue(json.load(response)["ok"])
        with urlopen(self.base + "/api/config") as response:
            self.assertIn("video_url", json.load(response))


if __name__ == "__main__":
    unittest.main()
