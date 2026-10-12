"""Compile real scheduling and receiver seams, without Linux/WPE headers."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class NativeSchedulingTests(unittest.TestCase):
    def test_unsignalled_ui_does_not_gate_video(self):
        source = (ROOT / 'browser/signage-browser.c').read_text()
        self.assertIn('static gboolean frame_ready(', source)
        function = source[source.index('static gboolean frame_ready(const Frame *frame)\n{'):source.index('/* Puts the queued browser frame')]
        harness = '''#include <poll.h>
#include <unistd.h>
#include <assert.h>
#define gboolean int
#define TRUE 1
#define FALSE 0
typedef struct { int fence_fd; } Frame;
''' + function + '''
int main(void) { int fds[2]; assert(pipe(fds)==0); Frame frame={fds[0]};
assert(!frame_ready(&frame)); assert(write(fds[1], "x", 1)==1); assert(frame_ready(&frame));
frame.fence_fd=-1; assert(frame_ready(&frame)); assert(frame_ready(0)); return 0; }
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'test.c').write_text(harness)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
        self.assertIn('if (frame && !frame_ready(frame))', source)
        self.assertIn('if (!video->dirty) return TRUE;', source)

    def test_release_receiver_independent_of_render_and_joined(self):
        source = (ROOT / 'native/gst-admiral-videoplane.c').read_text()
        self.assertIn('static gpointer release_receiver(', source)
        receiver = source[source.index('static gpointer release_receiver('):source.index('static void\nremove_framebuffer')]
        self.assertIn('drain_releases(self, 0)', receiver)
        self.assertIn('g_cond_wait_until', receiver)
        stop = source[source.index('admiral_plane_sink_stop('):source.index('admiral_plane_sink_set_render_rectangle(')]
        self.assertIn('g_thread_join', stop)
        self.assertLess(stop.index('g_thread_join'), stop.index('prune_framebuffers'))
        self.assertIn('held->serial == message.serial', source)

    def test_load_failure_suppresses_error_page_and_bounds_retries(self):
        source = (ROOT / 'browser/signage-browser.c').read_text()
        failure = source[source.index('static gboolean load_failed('):source.index('static void process_terminated(')]
        self.assertIn('load_attempts >= 5', failure)
        self.assertIn('return TRUE;', failure)
        self.assertIn('g_timeout_add', failure)
