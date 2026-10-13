/* SPDX-License-Identifier: MIT
 * Actual Gst sink lifecycle test; no DRM master/display/decoder required.
 * cc ... tests/native-release-lifecycle.c $(pkg-config ...)
 */
#include "../native/gst-admiral-videoplane.c"
#include <assert.h>
#include <fcntl.h>

static gpointer broker_peer(gpointer data)
{
    int fd = GPOINTER_TO_INT(data);
    SignageVideoMessage message;
    ssize_t n = recv(fd, &message, sizeof(message), MSG_WAITALL);
    assert(n == sizeof(message) && message.type == SIGNAGE_VIDEO_HIDE);
    SignageVideoMessage released = { .type = SIGNAGE_VIDEO_RELEASED, .fb = 7, .serial = 2 };
    assert(send(fd, &released, sizeof(released), 0) == sizeof(released));
    return NULL;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    AdmiralPlaneSink *caps_sink = g_object_new(admiral_plane_sink_get_type(), NULL);
    for (int fps = 60; fps >= 0; fps -= 30) {
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, 3840, "height", G_TYPE_INT, 2160,
            "framerate", GST_TYPE_FRACTION, fps, 1, NULL);
        assert(admiral_plane_sink_set_caps(GST_BASE_SINK(caps_sink), caps));
        assert(gst_base_sink_get_render_delay(GST_BASE_SINK(caps_sink)) == (fps ? gst_util_uint64_scale(2 * GST_SECOND, 1, fps) : 0));
        assert(gst_base_sink_get_processing_deadline(GST_BASE_SINK(caps_sink)) == (fps ? gst_util_uint64_scale(3 * GST_SECOND, 1, fps) : 0));
        gst_caps_unref(caps);
    }
    gst_object_unref(caps_sink);
    for (int i = 0; i < 100; i++) {
        int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        int fd = open("/dev/null", O_RDWR); assert(fd >= 0);
        AdmiralPlaneSink *self = g_object_new(admiral_plane_sink_get_type(), NULL);
        admiral_plane_sink_configure(GST_ELEMENT(self), fd, sockets[0], 3840, 2160);
        close(fd); close(sockets[0]); /* child duplicates must survive parent closure */
        assert(admiral_plane_sink_start(GST_BASE_SINK(self)));
        GstBuffer *buffer = gst_buffer_new();
        HeldFrame *held = g_new0(HeldFrame, 1); held->serial = 2; held->buffer = gst_buffer_ref(buffer);
        g_mutex_lock(&self->lock);
        g_hash_table_insert(self->held, GUINT_TO_POINTER(7), held);
        g_mutex_unlock(&self->lock);
        SignageVideoMessage release = { .type = SIGNAGE_VIDEO_RELEASED, .fb = 7, .serial = 1 };
        assert(send(sockets[1], &release, sizeof(release), 0) == sizeof(release));
        g_usleep(10000);
        g_mutex_lock(&self->lock); assert(g_hash_table_size(self->held) == 1); g_mutex_unlock(&self->lock);
        if (i % 2) {
            release.serial = 2;
            assert(send(sockets[1], &release, 2, 0) == 2);
            g_usleep(6000);
            assert(send(sockets[1], ((char *)&release) + 2, sizeof(release) - 2, 0) == sizeof(release) - 2);
            gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
            gboolean done = FALSE;
            while (g_get_monotonic_time() < deadline) {
                g_mutex_lock(&self->lock); done = g_hash_table_size(self->held) == 0; g_mutex_unlock(&self->lock);
                if (done) break;
                g_usleep(1000);
            }
            assert(done); /* stalled render/QoS: no render/expose call */
        }
        GThread *peer = g_thread_new("test-broker", broker_peer, GINT_TO_POINTER(sockets[1]));
        assert(admiral_plane_sink_stop(GST_BASE_SINK(self)));
        g_thread_join(peer);
        assert(self->receiver == NULL && g_hash_table_size(self->held) == 0);
        assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 1);
        gst_buffer_unref(buffer); close(sockets[1]); gst_object_unref(self);
    }
    return 0;
}
