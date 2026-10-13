import unittest
from check_presented_order import validate
class OrderedAcceptanceTests(unittest.TestCase):
    def test_offset_manifest_and_initial_preroll_reshow(self):
        expected=[33333333,50000000,66666667]
        self.assertEqual(validate(expected,[(1,7,33333333),(2,7,33333333),(3,8,50000000),(4,9,66666667)]),3)
    def test_unique_count_cannot_accept_reorder_offset_or_interior_duplicate(self):
        expected=[33,50,67]
        for pts in ([33,67,50],[0,17,34],[33,50,50,67],[33,50]):
            with self.assertRaises(ValueError): validate(expected,[(i+1,7,p) for i,p in enumerate(pts)])
    def test_duplicate_or_wrapped_serial_is_rejected(self):
        for serials in ([1,1],[4294967295,0]):
            with self.assertRaises(ValueError): validate([33,50],[(s,7,p) for s,p in zip(serials,[33,50])])
