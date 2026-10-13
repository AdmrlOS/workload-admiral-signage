"""Execute the production split geometry contract with no DRM dependencies."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class SplitScanoutTests(unittest.TestCase):
    def test_browser_uses_owned_frame_split_and_atomic_second_plane(self):
        source = (ROOT / 'browser/signage-browser.c').read_text()
        self.assertIn('gboolean split;', source)
        self.assertIn('select_split_plane(&browser)', source)
        self.assertIn('add_ui_state(browser, request, frame)', source)
        self.assertIn('DRM_MODE_ATOMIC_TEST_ONLY', source[source.index('static Frame *import_direct('):source.index('/* ---- GPU copy')])

    def test_native_views_and_rejected_geometry(self):
        header = ROOT / 'browser/split-scanout.h'
        self.assertTrue(header.exists(), 'production split geometry contract is missing')
        source = r'''
#include <assert.h>
#include "split-scanout.h"
int main(void) {
    SignageSplitGeometry g = {0};
    /* sun4i labels its full-size RGB-only second UI channel Cursor. */
    assert(signage_split_plane_type(0));
    assert(signage_split_plane_type(2));
    assert(!signage_split_plane_type(1));
    assert(!signage_split_plane_type(99));
    assert(signage_split_geometry(3840,2160,3840,2160,&g));
    assert(g.left == 1920 && g.right == 1920 && g.height == 2160);
    assert(g.left + g.right == 3840);
    assert(signage_split_geometry(4096,2160,4096,2160,&g));
    assert(g.left == 2048 && g.right == 2048);
    assert(!signage_split_geometry(3840,2160,1920,1080,&g));
    assert(!signage_split_geometry(3841,2160,3841,2160,&g));
    assert(!signage_split_geometry(3840,2168,3840,2168,&g));
    assert(!signage_split_geometry(1920,1080,1920,1080,&g));
    assert(!signage_split_geometry(8192,2160,8192,2160,&g));
    assert(!signage_split_geometry(0,0,0,0,&g));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c = pathlib.Path(d) / 'split.c'
            c.write_text(source)
            exe = pathlib.Path(d) / 'split'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(header.parent),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__':
    unittest.main()
