"""Run the ownership gate under ASAN/UBSAN; no fake DRM acceptance."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class StandbyProtocolTests(unittest.TestCase):
    def test_generation_admission_endpoint_and_retirement(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'standby'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            str(ROOT / 'tests/native-standby-protocol.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_cancelled_credit_is_not_eos_success(self):
        source = (ROOT / 'native/gst-admiral-videoplane.c').read_text()
        start = source.index('static GstFlowReturn wait_presented(')
        function = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#include <glib.h>
#include <stdint.h>
#include <assert.h>
typedef int GstFlowReturn;
#define GST_FLOW_OK 0
#define GST_FLOW_ERROR -1
#define GST_ELEMENT_ERROR(...) ((void)0)
typedef struct { guint32 serial, last_presented_serial; gboolean presentation_failed; } AdmiralPlaneSink;
static GstFlowReturn wait_credit(AdmiralPlaneSink *s, unsigned n) { (void)s; (void)n; return GST_FLOW_OK; }
''' + function + r'''
int main(void) {
 AdmiralPlaneSink s={.serial=600,.last_presented_serial=599};
 assert(wait_presented(&s)==GST_FLOW_ERROR);
 s.last_presented_serial=600; assert(wait_presented(&s)==GST_FLOW_OK);
 s.presentation_failed=TRUE; assert(wait_presented(&s)==GST_FLOW_ERROR);
 s.presentation_failed=FALSE; s.serial=0; s.last_presented_serial=0;
 assert(wait_presented(&s)==GST_FLOW_ERROR);
 return 0;
}
'''
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'test.c').write_text(harness)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(p / 'test.c'), '-o', str(p / 'test'), *flags], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=30)
