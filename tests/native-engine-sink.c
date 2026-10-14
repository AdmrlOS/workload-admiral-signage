/* SPDX-License-Identifier: MIT
 * Engine-mode plane sink behaviour against the actual Gst source (no DRM/decoder).
 * Built with ASAN/UBSAN by tests/run-native-tests.sh in the ARM64 toolchain image.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "../native/gst-admiral-videoplane.c"
#include <assert.h>
#include <fcntl.h>

static gboolean readable(int fd, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, timeout_ms) > 0;
}

static AdmiralPlaneSink *new_sink(int sockets[2], gboolean engine)
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int fd = open("/dev/null", O_RDWR); assert(fd >= 0);
    AdmiralPlaneSink *self = g_object_new(admiral_plane_sink_get_type(), NULL);
    admiral_plane_sink_configure(GST_ELEMENT(self), fd, sockets[0], 3840, 2160);
    close(fd); close(sockets[0]);
    admiral_plane_sink_set_engine(GST_ELEMENT(self), engine);
    self->defer_us = 0; /* tests below exercise the immediate re-SHOW path; deferral has its own test */
    assert(admiral_plane_sink_start(GST_BASE_SINK(self)));
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12,width=64,height=32,framerate=60/1");
    assert(admiral_plane_sink_set_caps(GST_BASE_SINK(self), caps));
    gst_caps_unref(caps);
    return self;
}

static void finish(AdmiralPlaneSink *self, int peer)
{
    g_mutex_lock(&self->lock); g_hash_table_remove_all(self->held); g_mutex_unlock(&self->lock);
    close(peer); /* EOF before stop: stop must not block */
    assert(admiral_plane_sink_stop(GST_BASE_SINK(self)));
    gst_object_unref(self);
}


