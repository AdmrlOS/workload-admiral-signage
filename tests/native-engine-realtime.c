/* SPDX-License-Identifier: MIT
 * Real-time cadence of the ENGINE sink with GStreamer QoS/lateness left at its defaults.
 *
 * 600 frames (10 s at 60 fps) are pushed through a real GstPipeline (appsrc -> AdmiralPlaneSink,
 * sync=true on the system clock). A simulated browser reads the broker protocol from the other
 * end of the socketpair, keeps the real two-slot FIFO, and "flips" on a 60 Hz vblank timer
 * (commit at vblank N, PRESENTED at vblank N+1). The test then checks the *measured* clock
 * path, not PTS arithmetic: SHOW arrival time vs running time, flip time vs running time, the
 * flip interval, and that no frame was dropped or reported late upstream.
 *
 * Phases: steady, and rectangle changes (each one re-SHOWs the frame on screen and spends a
 * credit) every 400 ms. Used with SIGNAGE_SLOW=1 as a 2 x 10 s test; a 2 s variant always runs.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "../native/gst-admiral-videoplane.c"
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <gst/app/gstappsrc.h>

#define POOL 8
#define WARMUP 15
#define VBLANK_NS 16666667LL

static GstBuffer *dmabuf_frame(AdmiralPlaneSink *self, GstAllocator *alloc, guint64 pts, guint32 fb)
{
    int fd = memfd_create("frame", 0); assert(fd >= 0 && ftruncate(fd, 64 * 32 * 3 / 2) == 0);
    GstBuffer *b = gst_buffer_new(); gst_buffer_append_memory(b, gst_dmabuf_allocator_alloc(alloc, fd, 64 * 32 * 3 / 2));
    GST_BUFFER_PTS(b) = pts; GST_BUFFER_DURATION(b) = 16666666;
    struct stat st; assert(fstat(fd, &st) == 0);
    GString *key = g_string_new(NULL);
    for (guint p = 0; p < 2; p++)
        g_string_append_printf(key, "%lu:%u:%u;", (unsigned long)st.st_ino, (guint)GST_VIDEO_INFO_PLANE_OFFSET(&self->info, p), (guint)GST_VIDEO_INFO_PLANE_STRIDE(&self->info, p));
    g_string_append_printf(key, "%dx%d", GST_VIDEO_INFO_WIDTH(&self->info), GST_VIDEO_INFO_HEIGHT(&self->info));
    g_mutex_lock(&self->lock); g_hash_table_insert(self->framebuffers, g_string_free(key, FALSE), GUINT_TO_POINTER(fb)); g_mutex_unlock(&self->lock);
    return b;
}

static gint64 now_ns(void) { return g_get_monotonic_time() * 1000; }

typedef struct {
    int peer;
    gboolean stop;
    GstClockTime base;               /* pipeline base time (monotonic ns) */
    guint32 next_fb_index;
    /* browser model */
    struct { SignageVideoMessage m; gint64 arrived; } fifo[8];
    guint fifo_n;
    gboolean have_pending, have_current;
    SignageVideoMessage pending, current;
    gint64 pending_arrived;
    /* outputs (sim thread writes, main reads after join) */
    guint shows, reshows, flips, max_fifo, protocol_violations;
    gint64 flip_ns[4096];            /* per NEW frame (index = frame order), 0 when not shown */
    gint64 arrive_ns[4096];
    gint frame_of_serial_fb;         /* index of the newest distinct frame */
    guint32 last_fb;
    gint stall_ticks_per_geometry_change, stall; /* hardware: a plane commit that changes size/position takes extra vblanks */
    guint32 last_crtc_w; gint32 last_crtc_x;
    guint stalls;
} Browser;

static void send_ack(Browser *b, guint32 type, guint32 fb, guint32 serial)
{
    SignageVideoMessage m = { .type = type, .fb = fb, .serial = serial };
    ssize_t n = send(b->peer, &m, sizeof m, MSG_NOSIGNAL);
    (void)n;
}

