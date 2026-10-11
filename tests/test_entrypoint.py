import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app"))
import entrypoint


class DecoderConfigTests(unittest.TestCase):
    def test_default_allows_all_stateless_decoders(self):
        decoder, ranks = entrypoint.decoder_config({})
        self.assertEqual(decoder, "v4l2slh264dec,v4l2slh265dec,v4l2slvp9dec")
        self.assertIn("v4l2slh265dec:512", ranks)
        self.assertIn("avdec_h264:0", ranks)

    def test_single_hevc_decoder(self):
        decoder, ranks = entrypoint.decoder_config({"SIGNAGE_DECODER": "v4l2slh265dec"})
        self.assertEqual(decoder, "v4l2slh265dec")
        self.assertIn("v4l2slh265dec:512", ranks)
        self.assertIn("v4l2slh264dec:0", ranks)

    def test_software_requires_opt_in(self):
        with self.assertRaises(ValueError):
            entrypoint.decoder_config({"SIGNAGE_DECODER": "avdec_h264"})
        decoder, ranks = entrypoint.decoder_config({"SIGNAGE_ALLOW_SOFTWARE": "1"})
        self.assertEqual(decoder, "avdec_h264")
        self.assertIn("avdec_h264:512", ranks)
        self.assertIn("v4l2slh264dec:0", ranks)

    def test_unknown_decoder_rejected(self):
        with self.assertRaises(ValueError):
            entrypoint.decoder_config({"SIGNAGE_DECODER": "v4l2slh264dec,nonsense"})


class CacheTests(unittest.TestCase):
    def test_caches_are_writable_and_registry_is_keyed(self):
        import os
        import tempfile
        saved = dict(os.environ)
        try:
            os.environ.pop("XDG_CACHE_HOME", None)
            os.environ.pop("GST_REGISTRY", None)
            with tempfile.TemporaryDirectory() as temp:
                cache = entrypoint.configure_caches(Path(temp))
                self.assertEqual(os.environ["XDG_CACHE_HOME"], str(cache))
                self.assertTrue(os.environ["GST_REGISTRY"].startswith(str(cache / "gstreamer-1.0" / "registry-")))
                self.assertTrue(Path(os.environ["GST_REGISTRY"]).parent.is_dir())
        finally:
            os.environ.clear()
            os.environ.update(saved)


class StartUrlTests(unittest.TestCase):
    def test_default_is_display_lab(self):
        self.assertEqual(entrypoint.start_url({}), "http://127.0.0.1:8080/")

    def test_player_page(self):
        self.assertEqual(entrypoint.start_url({"SIGNAGE_PAGE": "player"}), "http://127.0.0.1:8080/player/")

    def test_menuboard_page(self):
        self.assertEqual(entrypoint.start_url({"SIGNAGE_PAGE": "menuboard", "SIGNAGE_HTTP_PORT": "9000"}),
                         "http://127.0.0.1:9000/menuboard/")

    def test_explicit_url_wins_and_unknown_page_rejected(self):
        self.assertEqual(entrypoint.start_url({"SIGNAGE_URL": "http://x/", "SIGNAGE_PAGE": "menuboard"}), "http://x/")
        with self.assertRaises(ValueError):
            entrypoint.start_url({"SIGNAGE_PAGE": "nope"})


if __name__ == "__main__":
    unittest.main()