typedef struct { AdmiralPlaneSink *self; GstBuffer *buffer; int x, y, w, h; } RaceArgs;
static gpointer race_stream(gpointer data) { RaceArgs *a = data; admiral_plane_sink_show_frame(GST_VIDEO_SINK(a->self), a->buffer); return NULL; }
static gpointer race_rect(gpointer data) { RaceArgs *a = data; gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(a->self), a->x, a->y, a->w, a->h); return NULL; }

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

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    int s[2];

    /* 1. PRESENTED for an engine sink is posted on the bus with its exact PTS, from the
     *    receiver thread; CANCELLED is never reported as presented. */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstBus *bus = gst_bus_new();
        gst_element_set_bus(GST_ELEMENT(self), bus);
        g_mutex_lock(&self->lock);
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(9), GUINT_TO_POINTER(4));
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(10), GUINT_TO_POINTER(5));
        guint64 *a = g_new(guint64, 1); *a = 16666666; g_hash_table_insert(self->identities, GUINT_TO_POINTER(9), a);
        guint64 *b = g_new(guint64, 1); *b = 33333333; g_hash_table_insert(self->identities, GUINT_TO_POINTER(10), b);
        g_mutex_unlock(&self->lock);
        SignageVideoMessage cancelled = { .type = SIGNAGE_VIDEO_CANCELLED, .fb = 5, .serial = 10 };
        SignageVideoMessage presented = { .type = SIGNAGE_VIDEO_PRESENTED, .fb = 4, .serial = 9 };
        assert(send(s[1], &cancelled, sizeof cancelled, 0) == sizeof cancelled);
        assert(send(s[1], &presented, 3, 0) == 3); /* fragmented record */
        g_usleep(3000);
        assert(send(s[1], (char *)&presented + 3, sizeof presented - 3, 0) == sizeof presented - 3);
        GstMessage *m = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ELEMENT);
        assert(m);
        const GstStructure *st = gst_message_get_structure(m);
        assert(gst_structure_has_name(st, "admiral-presented"));
        guint64 pts = 0; guint serial = 0;
        assert(gst_structure_get_uint64(st, "pts", &pts) && pts == 16666666);
        assert(gst_structure_get_uint(st, "serial", &serial) && serial == 9);
        gst_message_unref(m);
        assert(!gst_bus_timed_pop_filtered(bus, 50 * GST_MSECOND, GST_MESSAGE_ELEMENT));
        gst_element_set_bus(GST_ELEMENT(self), NULL); gst_object_unref(bus);
        finish(self, s[1]);
    }

    /* 2. Ordinary (HTML) sinks keep the old behaviour: no bus traffic per frame. */
    {
        AdmiralPlaneSink *self = new_sink(s, FALSE);
        GstBus *bus = gst_bus_new();
        gst_element_set_bus(GST_ELEMENT(self), bus);
        g_mutex_lock(&self->lock);
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(3), GUINT_TO_POINTER(4));
        guint64 *a = g_new(guint64, 1); *a = 1; g_hash_table_insert(self->identities, GUINT_TO_POINTER(3), a);
        g_mutex_unlock(&self->lock);
        SignageVideoMessage presented = { .type = SIGNAGE_VIDEO_PRESENTED, .fb = 4, .serial = 3 };
        assert(send(s[1], &presented, sizeof presented, 0) == sizeof presented);
        assert(!gst_bus_timed_pop_filtered(bus, 60 * GST_MSECOND, GST_MESSAGE_ELEMENT));
        gst_element_set_bus(GST_ELEMENT(self), NULL); gst_object_unref(bus);
        finish(self, s[1]);
    }

    /* 3. Invisible engine sink: frames are consumed without SHOW and without holding the
     *    decoder buffer (warm-up / hidden), and the warm-up frame never becomes `last`. */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        admiral_plane_sink_set_visible(GST_ELEMENT(self), FALSE);
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, 64 * 32 * 3 / 2, NULL);
        GST_BUFFER_PTS(buffer) = 0;
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), buffer) == GST_FLOW_OK);
        assert(!readable(s[1], 30));
        g_mutex_lock(&self->lock);
        assert(self->last == NULL && g_hash_table_size(self->held) == 0 && self->serial == 0);
        g_mutex_unlock(&self->lock);
        assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 1);
        gst_buffer_unref(buffer);
        finish(self, s[1]);
    }

    /* 4. Hiding a shown engine sink sends exactly one HIDE; showing again with a paused
     *    frame re-presents that same frame (one SHOW attempt), hiding twice is idempotent. */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, 16, NULL);
        g_mutex_lock(&self->lock);
        gst_buffer_replace(&self->last, buffer); self->hidden = FALSE; self->serial = 1; self->last_presented_serial = 1;
        g_mutex_unlock(&self->lock);
        admiral_plane_sink_set_visible(GST_ELEMENT(self), FALSE);
        admiral_plane_sink_set_visible(GST_ELEMENT(self), FALSE);
        SignageVideoMessage got;
        assert(recv(s[1], &got, sizeof got, MSG_WAITALL) == sizeof got && got.type == SIGNAGE_VIDEO_HIDE);
        assert(!readable(s[1], 30));
        g_mutex_lock(&self->lock); assert(self->last == buffer && self->hidden); g_mutex_unlock(&self->lock);
        gst_buffer_unref(buffer);
        finish(self, s[1]);
    }

    /* 5. Engine flush (seek / immediate switch) keeps the last frame on screen: no HIDE,
     *    while an HTML sink still hides on flush. */
    for (int engine = 0; engine < 2; engine++) {
        AdmiralPlaneSink *self = new_sink(s, engine);
        g_mutex_lock(&self->lock); self->hidden = FALSE; g_mutex_unlock(&self->lock);
        assert(admiral_plane_sink_event(GST_BASE_SINK(self), gst_event_new_flush_start()));
        gboolean hid = readable(s[1], 30);
        if (hid) { SignageVideoMessage got; assert(recv(s[1], &got, sizeof got, MSG_WAITALL) == sizeof got && got.type == SIGNAGE_VIDEO_HIDE); }
        assert(hid == !engine);
        assert(admiral_plane_sink_event(GST_BASE_SINK(self), gst_event_new_flush_stop(FALSE)));
        finish(self, s[1]);
    }

    /* 6. A paused/prerolled engine frame that was already submitted is not SHOWn again when
     *    BaseSink renders the same buffer on resume (no duplicate PRESENTED identity). */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, 16, NULL);
        g_mutex_lock(&self->lock);
        gst_buffer_replace(&self->last, buffer); self->hidden = FALSE; self->serial = 5;
        g_mutex_unlock(&self->lock);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), buffer) == GST_FLOW_OK);
        assert(!readable(s[1], 30));
        g_mutex_lock(&self->lock); assert(self->serial == 5); g_mutex_unlock(&self->lock);
        gst_buffer_unref(buffer);
        finish(self, s[1]);
    }
    /* 7. A geometry-only re-SHOW of the frame already on screen (rectangle change / expose /
     *    visibility restore) carries no PTS identity: its PRESENTED must not be reported as a
     *    new presented frame, while the original and a genuinely new frame still are. */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstBus *bus = gst_bus_new();
        gst_element_set_bus(GST_ELEMENT(self), bus);
        g_mutex_lock(&self->lock);
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(1), GUINT_TO_POINTER(4));
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(2), GUINT_TO_POINTER(4));
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(3), GUINT_TO_POINTER(5));
        guint64 *p1 = g_new(guint64, 1); *p1 = 50000000; g_hash_table_insert(self->identities, GUINT_TO_POINTER(1), p1);
        guint64 *p2 = g_new(guint64, 1); *p2 = GST_CLOCK_TIME_NONE; g_hash_table_insert(self->identities, GUINT_TO_POINTER(2), p2);
        guint64 *p3 = g_new(guint64, 1); *p3 = 66666666; g_hash_table_insert(self->identities, GUINT_TO_POINTER(3), p3);
        self->serial = 3;
        g_mutex_unlock(&self->lock);
        for (guint32 serial = 1; serial <= 3; serial++) {
            SignageVideoMessage presented = { .type = SIGNAGE_VIDEO_PRESENTED, .fb = serial == 3 ? 5 : 4, .serial = serial };
            assert(send(s[1], &presented, sizeof presented, 0) == sizeof presented);
        }
        guint64 seen[3]; int n = 0;
        for (GstMessage *m; n < 3 && (m = gst_bus_timed_pop_filtered(bus, 300 * GST_MSECOND, GST_MESSAGE_ELEMENT)); ) {
            guint64 pts = 0; assert(gst_structure_get_uint64(gst_message_get_structure(m), "pts", &pts)); seen[n++] = pts; gst_message_unref(m);
        }
        assert(n == 2 && seen[0] == 50000000 && seen[1] == 66666666);
        g_mutex_lock(&self->lock); assert(self->last_presented); g_mutex_unlock(&self->lock);
        gst_element_set_bus(GST_ELEMENT(self), NULL); gst_object_unref(bus);
        finish(self, s[1]);
    }

    /* 8. END TO END through the real show_locked(): rectangle changes (including while the frame
     *    is still pending), full->PiP->full, hide/show restore and an off-screen frame that
     *    returns on screen. Every PTS is reported PRESENTED exactly once, in order; re-shows are
     *    geometry only and each of them still consumes and returns a credit. */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstBus *bus = gst_bus_new();
        gst_element_set_bus(GST_ELEMENT(self), bus);
        GstAllocator *alloc = gst_dmabuf_allocator_new();
        GstBuffer *frames[3]; guint32 fbs[3]; guint64 ptss[3] = { 0, 16666666, 33333333 };
        for (int i = 0; i < 3; i++) {
            int fd = memfd_create("frame", 0); assert(fd >= 0 && ftruncate(fd, 64 * 32 * 3 / 2) == 0);
            frames[i] = gst_buffer_new(); gst_buffer_append_memory(frames[i], gst_dmabuf_allocator_alloc(alloc, fd, 64 * 32 * 3 / 2));
            GST_BUFFER_PTS(frames[i]) = ptss[i]; GST_BUFFER_DURATION(frames[i]) = 16666666;
            struct stat st; assert(fstat(fd, &st) == 0);
            GString *key = g_string_new(NULL);
            for (guint p = 0; p < 2; p++)
                g_string_append_printf(key, "%lu:%u:%u;", (unsigned long)st.st_ino, (guint)GST_VIDEO_INFO_PLANE_OFFSET(&self->info, p), (guint)GST_VIDEO_INFO_PLANE_STRIDE(&self->info, p));
            g_string_append_printf(key, "%dx%d", GST_VIDEO_INFO_WIDTH(&self->info), GST_VIDEO_INFO_HEIGHT(&self->info));
            fbs[i] = 100 + i;
            g_mutex_lock(&self->lock); g_hash_table_insert(self->framebuffers, g_string_free(key, FALSE), GUINT_TO_POINTER(fbs[i])); g_mutex_unlock(&self->lock);
        }
        GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(self);
        SignageVideoMessage got; guint32 pending[8]; int npending = 0;
