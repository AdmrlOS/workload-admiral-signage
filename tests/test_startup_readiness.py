import sys
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
import entrypoint

class ReadinessTests(unittest.TestCase):
    def test_timeout_fails_closed(self):
        with patch.object(entrypoint, 'urlopen', side_effect=OSError('refused')):
            self.assertFalse(entrypoint.wait_for_server('http://localhost/healthz', lambda: True, timeout=0.01))

    def test_late_server_is_retried(self):
        from contextlib import nullcontext
        with patch.object(entrypoint, 'urlopen', side_effect=[OSError('refused'), nullcontext(type('Response', (), {'status': 200})())]):
            self.assertTrue(entrypoint.wait_for_server('http://localhost/healthz', lambda: True, timeout=1))

    def test_dead_server_is_not_probed(self):
        with patch.object(entrypoint, 'urlopen') as probe:
            self.assertFalse(entrypoint.wait_for_server('http://localhost/healthz', lambda: False, timeout=1))
            probe.assert_not_called()
