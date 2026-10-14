/* SPDX-License-Identifier: MIT
 * Engine-mode plane sink behaviour against the actual Gst source (no DRM/decoder).
 * Built with ASAN/UBSAN by tests/run-native-tests.sh in the ARM64 toolchain image.
 */
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
    g_print("native-engine-sink: OK\n");
    return 0;
}