#define EXPECT(t, fbv) do { assert(readable(s[1], 500)); assert(recv(s[1], &got, sizeof got, MSG_WAITALL) == sizeof got); \
    assert(got.type == (t)); if (fbv) assert(got.fb == (fbv)); } while (0)
#define SHOWN(fbv) do { EXPECT(SIGNAGE_VIDEO_SHOW, fbv); pending[npending++] = got.serial; } while (0)
#define PRESENT(fbv) do { assert(npending > 0); SignageVideoMessage pm = { .type = SIGNAGE_VIDEO_PRESENTED, .fb = (fbv), .serial = pending[0] }; \
    memmove(pending, pending + 1, --npending * sizeof pending[0]); assert(send(s[1], &pm, sizeof pm, 0) == sizeof pm); g_usleep(20000); } while (0)
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), frames[0]) == GST_FLOW_OK);
        SHOWN(fbs[0]);
        /* rectangle change while frame 0 is still pending (not yet PRESENTED) */
        gst_video_overlay_set_render_rectangle(overlay, 2400, 100, 1200, 675);
        SHOWN(fbs[0]);
        PRESENT(fbs[0]); PRESENT(fbs[0]);
        /* PiP -> full -> PiP, each ack'd before the next (credit returns every time) */
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160); SHOWN(fbs[0]); PRESENT(fbs[0]);
        gst_video_overlay_set_render_rectangle(overlay, 2400, 100, 1200, 675); SHOWN(fbs[0]); PRESENT(fbs[0]);
        /* hide / restore */
        admiral_plane_sink_set_visible(GST_ELEMENT(self), FALSE); EXPECT(SIGNAGE_VIDEO_HIDE, 0);
        admiral_plane_sink_set_visible(GST_ELEMENT(self), TRUE); SHOWN(fbs[0]); PRESENT(fbs[0]);
        /* a genuinely new frame, then a rectangle change while it is pending */
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), frames[1]) == GST_FLOW_OK); SHOWN(fbs[1]);
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160); SHOWN(fbs[1]);
        PRESENT(fbs[1]); PRESENT(fbs[1]);
        /* cancelled re-show: no PRESENTED, credit returned (the next re-show must not time out) */
        gst_video_overlay_set_render_rectangle(overlay, 2400, 100, 1200, 675); SHOWN(fbs[1]);
        { SignageVideoMessage cm = { .type = SIGNAGE_VIDEO_CANCELLED, .fb = fbs[1], .serial = pending[0] }; npending = 0;
          assert(send(s[1], &cm, sizeof cm, 0) == sizeof cm); g_usleep(30000); }
        /* a frame that arrives while entirely off screen, then comes on screen */
        gst_video_overlay_set_render_rectangle(overlay, 5000, 5000, 100, 100);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), frames[2]) == GST_FLOW_OK);
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160);
        { while (readable(s[1], 30)) { assert(recv(s[1], &got, sizeof got, MSG_WAITALL) == sizeof got);
            if (got.type == SIGNAGE_VIDEO_SHOW) { assert(got.fb == fbs[2]); pending[npending++] = got.serial; } } }
        assert(npending == 1); PRESENT(fbs[2]);
        guint64 seen[8]; int n = 0;
        for (GstMessage *m; n < 8 && (m = gst_bus_timed_pop_filtered(bus, 200 * GST_MSECOND, GST_MESSAGE_ELEMENT)); ) {
            guint64 pts = 0; assert(gst_structure_get_uint64(gst_message_get_structure(m), "pts", &pts)); seen[n++] = pts; gst_message_unref(m);
        }
        assert(n == 3 && seen[0] == 0 && seen[1] == 16666666 && seen[2] == 33333333);
        g_mutex_lock(&self->lock); assert(g_hash_table_size(self->unpresented) == 0 && g_hash_table_size(self->identities) == 0); g_mutex_unlock(&self->lock);
        for (int i = 0; i < 3; i++) gst_buffer_unref(frames[i]);
        gst_object_unref(alloc);
        gst_element_set_bus(GST_ELEMENT(self), NULL); gst_object_unref(bus);
        finish(self, s[1]);
    }

    /* 10. Race: a rectangle refresh of frame A blocks for a credit (lock released) while the
     *     streaming thread also waits to SHOW the newer frame B. Whichever wakes first, the
     *     older frame A must never be put back on screen after B (hardware: duplicate/backward
     *     PTS at every rectangle change), and B must be SHOWn exactly once. */
    for (int iteration = 0; iteration < 40; iteration++) {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstAllocator *alloc = gst_dmabuf_allocator_new();
        GstBuffer *a = dmabuf_frame(self, alloc, 0, 100), *b = dmabuf_frame(self, alloc, 16666666, 101);
        GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(self);
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), a) == GST_FLOW_OK);
        gst_video_overlay_set_render_rectangle(overlay, 2400, 100, 1200, 675); /* credits now full: 2 unpresented */
        RaceArgs sa = { self, b, 0, 0, 0, 0 }, ra = { self, NULL, 0, 0, 3840, 2160 };
        GThread *ts = g_thread_new("stream", race_stream, &sa);
        if (iteration & 1) g_usleep(20000);
        GThread *tr = g_thread_new("rect", race_rect, &ra);
        g_usleep(60000);
        SignageVideoMessage m; guint32 serials[2];
        for (int i = 0; i < 2; i++) { assert(readable(s[1], 500)); assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m && m.type == SIGNAGE_VIDEO_SHOW); serials[i] = m.serial; }
        for (int i = 0; i < 2; i++) { SignageVideoMessage pm = { .type = SIGNAGE_VIDEO_PRESENTED, .fb = 100, .serial = serials[i] }; assert(send(s[1], &pm, sizeof pm, 0) == sizeof pm); }
        g_thread_join(ts); g_thread_join(tr);
        int seen_b = 0, b_shows = 0;
        while (readable(s[1], 80)) {
            assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m);
            if (m.type != SIGNAGE_VIDEO_SHOW) continue;
            if (m.fb == 101) { seen_b = 1; b_shows++; }
            else assert(!seen_b && "older frame A re-shown after newer frame B");
        }
        assert(b_shows >= 1);
        g_mutex_lock(&self->lock); assert(self->last == b); g_mutex_unlock(&self->lock);
        gst_buffer_unref(a); gst_buffer_unref(b); gst_object_unref(alloc);
        finish(self, s[1]);
    }
    /* 9. QoS and lateness handling stay at the GStreamer defaults for the engine sink as for
     *    an HTML sink: a frame that really arrives late is dropped/reported, never silently
     *    delivered. (Suppressing it made "0 QoS drops" meaningless.) */
    for (int engine = 0; engine < 2; engine++) {
        AdmiralPlaneSink *self = new_sink(s, engine);
        AdmiralPlaneSink *reference = g_object_new(admiral_plane_sink_get_type(), NULL);
        assert(gst_base_sink_is_qos_enabled(GST_BASE_SINK(self)) == gst_base_sink_is_qos_enabled(GST_BASE_SINK(reference)));
        assert(gst_base_sink_is_qos_enabled(GST_BASE_SINK(self)));
        assert(gst_base_sink_get_max_lateness(GST_BASE_SINK(self)) == gst_base_sink_get_max_lateness(GST_BASE_SINK(reference)));
        assert(gst_base_sink_get_sync(GST_BASE_SINK(self)));
        gst_object_unref(reference);
        finish(self, s[1]);
    }

    /* 11. A flush (clip cut / seek) must not make the sink forget SHOWs the browser still
     *     owns. Hardware: after FLUSH the first SHOW was a THIRD outstanding frame, overflowed
     *     the browser's two slots and the browser dropped the channel (engine abort -5).
     *     The browser model below is the real acceptance rule (pending + queued >= 2 => drop). */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        GstAllocator *alloc = gst_dmabuf_allocator_new();
        GstBuffer *a = dmabuf_frame(self, alloc, 0, 100), *b = dmabuf_frame(self, alloc, 16666666, 101), *c = dmabuf_frame(self, alloc, 33333333, 102);
        gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(self), 0, 0, 3840, 2160);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), a) == GST_FLOW_OK);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), b) == GST_FLOW_OK); /* browser owns 2 */
        g_mutex_lock(&self->lock);
        flush_start_locked(self);
        self->flushing = FALSE; /* FLUSH_STOP */
        g_mutex_unlock(&self->lock);
        SignageVideoMessage m; guint32 serials[3]; int outstanding = 0, max_outstanding = 0, n = 0;
        while (readable(s[1], 20)) { assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m); serials[n++] = m.serial; outstanding++; }
        assert(n == 2 && outstanding == 2);
        RaceArgs sa = { self, c, 0, 0, 0, 0 };
        GThread *ts = g_thread_new("stream", race_stream, &sa);
        assert(!readable(s[1], 150) && "post-flush SHOW sent while the browser still owns two frames");
        SignageVideoMessage pm = { .type = SIGNAGE_VIDEO_PRESENTED, .fb = 100, .serial = serials[0] };
        assert(send(s[1], &pm, sizeof pm, 0) == sizeof pm); outstanding--;
        assert(readable(s[1], 1000)); /* credit returned: the new frame goes out */
        assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m && m.type == SIGNAGE_VIDEO_SHOW && m.fb == 102);
        outstanding++; max_outstanding = MAX(max_outstanding, outstanding);
        assert(max_outstanding <= 2);
        g_thread_join(ts);
        gst_buffer_unref(a); gst_buffer_unref(b); gst_buffer_unref(c); gst_object_unref(alloc);
        finish(self, s[1]);
    }
    /* 11b. An HTML sink still forgets its credits on flush (it sends HIDE, which the browser
     *      answers by cancelling everything). */
    {
        AdmiralPlaneSink *self = new_sink(s, FALSE);
        g_mutex_lock(&self->lock);
        g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(1), GUINT_TO_POINTER(7));
        flush_start_locked(self);
        assert(g_hash_table_size(self->unpresented) == 0);
        g_mutex_unlock(&self->lock);
        finish(self, s[1]);
    }
    /* 12. Geometry refresh policy. While frames are arriving, a rectangle change costs no
     *     display slot (the next frame's SHOW carries the new rectangle); an idle stream is
     *     refreshed by the receiver thread. Hardware/realtime test: a separate re-SHOW at
     *     source rate == refresh rate delayed every later frame by a vblank, forever. */
    {
        AdmiralPlaneSink *self = new_sink(s, TRUE);
        self->defer_us = 120000;
        GstAllocator *alloc = gst_dmabuf_allocator_new();
        GstBuffer *a = dmabuf_frame(self, alloc, 0, 100), *b = dmabuf_frame(self, alloc, 16666666, 101);
        GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(self);
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160);
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), a) == GST_FLOW_OK);
        SignageVideoMessage m, pm;
        assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m && m.type == SIGNAGE_VIDEO_SHOW && m.fb == 100);
        pm = (SignageVideoMessage){ .type = SIGNAGE_VIDEO_PRESENTED, .fb = 100, .serial = m.serial };
        assert(send(s[1], &pm, sizeof pm, 0) == sizeof pm);
        gst_video_overlay_set_render_rectangle(overlay, 2400, 100, 1200, 675);
        assert(!readable(s[1], 40) && "rectangle change spent its own SHOW while frames are flowing");
        assert(admiral_plane_sink_show_frame(GST_VIDEO_SINK(self), b) == GST_FLOW_OK);
        assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m && m.type == SIGNAGE_VIDEO_SHOW && m.fb == 101);
        assert(m.crtc_x == 2400 && m.crtc_w == 1200 && "next frame carries the new rectangle");
        pm = (SignageVideoMessage){ .type = SIGNAGE_VIDEO_PRESENTED, .fb = 101, .serial = m.serial };
        assert(send(s[1], &pm, sizeof pm, 0) == sizeof pm);
        g_mutex_lock(&self->lock); assert(!self->geometry_pending); g_mutex_unlock(&self->lock);
        /* now the stream goes idle: the refresh happens on its own after the window */
        gst_video_overlay_set_render_rectangle(overlay, 0, 0, 3840, 2160);
        assert(!readable(s[1], 40));
        assert(readable(s[1], 600) && "idle stream never refreshed");
        assert(recv(s[1], &m, sizeof m, MSG_WAITALL) == sizeof m && m.type == SIGNAGE_VIDEO_SHOW && m.fb == 101 && m.crtc_w == 3840);
        assert(!readable(s[1], 80) && "refresh sent more than once");
        gst_buffer_unref(a); gst_buffer_unref(b); gst_object_unref(alloc);
        finish(self, s[1]);
    }
    g_print("native-engine-sink: OK\n");
    return 0;
}
