"""Execute the production UI-only native preparation/activation path
(standby_progress from browser/standby-renderer.h) with instrumented KMS and
broker calls. Ordering/ownership evidence only: not hardware KMS TEST_ONLY,
scanout or presentation measurement.
"""
from pathlib import Path
import subprocess, tempfile, unittest

R = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <glib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "standby-protocol.h"
''' + '%TYPES%' + r'''
typedef struct { uint32_t fb; gboolean ready; void *owner; } Frame;
typedef struct { VideoPlane video; void *staged_view,*retiring_view; guint64 cancelled_generation,dead_generation,stage_ui_generation; int64_t stage_deadline; gboolean stage_ui_ready,activation_requested,commit_pending,activation_endpoint,retiring_no_video; int64_t activation_deadline; Frame *activation_frame; Frame *queued,*current,*pending; int fd; uint32_t crtc; } Browser;
typedef struct { unsigned properties; uint64_t video_fb; int video_fb_set; } drmModeAtomicReq;
#define DRM_MODE_ATOMIC_TEST_ONLY 0x100
static unsigned tests, commits, acks, recycled, destroyed;
static char last_notice[32];
static gboolean kms_ok = TRUE;
static guint64 broker_active;
static const uint32_t VIDEO_FB_PROP = 77;
static drmModeAtomicReq *drmModeAtomicAlloc(void) { return g_new0(drmModeAtomicReq, 1); }
static void drmModeAtomicFree(drmModeAtomicReq *r) { g_free(r); }
static int drmModeAtomicAddProperty(drmModeAtomicReq *r, uint32_t o, uint32_t p, uint64_t v) {
    (void)o; if (p == VIDEO_FB_PROP) { r->video_fb = v; r->video_fb_set = 1; } r->properties++; return 0; }
static int drmModeAtomicCommit(int fd, drmModeAtomicReq *r, uint32_t flags, void *data) {
    (void)fd; (void)data; assert(flags == DRM_MODE_ATOMIC_TEST_ONLY);
    /* TEST_ONLY of a UI-only state always disables the video plane. */
    assert(r->video_fb_set && r->video_fb == 0);
    tests++; return kms_ok ? 0 : -1; }
#define ADD(o,p,v) do { if (drmModeAtomicAddProperty(request,o,p,v) < 0) valid = FALSE; } while (0)
static gboolean add_ui_state(Browser *b, drmModeAtomicReq *r, Frame *f) { (void)b; assert(f->ready && f->fb); r->properties++; return TRUE; }
static gboolean frame_ready(Frame *f) { return f->ready; }
static void recycle_stage_view(Browser *b, Browser *v) { (void)b; (void)v; recycled++; }
static void destroy_stage_view(Browser *b) { (void)b; destroyed++; }
static void retain_or_destroy_staged(Browser *b, Browser *v) { (void)b; (void)v; }
static void release_frame(Browser *b, Frame *f) { (void)b; (void)f; }
static gboolean signage_drm_broker_video_tx_idle(guint64 g) { (void)g; return TRUE; }
static gboolean signage_drm_broker_video_connected(guint64 g) { (void)g; return FALSE; }
static guint64 signage_drm_broker_video_generation(void) { return broker_active; }
static void signage_drm_broker_video_retired(guint64 g) { (void)g; }
/* UI-only never PREPARED/ACTIVATED-acks a producer: there is none. */
static void signage_drm_broker_video_ack(guint64 g, uint32_t type, uint32_t fb, uint32_t serial) { (void)g; (void)type; (void)fb; (void)serial; acks++; }
#define SIGNAGE_VIDEO_PREPARED 6
static void native_notice(Browser *b, const char *e, guint64 g) { (void)b; (void)g; g_strlcpy(last_notice, e, sizeof last_notice); }
static Frame *committed_ui; static VideoState committed_video; static gboolean committed_with_video;
static gboolean submit_commit(Browser *b) {
    /* One atomic commit: incoming UI plus (only when outgoing video is on screen) its HIDE. */
    assert(b->queued && b->video.gate.phase == SIGNAGE_STANDBY_COMMITTING);
    committed_ui = b->queued; committed_with_video = b->video.dirty; committed_video = b->video.next;
    commits++; return TRUE; }