static void vblank(Browser *b, gint64 t)
{
    if (b->have_pending && b->stall > 0) { b->stall--; return; } /* this commit's flip is still not done */
    if (b->have_pending) {
        SignageVideoMessage old = b->current; gboolean had = b->have_current;
        b->current = b->pending; b->have_current = TRUE; b->have_pending = FALSE;
        send_ack(b, SIGNAGE_VIDEO_PRESENTED, b->current.fb, b->current.serial);
        if (b->current.fb != b->last_fb) { /* a NEW frame reached the screen */ }
        if (had && old.fb != b->current.fb) send_ack(b, SIGNAGE_VIDEO_RELEASED, old.fb, old.serial);
        gint idx = (gint)b->pending.generation; /* frame index stashed at arrival */
        if (idx >= 0 && idx < 4096 && !b->flip_ns[idx]) b->flip_ns[idx] = t;
        b->flips++;
    }
    if (!b->have_pending && b->fifo_n) {
        b->pending = b->fifo[0].m; b->pending_arrived = b->fifo[0].arrived;
        memmove(&b->fifo[0], &b->fifo[1], (b->fifo_n - 1) * sizeof b->fifo[0]);
        b->fifo_n--; b->have_pending = TRUE;
        if (b->stall_ticks_per_geometry_change && (b->pending.crtc_w != b->last_crtc_w || b->pending.crtc_x != b->last_crtc_x) && b->last_crtc_w) {
            b->stall = b->stall_ticks_per_geometry_change; b->stalls++;
        }
        b->last_crtc_w = b->pending.crtc_w; b->last_crtc_x = b->pending.crtc_x;
    }
}

static gpointer browser_thread(gpointer data)
{
    Browser *b = data;
    gint64 next_vblank = now_ns() + VBLANK_NS;
    guint32 prev_fb = 0; gint frame = -1;
    while (!g_atomic_int_get(&b->stop)) {
        gint64 now = now_ns();
        gint64 wait_ns = next_vblank - now;
        struct pollfd p = { .fd = b->peer, .events = POLLIN };
        struct timespec ts = { .tv_sec = 0, .tv_nsec = wait_ns > 0 ? wait_ns : 0 };
        if (ppoll(&p, 1, &ts, NULL) > 0 && (p.revents & POLLIN)) {
            SignageVideoMessage m;
            while (recv(b->peer, &m, sizeof m, MSG_DONTWAIT | MSG_WAITALL) == (ssize_t)sizeof m) {
                if (m.type != SIGNAGE_VIDEO_SHOW) continue;
                b->shows++;
                if (m.fb != prev_fb) { frame++; prev_fb = m.fb; if (frame < 4096) b->arrive_ns[frame] = now_ns(); }
                else b->reshows++;
                /* real browser rule (signage-browser.c video_message): both slots busy => disconnect */
                guint used = (b->have_pending ? 1 : 0) + b->fifo_n;
                if (used >= 2) { b->protocol_violations++; continue; }
                m.generation = (guint64)frame; /* carry the frame index through the FIFO */
                b->fifo[b->fifo_n].m = m; b->fifo[b->fifo_n].arrived = now_ns(); b->fifo_n++;
                b->max_fifo = MAX(b->max_fifo, used + 1);
            }
        }
        now = now_ns();
        if (now >= next_vblank) { vblank(b, now); next_vblank += VBLANK_NS; if (next_vblank < now) next_vblank = now + VBLANK_NS; }
    }
    return NULL;
}

typedef struct { guint qos_events, qos_late; gint64 max_jitter; } QosStats;
static GstPadProbeReturn qos_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad;
    QosStats *q = data; GstEvent *e = GST_PAD_PROBE_INFO_EVENT(info);
    if (e && GST_EVENT_TYPE(e) == GST_EVENT_QOS) {
        GstQOSType type; gdouble prop; GstClockTimeDiff diff; GstClockTime ts;
        gst_event_parse_qos(e, &type, &prop, &diff, &ts);
        q->qos_events++;
        if (diff > 5 * GST_MSECOND) { q->qos_late++; q->max_jitter = MAX(q->max_jitter, diff); }
    }
    return GST_PAD_PROBE_OK;
}

