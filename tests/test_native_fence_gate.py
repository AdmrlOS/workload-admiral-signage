"""Execute the real frame_render_done() from browser/signage-browser.c with a fake dma-buf
ioctl that returns a real pollable sync_file fd (a pipe): unsignalled -> not ready (UI commit
withheld, video-only commit proceeds), signalled -> ready once and fds closed, EINTR -> retry
without disabling the gate, other errors -> gate permanently disabled (old behaviour), and every
exported sync_file fd is closed (no fd leak)."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FenceGateTests(unittest.TestCase):
    def test_frame_render_done_fence_lifecycle(self):
        source = (ROOT / 'browser/signage-browser.c').read_text()
        start = source.index('static gboolean frame_render_done(Frame *frame)\n{')
        function = source[start:source.index('\nstatic gboolean frame_ready(const Frame *frame)\n{', start)]
        harness = r'''#define _GNU_SOURCE
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#define gboolean int
#define TRUE 1
#define FALSE 0
#define g_message(...) ((void)0)
#define g_strerror strerror
struct signage_dma_buf_export_sync_file { uint32_t flags; int32_t fd; };
#define DMA_BUF_SYNC_READ_FLAG 1u
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE 0xb002u
typedef struct { int dmabuf_fd; gboolean render_done; } Frame;
static int mode, calls, pipe_w = -1, open_sync, wends[16], nw;
static void close_wends(void) { while (nw) close(wends[--nw]); }
static int fake_ioctl(int fd, unsigned long req, void *arg) {
    (void)fd; (void)req; calls++;
    struct signage_dma_buf_export_sync_file *r = arg;
    if (mode == 1) { errno = EINTR; return -1; }
    if (mode == 2) { errno = ENOTTY; return -1; }
    int p[2]; assert(pipe(p) == 0); open_sync++;
    if (mode == 3) { assert(write(p[1], "x", 1) == 1); }
    pipe_w = p[1]; wends[nw++] = p[1];
    r->fd = p[0];
    return 0;
}
#define ioctl fake_ioctl
''' + function + r'''
static int count_open_fds(void) { int n = 0; for (int i = 0; i < 256; i++) if (fcntl(i, F_GETFD) != -1) n++; return n; }
int main(void) {
    int base = count_open_fds();
    /* no dmabuf (GPU-copy path) or already done: never gates */
    Frame none = { -1, FALSE }; assert(frame_render_done(&none));
    /* unsignalled implicit fence: withheld, dmabuf kept, sync_file closed each poll */
    mode = 0; int dma = dup(0); Frame f = { dma, FALSE };
    assert(!frame_render_done(&f)); assert(!frame_render_done(&f)); assert(!f.render_done && f.dmabuf_fd == dma);
    assert(calls == 2);
    close_wends(); assert(count_open_fds() == base + 1 /* only the dmabuf dup: no sync_file leaked */);
    /* signalled: ready, dmabuf dup released, sticky */
    mode = 3; assert(frame_render_done(&f)); assert(f.render_done && f.dmabuf_fd == -1);
    int c = calls; assert(frame_render_done(&f)); assert(calls == c);
    close_wends();
    assert(count_open_fds() == base);
    /* EINTR: do not disable the gate; next poll still asks the kernel */
    mode = 1; Frame g = { dup(0), FALSE }; assert(frame_render_done(&g)); mode = 0;
    int c2 = calls; assert(!frame_render_done(&g)); assert(calls == c2 + 1); close_wends();
    /* unsupported: ready (old behaviour) and sticky for every later frame */
    mode = 2; assert(frame_render_done(&g)); mode = 0;
    Frame h = { dup(0), FALSE }; int c3 = calls; assert(frame_render_done(&h)); assert(calls == c3);
    close(g.dmabuf_fd); close(h.dmabuf_fd);
    assert(count_open_fds() == base);
    puts("fence gate OK"); return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_submit_commit_gates_ui_on_both_fences_and_video_still_commits(self):
        source = (ROOT / 'browser/signage-browser.c').read_text()
        self.assertIn('if (frame && (!frame_ready(frame) || !frame_render_done(frame)))', source)
        gate = source[source.index('(!frame_ready(frame) || !frame_render_done(frame))'):]
        gate = gate[:gate.index('\n    }\n') + 7]
        self.assertIn('retry_fence', gate)
        self.assertNotIn('return FALSE', gate.split('retry_fence')[0])


if __name__ == '__main__':
    unittest.main()
