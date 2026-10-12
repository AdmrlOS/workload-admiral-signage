"""Compile the browser's actual size-selection functions without Linux/WPE deps."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'C compiler required for browser size tests')
class BrowserUISizeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='signage-ui-')
        source = (ROOT / 'browser/signage-browser.c').read_text()
        functions = source[source.index('static gboolean choose_view_size('):
                           source.index('static gboolean open_display(')]
        harness = r'''
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define gboolean int
#define TRUE 1
#define FALSE 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define g_getenv getenv
#define g_ascii_strtod strtod
#define g_printerr(...) fprintf(stderr, __VA_ARGS__)
#define g_message(...) fprintf(stderr, __VA_ARGS__)
typedef struct {
    struct { unsigned hdisplay, vdisplay; } mode;
    unsigned ui_width, ui_height, view_width, view_height;
    float ui_scale;
} Browser;
'''
        harness += functions
        harness += r'''
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Browser browser = { .mode = { (unsigned)strtoul(argv[1], NULL, 10),
                                  (unsigned)strtoul(argv[2], NULL, 10) } };
    if (!choose_ui_size(&browser)) return 1;
    printf("%u %u %u %u %g\n", browser.ui_width, browser.ui_height,
           browser.view_width, browser.view_height, (double)browser.ui_scale);
    return 0;
}
'''
        path = Path(cls.directory.name)
        (path / 'ui.c').write_text(harness)
        cls.binary = path / 'ui'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path / 'ui.c'), '-o', str(cls.binary)], check=True,
                       capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def choose(self, width=3840, height=2160, size=None, scale=None):
        env = dict(os.environ)
        for key, value in [('SIGNAGE_UI_SIZE', size), ('SIGNAGE_UI_SCALE', scale)]:
            env.pop(key, None)
            if value is not None:
                env[key] = value
        return subprocess.run([str(self.binary), str(width), str(height)],
                              env=env, capture_output=True, text=True)

    def test_default_uses_native_framebuffer_and_css_viewport(self):
        for width, height, expected in [(3840, 2160, '3840 2160 3840 2160 1'),
                                        (1920, 1080, '1920 1080 1920 1080 1'),
                                        (1280, 720, '1280 720 1280 720 1'),
                                        (2160, 3840, '2160 3840 2160 3840 1')]:
            with self.subTest(mode=(width, height)):
                result = self.choose(width, height)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_native_and_explicit_4k(self):
        for size in ['native', '3840x2160']:
            for scale, expected in [(None, '3840 2160 3840 2160 1'),
                                    ('2', '3840 2160 1920 1080 2'),
                                    ('1.5', '3840 2160 2560 1440 1.5')]:
                with self.subTest(size=size, scale=scale):
                    result = self.choose(size=size, scale=scale)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(), expected)

    def test_scale_does_not_reduce_native_framebuffer(self):
        result = self.choose(scale='2')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '3840 2160 1920 1080 2')

    def test_lower_framebuffer_is_explicit_performance_opt_in(self):
        for scale, expected in [(None, '1920 1080 1920 1080 1'),
                                ('2', '1920 1080 960 540 2')]:
            with self.subTest(scale=scale):
                result = self.choose(size='1920x1080', scale=scale)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_empty_settings_use_defaults(self):
        result = self.choose(size='', scale='')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '3840 2160 3840 2160 1')

    def test_invalid_sizes_fail(self):
        for size in ['Native', '0x1080', '1920x0', '4000x2160', '3840x2200',
                     '-1x1080', '+1920x1080', '1920x-1080', '1920x+1080',
                     '1920x1080junk', '1920x1080 ', '1920X1080', '1920',
                     '18446744073709551616x1080', '1920x18446744073709551616']:
            with self.subTest(size=size):
                self.assertEqual(self.choose(size=size).returncode, 1)

    def test_invalid_scales_fail(self):
        for scale in ['0', '-2', 'nan', 'inf', '1e999', '1e-999', '2junk',
                      '2 ', '0.049', '5.01', '7', '1.7']:
            with self.subTest(scale=scale):
                self.assertEqual(self.choose(size='native', scale=scale).returncode, 1)

    def test_wpe_scale_boundaries(self):
        for scale, expected in [('0.05', '3840 2160 76800 43200 0.05'),
                                ('5', '3840 2160 768 432 5')]:
            result = self.choose(size='native', scale=scale)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), expected)

    def test_backend_and_coordinates_use_correct_spaces(self):
        source = (ROOT / 'browser/signage-browser.c').read_text()
        self.assertIn('fdo_create(&client, browser, browser->view_width, browser->view_height)', source)
        self.assertIn('signage_input_start(browser->backend, browser->ui_width, browser->ui_height)', source)
        self.assertIn('browser.ui_width, browser.ui_height,\n', source)
        self.assertLess(source.index('browser->view = webkit_web_view_new(backend)'),
                        source.index('wpe_view_backend_dispatch_set_device_scale_factor('))
        self.assertLess(source.index('wpe_view_backend_dispatch_set_device_scale_factor('),
                        source.index('webkit_web_view_load_uri(browser->view, browser->uri)', source.index('static gboolean initialize_webkit(')))
        self.assertIn('browser->ui_height = browser->mode.vdisplay;\n        if (!choose_view_size(browser))', source)
