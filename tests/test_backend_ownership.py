from pathlib import Path
import unittest
R=Path(__file__).resolve().parents[1]
class BackendOwnership(unittest.TestCase):
 def test_exportable_owns_backend(self):
  s=(R/'browser/signage-browser.c').read_text()
  self.assertIn('webkit_web_view_backend_new(browser->backend, retain_exportable_backend, NULL)',s)
