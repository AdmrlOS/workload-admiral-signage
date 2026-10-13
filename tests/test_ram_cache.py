"""Real anonymous-RAM cache; never a persistent playback write."""
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))


class RamCacheTests(unittest.TestCase):
    def test_cold_then_warm_file_survives_source_read_failure(self):
        try:
            from ram_cache import RamCache
        except ImportError:
            self.fail('bounded RAM cache is not implemented')
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'clip'; file.write_bytes(b'0123456789')
            cache = RamCache(32, reserve=0, available=lambda: 1024)
            self.assertTrue(cache.warm(file))
            with cache.acquire(file) as data:
                self.assertEqual(bytes(data[2:5]), b'234')
            self.assertTrue(cache.warm(file))
            self.assertEqual(cache.stats()['loads'], 1)
            cache.close()


    def test_budget_pins_leases_oversized_and_pressure(self):
        from ram_cache import RamCache
        with tempfile.TemporaryDirectory() as temp:
            a, b, c = [Path(temp) / name for name in ('a', 'b', 'c')]
            for file in (a, b, c): file.write_bytes(b'abcd')
            cache = RamCache(8, reserve=4, available=lambda: 100)
            epoch = cache.set_window([a, b])
            self.assertTrue(cache.warm(a, epoch)); self.assertTrue(cache.warm(b, epoch))
            self.assertFalse(cache.warm(c, epoch))
            with cache.acquire(a) as leased:
                epoch = cache.set_window([c])
                self.assertTrue(cache.warm(c, epoch))
                self.assertEqual(bytes(leased), b'abcd')
                self.assertLessEqual(cache.stats()['bytes'], 8)
            a.write_bytes(b'too large')
            self.assertFalse(cache.warm(a))
            cache.available = lambda: 3
            self.assertFalse(cache.warm(b))
            cache.close()

    def test_changed_source_and_pressure_during_load_discard_partial_ram(self):
        from ram_cache import RamCache
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'clip'; file.write_bytes(b'x' * 100)
            readings = iter([1000, 0])
            cache = RamCache(200, reserve=10, available=lambda: next(readings, 0))
            self.assertFalse(cache.warm(file))
            self.assertEqual(cache.stats()['bytes'], 0)
            cache.available = lambda: 1000
            self.assertTrue(cache.warm(file))
            file.write_bytes(b'y' * 101)
            with cache.acquire(file) as data:
                self.assertIsNone(data)
            self.assertEqual(cache.stats()['bytes'], 0)
            cache.close()

    def test_cancellation_and_missing_source_fail_without_partial_entries(self):
        from ram_cache import RamCache
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'clip'; file.write_bytes(b'x' * 100)
            cache = RamCache(200, reserve=0, available=lambda: 1000)
            epoch = cache.set_window([file]); cache.set_window([])
            self.assertFalse(cache.warm(file, epoch))
            self.assertFalse(cache.warm(file.with_name('missing')))
            self.assertEqual(cache.stats()['bytes'], 0)
            self.assertEqual(cache.stats()['loading'], 0)
            cache.close()


if __name__ == '__main__':
    unittest.main()