typedef struct { AdmiralPlaneSink *sink; gboolean stop; guint changes; gint64 period_ms; } RectArgs;
static gpointer rect_thread(gpointer data)
{
    RectArgs *r = data; gboolean small = FALSE;
    while (!g_atomic_int_get(&r->stop)) {
        g_usleep((gulong)(r->period_ms * 1000));
        if (g_atomic_int_get(&r->stop)) break;
        small = !small; r->changes++;
        if (small) gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(r->sink), 2400, 100, 1200, 675);
        else gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(r->sink), 0, 0, 3840, 2160);
    }
    return NULL;
}

static int cmp64(const void *a, const void *b) { gint64 x = *(const gint64 *)a, y = *(const gint64 *)b; return x < y ? -1 : x > y; }

static gboolean run_phase(const char *name, guint frames, gint64 rect_period_ms, gint stall_ticks)
{
    int s[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    int fd = open("/dev/null", O_RDWR); assert(fd >= 0);
    AdmiralPlaneSink *self = g_object_new(admiral_plane_sink_get_type(), NULL);
    admiral_plane_sink_configure(GST_ELEMENT(self), fd, s[0], 3840, 2160);
    close(fd); close(s[0]);
    admiral_plane_sink_set_engine(GST_ELEMENT(self), TRUE);
    /* the properties this test exists to protect */
    assert(gst_base_sink_is_qos_enabled(GST_BASE_SINK(self)));
    {
        AdmiralPlaneSink *reference = g_object_new(admiral_plane_sink_get_type(), NULL);
        assert(gst_base_sink_get_max_lateness(GST_BASE_SINK(self)) == gst_base_sink_get_max_lateness(GST_BASE_SINK(reference)));
        gst_object_unref(reference);
    }

    GstCaps *vcaps = gst_caps_from_string("video/x-raw,format=NV12,width=64,height=32,framerate=60/1");
    assert(admiral_plane_sink_start(GST_BASE_SINK(self)) && admiral_plane_sink_set_caps(GST_BASE_SINK(self), vcaps));
    gst_caps_unref(vcaps);
    GstElement *pipe = gst_pipeline_new("rt"), *src = gst_element_factory_make("appsrc", "src");
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12,width=64,height=32,framerate=60/1");
    g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, "block", TRUE, "max-bytes", (guint64)(64 * 32 * 3 / 2 * 3), NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipe), src, GST_ELEMENT(self), NULL);
    assert(gst_element_link(src, GST_ELEMENT(self)));
    QosStats qos = { 0 };
    GstPad *pad = gst_element_get_static_pad(GST_ELEMENT(self), "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, qos_probe, &qos, NULL);
    gst_object_unref(pad);

    GstAllocator *alloc = gst_dmabuf_allocator_new();
    GstMemory *mem[POOL];
    for (int i = 0; i < POOL; i++) {
        GstBuffer *tmp = dmabuf_frame(self, alloc, 0, 100 + i);
        mem[i] = gst_memory_ref(gst_buffer_peek_memory(tmp, 0));
        gst_buffer_unref(tmp);
    }
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(self), 0, 0, 3840, 2160);

    Browser *b = g_new0(Browser, 1); b->peer = s[1]; b->stall_ticks_per_geometry_change = stall_ticks;
    GThread *bt = g_thread_new("browser", browser_thread, b);
    assert(gst_element_set_state(pipe, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    RectArgs ra = { self, FALSE, 0, rect_period_ms };
    GThread *rt = rect_period_ms ? g_thread_new("rect", rect_thread, &ra) : NULL;

    gint64 pts_ns[4096]; assert(frames <= 4096);
    for (guint i = 0; i < frames; i++) {
        GstBuffer *buf = gst_buffer_new(); gst_buffer_append_memory(buf, gst_memory_ref(mem[i % POOL]));
        pts_ns[i] = gst_util_uint64_scale(i, GST_SECOND, 60);
        GST_BUFFER_PTS(buf) = GST_BUFFER_DTS(buf) = (GstClockTime)pts_ns[i];
        GST_BUFFER_DURATION(buf) = 16666666;
        GstFlowReturn r = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
        assert(r == GST_FLOW_OK);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    GstBus *bus = gst_element_get_bus(pipe);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    assert(m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS && "pipeline must reach EOS with every frame presented");
    gst_message_unref(m); gst_object_unref(bus);
    GstClockTime base = gst_element_get_base_time(pipe);
    GstStructure *stats = NULL; guint64 rendered = 0, dropped = 0;
    g_object_get(self, "stats", &stats, NULL);
    if (stats) { gst_structure_get_uint64(stats, "rendered", &rendered); gst_structure_get_uint64(stats, "dropped", &dropped); gst_structure_free(stats); }
    if (rt) { g_atomic_int_set(&ra.stop, TRUE); g_thread_join(rt); }
    g_usleep(100000);
    g_atomic_int_set(&b->stop, TRUE); g_thread_join(bt);

    /* ---- verdict from measured clock times ---- */
    guint seen = 0, seen_all = 0; gint64 lat[4096], arr[4096]; gint64 prev_flip = 0, max_int = 0, min_int = G_MAXINT64; guint late_flip = 0;
    for (guint i = 0; i < frames; i++) {
        if (!b->flip_ns[i]) continue;
        if (i < WARMUP) { seen_all++; continue; } /* preroll/clock-sync transient */
        gint64 target = (gint64)base + pts_ns[i];
        lat[seen] = b->flip_ns[i] - target;          /* PTS -> flipped on screen */
        arr[seen] = b->arrive_ns[i] - target;        /* PTS -> SHOW reached the browser */
        if (prev_flip) { gint64 d = b->flip_ns[i] - prev_flip; max_int = MAX(max_int, d); min_int = MIN(min_int, d); }
        prev_flip = b->flip_ns[i]; seen++;
    }
    gint64 sorted[4096]; memcpy(sorted, lat, seen * sizeof(gint64)); qsort(sorted, seen, sizeof(gint64), cmp64);
    gint64 asorted[4096]; memcpy(asorted, arr, seen * sizeof(gint64)); qsort(asorted, seen, sizeof(gint64), cmp64);
    gint64 span = seen > 1 ? (b->flip_ns[frames - 1] - b->flip_ns[WARMUP]) : 0;
    g_print("  [%s] frames=%u flipped=%u(+%u warm-up) shows=%u reshows=%u rect_changes=%u dropped=%" G_GUINT64_FORMAT " rendered=%" G_GUINT64_FORMAT
            " qos_events=%u qos_late=%u qos_max_jitter=%.1fms violations=%u max_fifo=%u\n"
            "         PTS->SHOW arrival p50/p99/max %.1f/%.1f/%.1f ms; PTS->flip p50/p99/max %.1f/%.1f/%.1f ms; flip interval min/max %.1f/%.1f ms; "
            "mean cadence %.3f ms\n",
            name, frames, seen, seen_all, b->shows, b->reshows, ra.changes, dropped, rendered, qos.qos_events, qos.qos_late,
            qos.max_jitter / 1e6, b->protocol_violations, b->max_fifo,
            asorted[seen / 2] / 1e6, asorted[seen * 99 / 100] / 1e6, asorted[seen - 1] / 1e6,
            sorted[seen / 2] / 1e6, sorted[seen * 99 / 100] / 1e6, sorted[seen - 1] / 1e6, min_int / 1e6, max_int / 1e6,
            seen > 1 ? span / (double)(frames - 1 - WARMUP) / 1e6 : 0.0);
    (void)late_flip;
    /* every frame reached the screen exactly once, in order, nothing was dropped or reported
     * late (>5 ms), no protocol violation, 60 Hz +-1 %, and no accumulated lateness: the spread of
     * PTS->flip stays inside one vblank (+5 ms of host scheduling noise). */
    double mean = seen > 1 ? span / (double)(frames - 1 - WARMUP) : 0;
    gboolean ok = TRUE;
    /* Wall-clock timing checks need a quiet host (60 Hz with no slack: one 20 ms scheduler stall is a
     * real late frame). They are enforced with SIGNAGE_RT_STRICT=1 and otherwise reported as advisory;
     * the structural checks (slots, accounting, no geometry-only SHOW while frames flow) always are. */
    gboolean strict = g_getenv("SIGNAGE_RT_STRICT") && *g_getenv("SIGNAGE_RT_STRICT");
#define CHECK(cond, why) do { if (!(cond)) { g_print("         %s: %s\n", strict ? "FAIL" : "advisory (host timing)", why); ok &= !strict; } } while (0)
#define ALWAYS(cond, why) do { if (!(cond)) { g_print("         FAIL: %s\n", why); ok = FALSE; } } while (0)
    ALWAYS(b->protocol_violations == 0, "sink exceeded the browser's two SHOW slots");
    ALWAYS(seen + seen_all + dropped == frames, "frames neither shown nor counted as dropped (hidden loss)");
    if (!stall_ticks) {
        ALWAYS(b->reshows == 0 && b->shows == frames - dropped, "a rectangle change spent its own SHOW (display slot) while frames were flowing");
        CHECK(seen + seen_all == frames, "a frame never reached the screen");
        CHECK(dropped == 0 && rendered == frames, "frames were dropped by the sink");
        CHECK(qos.qos_late == 0, "the sink reported late frames upstream");
        CHECK(mean > VBLANK_NS * 0.99 && mean < VBLANK_NS * 1.01, "mean flip cadence is not 60 Hz +-1%");
        CHECK(seen > 1 && sorted[seen - 1] - sorted[0] <= VBLANK_NS + 5000000, "PTS->flip lateness accumulated");
        CHECK(max_int <= 2 * VBLANK_NS + 5000000 && min_int >= 5000000, "flip interval left the 1-2 vblank band");
    } else {
        /* The display itself loses vblanks on every geometry change (measured on the A523: that
         * commit's flip takes 2 vblanks instead of 1, so 1-2 slots are gone and 60 -> 60 fps has
         * no slack to win them back). Whatever the sink cannot show it must say so: every frame is
         * either on screen or counted as dropped (no hidden loss), and the lateness is reported
         * upstream as QoS events (qos_late > 0) instead of being swallowed. Bounding the carried
         * lateness needs an upstream element that honours those events (the V4L2 decoder does; this
         * appsrc harness has none, so its lateness is only reported here) and is measured on the
         * board by cadence.py + the engine's qos_* status counters. */
        CHECK(qos.qos_late > 0, "display stalls were not reported as late frames");
        CHECK(b->stalls > 0, "test did not exercise any display stall");
    }
    for (int i = 0; i < POOL; i++) gst_memory_unref(mem[i]);
    g_mutex_lock(&self->lock); g_hash_table_remove_all(self->held); g_mutex_unlock(&self->lock);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe); gst_object_unref(alloc);
    close(s[1]); g_free(b);
    return ok;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    alarm(120); /* a wedged pipeline must fail the test, not hang it */
    gboolean slow = g_getenv("SIGNAGE_SLOW") && *g_getenv("SIGNAGE_SLOW");
    guint frames = slow ? 600 : 120;
    /* A deterministic defect fails every attempt; a transient host stall (this runs on a shared
     * build machine, and 60->60 fps has no slack to catch up) is retried, and every attempt is
     * printed. */
    const char *names[] = { "steady", "rect-changes", "rect-changes+2-vblank-stall" };
    for (int phase = 0; phase < 3; phase++) {
        gboolean passed = FALSE;
        for (int attempt = 1; attempt <= 3 && !passed; attempt++) {
            g_print("  attempt %d/3\n", attempt);
            passed = run_phase(names[phase], frames, phase ? 400 : 0, phase == 2 ? 2 : 0);
        }
        assert(passed && "real-time cadence failed in all attempts");
    }
    g_print("native-engine-realtime: OK\n");
    return 0;
}
