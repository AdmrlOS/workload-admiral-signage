from pathlib import Path
import unittest
R=Path(__file__).resolve().parents[1]
class NativeStandbyWiring(unittest.TestCase):
 def test_physical_standby_is_wired(self):
  b=(R/'browser/signage-browser.c').read_text()+(R/'browser/standby-renderer.h').read_text()
  self.assertIn('signage_standby_ready(',b)
  self.assertIn('signage_standby_flipped(',b)
  self.assertIn('root->visible_view == browser',b)
  self.assertIn('DRM_MODE_ATOMIC_TEST_ONLY',b)
  s=(R/'native/gst-admiral-videoplane.c').read_text()
  self.assertIn('SIGNAGE_VIDEO_ENDPOINT',s)
  p=(R/'www/player/player.js').read_text()
  self.assertIn('native-stage',p)