''' + '%FUNCS%' + r'''
static void setup(Browser *b, Browser *stage, Frame *ui, guint64 active, guint64 token) {
    memset(b, 0, sizeof *b); memset(stage, 0, sizeof *stage);
    tests = commits = acks = recycled = destroyed = 0; last_notice[0] = 0; kms_ok = TRUE;
    b->video.plane = 1; b->video.fb = VIDEO_FB_PROP;
    broker_active = active; b->staged_view = stage; stage->queued = ui;
    assert(signage_standby_prepare_ui(&b->video.gate, active, token));
}

int main(void) {
    Browser b, stage; Frame ui = { .fb = 30, .ready = FALSE };

    /* 1. Startup: no outgoing endpoint, no broker producer, no video frame. */
    setup(&b, &stage, &ui, 0, 5);
    standby_progress(&b); assert(!tests && !last_notice[0]);            /* no ui-ready yet */
    b.stage_ui_ready = TRUE; standby_progress(&b); assert(!tests);      /* export fence pending */
    ui.ready = TRUE; kms_ok = FALSE; standby_progress(&b);
    assert(tests == 1 && b.video.gate.phase == SIGNAGE_STANDBY_PREPARING && !last_notice[0]);
    kms_ok = TRUE; standby_progress(&b);
    assert(b.video.gate.phase == SIGNAGE_STANDBY_READY && !strcmp(last_notice, "prepared") && acks == 0);
    assert(b.stage_ui_generation == 1 && !b.video.standby.fb);
    standby_progress(&b); assert(!commits);                             /* not requested */
    b.activation_requested = TRUE; b.activation_deadline = g_get_monotonic_time() + 2000000;
    b.video.current = (VideoState){ .fb = 99, .serial = 3, .generation = 0 };
    standby_progress(&b); assert(!commits);                             /* never hide a stray video */
    b.video.current = (VideoState){0};
    b.commit_pending = TRUE; standby_progress(&b); assert(!commits);
    b.commit_pending = FALSE; standby_progress(&b);
    assert(commits == 1 && committed_ui == &ui && !committed_with_video && b.activation_frame == &ui);
    assert(!stage.queued && b.video.gate.first_fb == 30 && !b.activation_requested && acks == 0);
    assert(!signage_standby_flipped_ui(&b.video.gate, 5, 31));
    assert(signage_standby_flipped_ui(&b.video.gate, 5, 30) && b.video.gate.phase == SIGNAGE_STANDBY_EMPTY);

    /* 2. HTML -> HTML: retiring UI-only view is recycled only once its UI is off screen. */
    Frame old_ui = { .fb = 31, .ready = TRUE }; Browser old_view = {0}; old_ui.owner = &old_view;
    Frame ui2 = { .fb = 32, .ready = TRUE };
    setup(&b, &stage, &ui2, 0, 6);
    b.current = &old_ui; b.retiring_view = &old_view; b.retiring_no_video = TRUE;
    b.video.gate.phase = SIGNAGE_STANDBY_EMPTY;                         /* previous activation done */
    standby_progress(&b); assert(!recycled);                            /* old UI still scanned out */
    b.current = &ui2; standby_progress(&b); assert(recycled == 1 && !b.retiring_view && !destroyed);

    /* 3. video -> HTML at EOS: wait for the exact outgoing endpoint interval; HIDE in same commit. */
    Frame ui3 = { .fb = 33, .ready = TRUE };
    setup(&b, &stage, &ui3, 7, 8);
    b.video.current = (VideoState){ .fb = 50, .serial = 600, .generation = 7 };
    b.stage_ui_ready = TRUE; standby_progress(&b); assert(b.video.gate.phase == SIGNAGE_STANDBY_READY);
    b.activation_requested = TRUE; b.activation_endpoint = TRUE;
    b.activation_deadline = g_get_monotonic_time() + 2000000;
    standby_progress(&b); assert(!commits);                             /* no endpoint proof */
    guint64 now = g_get_monotonic_time();
    assert(signage_standby_endpoint(&b.video.gate, 7, 600, 50, now, 100000));
    standby_progress(&b); assert(!commits);                             /* final interval incomplete */
    b.video.gate.final_presented_at = now - 100001;
    broker_active = 9; standby_progress(&b); assert(!commits);           /* outgoing stream changed */
    broker_active = 7; standby_progress(&b);
    assert(commits == 1 && committed_ui == &ui3 && committed_with_video && committed_video.fb == 0);
    assert(signage_standby_flipped_ui(&b.video.gate, 8, 33) && b.video.gate.phase == SIGNAGE_STANDBY_RETIRING);
    b.video.current = (VideoState){0}; b.video.dirty = FALSE;
    standby_progress(&b);
    assert(b.video.gate.phase == SIGNAGE_STANDBY_EMPTY && b.video.gate.active == 0);

    /* 4. Timed HTML (with video) -> UI-only without EOS: no endpoint wait, hide in same commit. */
    Frame ui4 = { .fb = 34, .ready = TRUE };
    setup(&b, &stage, &ui4, 11, 12);
    b.video.current = (VideoState){ .fb = 60, .serial = 9, .generation = 11 };
    b.stage_ui_ready = TRUE; standby_progress(&b);
    b.activation_requested = TRUE; b.activation_deadline = g_get_monotonic_time() + 2000000;
    standby_progress(&b);
    assert(commits == 1 && committed_with_video && committed_video.fb == 0 && committed_ui == &ui4);

    /* 5. Bounded activation: a missing endpoint reports failed; outgoing stays on screen. */
    Frame ui5 = { .fb = 35, .ready = TRUE };
    setup(&b, &stage, &ui5, 13, 14);
    b.video.current = (VideoState){ .fb = 70, .serial = 2, .generation = 13 };
    b.stage_ui_ready = TRUE; standby_progress(&b);
    b.activation_requested = TRUE; b.activation_endpoint = TRUE;
    b.activation_deadline = g_get_monotonic_time() - 1;
    standby_progress(&b);
    assert(!commits && !strcmp(last_notice, "failed") && !b.activation_requested);
    assert(b.video.gate.phase == SIGNAGE_STANDBY_READY && b.video.current.fb == 70);
    assert(signage_standby_cancel(&b.video.gate, 14));                  /* player cancels, slot empty */

    /* 6. Stale-token / input: a video-path ready can never complete a UI-only stage. */
    Frame ui6 = { .fb = 36, .ready = TRUE };
    setup(&b, &stage, &ui6, 0, 15);
    b.video.standby = (VideoState){ .fb = 80, .serial = 1, .generation = 15 };
    b.stage_ui_ready = TRUE; standby_progress(&b);
    assert(b.video.gate.phase == SIGNAGE_STANDBY_READY && b.video.gate.first_fb == 36 && !b.video.gate.first_serial);
    return 0;
}
'''


class NativeUiOnly(unittest.TestCase):
    def test_startup_html_html_video_html_bounded_and_no_fake_video(self):
        browser = (R / 'browser/signage-browser.c').read_text()
        types = browser[browser.index('typedef struct {\n    uint32_t fb, serial;'):browser.index('/* Per reporting interval;')]
        production = (R / 'browser/standby-renderer.h').read_text()
        funcs = production[production.index('static gboolean generation_busy('):production.index('static gboolean standby_tick(')]
        source = HARNESS.replace('%TYPES%', types).replace('%FUNCS%', funcs)
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
        with tempfile.TemporaryDirectory() as d:
            p = Path(d); (p / 'test.c').write_text(source)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(R / 'native'),
                            str(p / 'test.c'), '-o', str(p / 'test'), *flags], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=20)

    def test_flip_activation_routes_only_after_matching_ui_flip(self):
        source = (R / 'browser/signage-browser.c').read_text()
        start = source.index('static void frame_presented(')
        body = source[start:source.index('\n}\n', start)]
        ui = body.index('browser->activation_frame == browser->current')
        # ACK/route/deactivate only inside the matching-flip branch, after the UI swap.
        self.assertLess(body.index('browser->current = browser->pending;'), ui)
        branch = body[ui:body.index('native_notice(browser, "activated", token)', ui)]
        self.assertIn('signage_standby_flipped_ui(gate, token, browser->current->fb)', branch)
        self.assertIn('signage_drm_broker_video_deactivate(outgoing)', branch)
        video = (R / 'browser/signage-browser.c').read_text()
        v = video[video.index('static void video_message('):]
        v = v[:v.index('\n}\n')]
        # Outgoing SHOWs during/after a UI-only activation are CANCELLED, never queued.
        guard = v.index('video->gate.ui_only && generation == video->gate.active')
        self.assertLess(guard, v.index('if (message->type == SIGNAGE_VIDEO_SHOW) {'))

    def test_broker_ui_token_owns_no_slot(self):
        broker = (R / 'native/drm-broker.c').read_text()
        token = broker[broker.index('guint64 signage_drm_broker_ui_token(void)'):]
        token = token[:token.index('\n')]
        self.assertNotIn('clients', token)
        self.assertNotIn('reserved', token)


if __name__ == '__main__':
    unittest.main()
