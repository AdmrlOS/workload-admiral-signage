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
    g_print("native-engine-sink: OK\n");
    return 0;
}
