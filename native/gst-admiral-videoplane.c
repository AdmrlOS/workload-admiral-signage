/* SPDX-License-Identifier: MIT
 * Video sink for the holepunch adapter that never commits to KMS itself.
 *
 * kmssink updates its plane with its own (legacy, blocking) commit. A CRTC
 * takes one commit per vblank, so a second committer on the browser's CRTC
 * halves the UI frame rate while video plays: the two alternate vblanks. This
 * sink imports each decoded DMA-BUF frame as a framebuffer on the shared DRM
 * descriptor and hands it to the browser over the broker connection; the
 * browser adds the overlay plane to its next atomic commit, together with the
 * UI frame when there is one, and reports when a framebuffer is free again.
 */
#include "gst-admiral-videoplane.h"
#include "drm-broker.h"

#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/videooverlay.h>
#include <gst/allocators/gstdmabuf.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

GST_DEBUG_CATEGORY_STATIC(admiral_plane_debug);
#define GST_CAT_DEFAULT admiral_plane_debug

/* Bounds framebuffers kept for decoder buffers that are not on screen. */
#define MAX_CACHED_FRAMEBUFFERS 32
/* current + pending + next in the browser, plus the frame being rendered. */
#define MIN_POOL_BUFFERS 4

typedef struct {
    GstBuffer *buffer;
    guint32 serial;
} HeldFrame;

typedef struct {
    GstVideoSink parent;
    GMutex lock;
    GCond receiver_wake;
    GThread *receiver;
    gboolean receiver_stop;
    gint fd, socket, mode_width, mode_height;
    GstVideoInfo info;
    gboolean have_info, disconnected;
    GstVideoRectangle render; /* CRTC coordinates; w == 0 means "whole mode" */
    GHashTable *framebuffers; /* "inode:offsets:strides" -> fb */
    GHashTable *held;         /* fb -> HeldFrame, until the browser releases it */
    GstBuffer *last;          /* re-shown when the rectangle changes */
    guint32 last_fb, serial;
    gboolean hidden;
    gboolean flushing;
    guint32 awaiting_fb, awaiting_serial;
    GHashTable *unpresented; /* serial -> fb, bounded two-frame pipeline */
    GHashTable *identities, *presented_pts; /* serial -> PTS; unique decoded PTS per seek */
    guint64 presented_total;
    guint8 incoming[sizeof(SignageVideoMessage)];
    gsize incoming_length;
    /* Per-second render diagnostics (SIGNAGE_VIDEO_STATS=1). */
    gboolean stats;
    gint64 stats_at, blocked_us, render_us;
    guint frames, max_held;
} AdmiralPlaneSink;

typedef struct { GstVideoSinkClass parent_class; } AdmiralPlaneSinkClass;

static void admiral_plane_sink_overlay_init(GstVideoOverlayInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AdmiralPlaneSink, admiral_plane_sink, GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_OVERLAY, admiral_plane_sink_overlay_init)
    GST_DEBUG_CATEGORY_INIT(admiral_plane_debug, "admiralplanesink", 0, "Admiral browser-committed video plane"))

/* The holepunch bin constrains hardware decoders to linear NV12 already. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:DMABuf), format=(string)DMA_DRM, drm-format=(string)NV12; "
                    "video/x-raw, format=(string)NV12"));

static void
held_free(gpointer data)
{
    HeldFrame *held = data;
    gst_buffer_unref(held->buffer);
    g_free(held);
}

static gboolean
send_message(AdmiralPlaneSink *self, const SignageVideoMessage *message)
{
    const guint8 *bytes = (const guint8 *)message;
    gsize sent = 0;
    while (sent < sizeof(*message)) {
        ssize_t part = send(self->socket, bytes + sent, sizeof(*message) - sent, MSG_NOSIGNAL);
        if (part < 0 && errno == EINTR) continue;
        if (part <= 0) {
            GST_WARNING_OBJECT(self, "Browser frame broker went away: %s", part < 0 ? g_strerror(errno) : "closed");
            self->disconnected = TRUE;
            return FALSE;
        }
        sent += (gsize)part;
    }
    return TRUE;
}

/* Reads RELEASED messages; waits up to timeout_ms for the first one. Lock held. */
static void
drain_releases(AdmiralPlaneSink *self, gint timeout_ms)
{
    if (self->socket < 0 || self->disconnected) return;
    struct pollfd poll_fd = { .fd = self->socket, .events = POLLIN };
    gint wait = timeout_ms;
    while (poll(&poll_fd, 1, wait) > 0) {
        wait = 0;
        ssize_t received = recv(self->socket, self->incoming + self->incoming_length,
                                sizeof(self->incoming) - self->incoming_length, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (received <= 0) {
            /* Peer went away; this is not proof scanout has retired. */
            GST_WARNING_OBJECT(self, "Browser frame broker closed the connection");
            self->disconnected = TRUE;
            /* EOF is not a scanout acknowledgement: browser hide may still be pending. */
            return;
        }
        self->incoming_length += (gsize)received;
        if (self->incoming_length < sizeof(self->incoming)) continue;
        SignageVideoMessage message;
        memcpy(&message, self->incoming, sizeof(message));
        self->incoming_length = 0;
        if (message.type == SIGNAGE_VIDEO_CANCELLED) {
            if (message.fb && GPOINTER_TO_UINT(g_hash_table_lookup(self->unpresented, GUINT_TO_POINTER(message.serial))) == message.fb) {
                g_hash_table_remove(self->unpresented, GUINT_TO_POINTER(message.serial));
                g_hash_table_remove(self->identities, GUINT_TO_POINTER(message.serial));
                g_cond_broadcast(&self->receiver_wake);
            }
            continue; /* NEVER release decoder storage on cancellation */
        }
        if (message.type == SIGNAGE_VIDEO_PRESENTED) {
            if (GPOINTER_TO_UINT(g_hash_table_lookup(self->unpresented, GUINT_TO_POINTER(message.serial))) == message.fb && message.fb) {
                g_hash_table_remove(self->unpresented, GUINT_TO_POINTER(message.serial));
                guint64 *pts = g_hash_table_lookup(self->identities, GUINT_TO_POINTER(message.serial));
                if (pts && *pts != GST_CLOCK_TIME_NONE) {
                    if (g_getenv("SIGNAGE_VIDEO_TRACE_PRESENTED"))
                        g_message("Video presented identity: serial=%u fb=%u pts=%" G_GUINT64_FORMAT,
                                  message.serial, message.fb, *pts);
                    /* Optional diagnostic, never an unbounded long-stream set.
                     * Exact acceptance uses the ordered external trace. */
                    if (g_getenv("SIGNAGE_VIDEO_TRACE_PRESENTED") && g_hash_table_size(self->presented_pts) < 4096) {
                        guint64 *copy = g_new(guint64, 1); *copy = *pts;
                        g_hash_table_add(self->presented_pts, copy);
                    }
                }
                g_hash_table_remove(self->identities, GUINT_TO_POINTER(message.serial));
                self->awaiting_fb = self->awaiting_serial = 0;
                self->presented_total++;
                g_cond_broadcast(&self->receiver_wake);
            }
            continue;
        }
        if (message.type != SIGNAGE_VIDEO_RELEASED) continue;
        HeldFrame *held = g_hash_table_lookup(self->held, GUINT_TO_POINTER(message.fb));
        /* An older release crossed a newer SHOW of the same framebuffer. */
        if (held && held->serial == message.serial)
            g_hash_table_remove(self->held, GUINT_TO_POINTER(message.fb));
    }
}

/* Duplicate descriptors are child-owned and remain open until finalize.
 * State and the byte-stream parser have one mutex; stop joins before teardown.
 * Unlike show_frame, this loop runs even when upstream QoS drops all frames. */
static gpointer release_receiver(gpointer data)
{
    AdmiralPlaneSink *self = data;
    g_mutex_lock(&self->lock);
    while (!self->receiver_stop) {
        drain_releases(self, 0);
        g_cond_wait_until(&self->receiver_wake, &self->lock,
                          g_get_monotonic_time() + 5000);
    }
    g_mutex_unlock(&self->lock);
    return NULL;
}

static void
remove_framebuffer(gpointer data, gpointer user_data)
{
    AdmiralPlaneSink *self = user_data;
    guint32 fb = GPOINTER_TO_UINT(data);
    if (self->fd >= 0 && fb) drmModeRmFB(self->fd, fb);
}

/* Drops cached framebuffers no longer on screen, queued or last. Lock held. */
static void
prune_framebuffers(AdmiralPlaneSink *self, gboolean all)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->framebuffers);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        guint32 fb = GPOINTER_TO_UINT(value);
        if (!all && (fb == self->last_fb || g_hash_table_contains(self->held, value))) continue;
        remove_framebuffer(value, self);
        g_hash_table_iter_remove(&iter);
    }
}

/* Returns the framebuffer for a linear NV12 DMA-BUF frame, creating it once per
 * decoder buffer. The decoder's pool reuses the same dma-bufs, so the cache
 * stays small; a dma-buf inode cannot be reused while our framebuffer holds it. */
static guint32
framebuffer_for(AdmiralPlaneSink *self, GstBuffer *buffer, GError **error)
{
    const GstVideoInfo *info = &self->info;
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    guint n_planes = GST_VIDEO_INFO_N_PLANES(info);
    guint32 handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
    gint fds[4] = { -1, -1, -1, -1 };
    GString *key = g_string_new(NULL);
    for (guint i = 0; i < n_planes && i < 4; i++) {
        gsize offset = meta ? meta->offset[i] : GST_VIDEO_INFO_PLANE_OFFSET(info, i);
        guint index, length;
        gsize skip;
        if (!gst_buffer_find_memory(buffer, offset, 1, &index, &length, &skip)) {
            g_set_error(error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT, "Plane %u offset %" G_GSIZE_FORMAT " is outside the buffer", i, offset);
            g_string_free(key, TRUE);
            return 0;
        }
        GstMemory *memory = gst_buffer_peek_memory(buffer, index);
        if (!gst_is_dmabuf_memory(memory)) {
            g_set_error_literal(error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
                                "The browser-committed video plane needs DMA-BUF frames (use a v4l2sl* hardware decoder)");
            g_string_free(key, TRUE);
            return 0;
        }
        fds[i] = gst_dmabuf_memory_get_fd(memory);
        offsets[i] = (guint32)(memory->offset + skip);
        pitches[i] = meta ? (guint32)meta->stride[i] : (guint32)GST_VIDEO_INFO_PLANE_STRIDE(info, i);
        struct stat st;
        if (fstat(fds[i], &st) != 0) {
            g_set_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED, "fstat on a decoder dma-buf failed: %s", g_strerror(errno));
            g_string_free(key, TRUE);
            return 0;
        }
        g_string_append_printf(key, "%lu:%u:%u;", (unsigned long)st.st_ino, offsets[i], pitches[i]);
    }
    g_string_append_printf(key, "%dx%d", GST_VIDEO_INFO_WIDTH(info), GST_VIDEO_INFO_HEIGHT(info));
    gpointer cached = g_hash_table_lookup(self->framebuffers, key->str);
    if (cached) {
        g_string_free(key, TRUE);
        return GPOINTER_TO_UINT(cached);
    }
    for (guint i = 0; i < n_planes && i < 4; i++) {
        if (drmPrimeFDToHandle(self->fd, fds[i], &handles[i]) != 0) {
            g_set_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED, "drmPrimeFDToHandle failed: %s", g_strerror(errno));
            for (guint j = 0; j < i; j++) {
                gboolean unique = TRUE;
                for (guint k = 0; k < j; k++) unique &= handles[k] != handles[j];
                if (unique) drmCloseBufferHandle(self->fd, handles[j]);
            }
            g_string_free(key, TRUE);
            return 0;
        }
    }
    guint32 fb = 0;
    int result = drmModeAddFB2(self->fd, GST_VIDEO_INFO_WIDTH(info), GST_VIDEO_INFO_HEIGHT(info), DRM_FORMAT_NV12,
                               handles, pitches, offsets, &fb, 0);
    int saved_errno = errno;
    /* The framebuffer holds the buffer object; the handles are not needed. The
     * descriptor (and GEM handle namespace) is shared with the browser, so
     * close each handle exactly once. */
    for (guint i = 0; i < n_planes && i < 4; i++) {
        gboolean unique = TRUE;
        for (guint j = 0; j < i; j++) unique &= handles[j] != handles[i];
        if (unique && handles[i]) drmCloseBufferHandle(self->fd, handles[i]);
    }
    if (result != 0) {
        g_set_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED, "drmModeAddFB2 for a %dx%d NV12 frame failed: %s",
                    GST_VIDEO_INFO_WIDTH(info), GST_VIDEO_INFO_HEIGHT(info), g_strerror(saved_errno));
        g_string_free(key, TRUE);
        return 0;
    }
    if (g_hash_table_size(self->framebuffers) >= MAX_CACHED_FRAMEBUFFERS) prune_framebuffers(self, FALSE);
    g_hash_table_insert(self->framebuffers, g_string_free(key, FALSE), GUINT_TO_POINTER(fb));
    GST_DEBUG_OBJECT(self, "Imported decoder buffer as fb %u (%u cached)", fb, g_hash_table_size(self->framebuffers));
    return fb;
}

/* Fits the video into the render rectangle keeping its aspect ratio (as
 * kmssink does), then clips it to the CRTC, cropping the source to match.
 * Returns FALSE when nothing is visible. Lock held. */
static gboolean
compute_geometry(AdmiralPlaneSink *self, GstBuffer *buffer, SignageVideoMessage *message)
{
    const GstVideoInfo *info = &self->info;
    gint src_x = 0, src_y = 0, src_w = GST_VIDEO_INFO_WIDTH(info), src_h = GST_VIDEO_INFO_HEIGHT(info);
    GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta(buffer);
    if (crop && crop->width && crop->height) {
        src_x = crop->x; src_y = crop->y; src_w = crop->width; src_h = crop->height;
    }
    GstVideoRectangle area = self->render;
    if (area.w <= 0 || area.h <= 0) area = (GstVideoRectangle){ 0, 0, self->mode_width, self->mode_height };
    gint par_n = MAX(1, GST_VIDEO_INFO_PAR_N(info)), par_d = MAX(1, GST_VIDEO_INFO_PAR_D(info));
    GstVideoRectangle source = { 0, 0, (gint)gst_util_uint64_scale_int(src_w, par_n, par_d), src_h }, result;
    gst_video_center_rect(&source, &area, &result, TRUE);
    gint64 x0 = MAX(result.x, 0), y0 = MAX(result.y, 0);
    gint64 x1 = MIN((gint64)result.x + result.w, self->mode_width), y1 = MIN((gint64)result.y + result.h, self->mode_height);
    if (result.w <= 0 || result.h <= 0 || x1 <= x0 || y1 <= y0) return FALSE;
    /* 16.16 source coordinates proportional to the visible part. */
    guint64 sx0 = ((guint64)src_x << 16) + (guint64)((x0 - result.x) * ((gint64)src_w << 16) / result.w);
    guint64 sx1 = ((guint64)src_x << 16) + (guint64)((x1 - result.x) * ((gint64)src_w << 16) / result.w);
    guint64 sy0 = ((guint64)src_y << 16) + (guint64)((y0 - result.y) * ((gint64)src_h << 16) / result.h);
    guint64 sy1 = ((guint64)src_y << 16) + (guint64)((y1 - result.y) * ((gint64)src_h << 16) / result.h);
    message->src_x = (guint32)sx0; message->src_y = (guint32)sy0;
    message->src_w = (guint32)(sx1 - sx0); message->src_h = (guint32)(sy1 - sy0);
    message->crtc_x = (gint32)x0; message->crtc_y = (gint32)y0;
    message->crtc_w = (guint32)(x1 - x0); message->crtc_h = (guint32)(y1 - y0);
    return message->src_w && message->src_h;
}

/* Two unpresented SHOW credits: backpressure the streaming thread, never the
 * browser/UI main loop. Buffer retirement remains a separate acknowledgement. */
static GstFlowReturn wait_credit(AdmiralPlaneSink *self, guint limit)
{
    gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (g_hash_table_size(self->unpresented) > limit && !self->disconnected && !self->flushing) {
        if (!g_cond_wait_until(&self->receiver_wake, &self->lock, deadline)) {
            GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("Video pageflip acknowledgement timed out"),
                              ("fb=%u serial=%u", self->awaiting_fb, self->awaiting_serial));
            return GST_FLOW_ERROR;
        }
    }
    return self->flushing ? GST_FLOW_FLUSHING : self->disconnected ? GST_FLOW_ERROR : GST_FLOW_OK;
}

/* Lock held. */
static GstFlowReturn
show_locked(AdmiralPlaneSink *self, GstBuffer *buffer)
{
    if (self->disconnected) {
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("The browser stopped accepting video frames"), (NULL));
        return GST_FLOW_ERROR;
    }
    drain_releases(self, 0);
    GError *error = NULL;
    GstFlowReturn credit = wait_credit(self, 1);
    if (credit != GST_FLOW_OK) return credit;
    guint32 fb = framebuffer_for(self, buffer, &error);
    if (!fb) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), (NULL));
        g_clear_error(&error);
        return GST_FLOW_ERROR;
    }
    if (buffer != self->last) gst_buffer_replace(&self->last, buffer);
    self->last_fb = fb;
    if (self->serial == G_MAXUINT32) {
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("Video identity exhausted; recreate pipeline"), (NULL));
        return GST_FLOW_ERROR;
    }
    SignageVideoMessage message = { .type = SIGNAGE_VIDEO_SHOW, .fb = fb, .serial = ++self->serial };
    if (!compute_geometry(self, buffer, &message)) {
        /* Entirely off screen: hide, but keep the frame for when it returns. */
        if (!self->hidden) {
            SignageVideoMessage hide = { .type = SIGNAGE_VIDEO_HIDE };
            if (!send_message(self, &hide)) return GST_FLOW_ERROR;
            self->hidden = TRUE;
        }
        return GST_FLOW_OK;
    }
    HeldFrame *held = g_hash_table_lookup(self->held, GUINT_TO_POINTER(fb));
    if (!held) {
        held = g_new0(HeldFrame, 1);
        held->buffer = gst_buffer_ref(buffer);
        g_hash_table_insert(self->held, GUINT_TO_POINTER(fb), held);
    } else if (held->buffer != buffer) {
        gst_buffer_replace(&held->buffer, buffer);
    }
    held->serial = message.serial;
    self->awaiting_fb = message.fb;
    self->awaiting_serial = message.serial;
    g_hash_table_insert(self->unpresented, GUINT_TO_POINTER(message.serial), GUINT_TO_POINTER(message.fb));
    guint64 *pts = g_new(guint64, 1); *pts = GST_BUFFER_PTS(buffer);
    if (message.serial <= 5) g_message("Video identity: serial=%u pts=%" G_GUINT64_FORMAT " duration=%" G_GUINT64_FORMAT, message.serial, *pts, GST_BUFFER_DURATION(buffer));
    g_hash_table_insert(self->identities, GUINT_TO_POINTER(message.serial), pts);
    if (!send_message(self, &message)) return GST_FLOW_ERROR;
    self->hidden = FALSE;
    /* The browser keeps at most three; more means it stopped presenting. */
    if (g_hash_table_size(self->held) > MIN_POOL_BUFFERS) {
        gint64 before = g_get_monotonic_time();
        drain_releases(self, 50);
        self->blocked_us += g_get_monotonic_time() - before;
    }
    return GST_FLOW_OK;
}

static GstFlowReturn
admiral_plane_sink_show_frame(GstVideoSink *video_sink, GstBuffer *buffer)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)video_sink;
    g_mutex_lock(&self->lock);
    gint64 start = g_get_monotonic_time();
    GstFlowReturn result = self->have_info && self->fd >= 0 && self->socket >= 0 ? show_locked(self, buffer) : GST_FLOW_NOT_NEGOTIATED;
    if (self->stats) {
        gint64 now = g_get_monotonic_time();
        self->render_us += now - start;
        self->frames++;
        self->max_held = MAX(self->max_held, g_hash_table_size(self->held));
        if (!self->stats_at) self->stats_at = now;
        if (now - self->stats_at >= G_USEC_PER_SEC) {
            guint64 rendered = 0, dropped = 0;
            GstStructure *st = NULL;
            g_object_get(self, "stats", &st, NULL);
            if (st) { gst_structure_get_uint64(st, "rendered", &rendered); gst_structure_get_uint64(st, "dropped", &dropped); gst_structure_free(st); }
            g_message("Video sink: %u frames in %.2f s, render avg %.2f ms, blocked on releases %.1f ms, max held %u, basesink rendered %" G_GUINT64_FORMAT " dropped %" G_GUINT64_FORMAT,
                      self->frames, (now - self->stats_at) / 1e6, self->render_us / 1000.0 / self->frames, self->blocked_us / 1000.0, self->max_held, rendered, dropped);
            self->stats_at = now; self->frames = 0; self->render_us = self->blocked_us = 0; self->max_held = 0;
        }
    }
    g_mutex_unlock(&self->lock);
    return result;
}

static GstFlowReturn wait_presented(AdmiralPlaneSink *self)
{
    return wait_credit(self, 0);
}

static gboolean admiral_plane_sink_query(GstElement *element, GstQuery *query)
{
    GstBaseSink *base = GST_BASE_SINK(element);
    gboolean live = FALSE, upstream_live = FALSE;
    GstClockTime minimum = 0, maximum = GST_CLOCK_TIME_NONE;
    gboolean latency_query = GST_QUERY_TYPE(query) == GST_QUERY_LATENCY;
    gboolean ok = latency_query
        ? gst_base_sink_query_latency(base, &live, &upstream_live, &minimum, &maximum)
        : GST_ELEMENT_CLASS(admiral_plane_sink_parent_class)->query(element, query);
    if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY && !ok) {
        /* Before preroll BaseSink rejects LATENCY and playbin sends zero.
         * We already know the renderer/processing budget from negotiated caps;
         * advertise it now so the first decoded frames get the same budget. */
        if (!gst_base_sink_get_sync(base)) {
            gst_query_set_latency(query, FALSE, 0, GST_CLOCK_TIME_NONE);
            return TRUE;
        }
        GstClockTime delay = gst_base_sink_get_render_delay(base);
        if (delay) {
            gst_query_set_latency(query, TRUE, delay + gst_base_sink_get_processing_deadline(base), GST_CLOCK_TIME_NONE);
            return TRUE;
        }
    }
    if (ok && GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
        /* BaseSink already accounts for processing when upstream is live.
         * Its returned live bit describes sink clock sync, not upstream. */
        GstClockTime processing = gst_base_sink_get_processing_deadline(base);
        if (live && !upstream_live) {
            minimum += processing;
            if (maximum != GST_CLOCK_TIME_NONE) maximum += processing;
        }
        gst_query_set_latency(query, live, minimum, maximum);
        g_message("Video latency query: live=%d min=%.3f ms max=%" G_GUINT64_FORMAT,
                  live, minimum / (double)GST_MSECOND, maximum);
    }
    return ok;
}

static gboolean admiral_plane_sink_event(GstBaseSink *base, GstEvent *event)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
        g_mutex_lock(&self->lock);
        GstFlowReturn result = wait_presented(self);
        g_message("Video EOS drain: unique_pts=%u presented=%" G_GUINT64_FORMAT " last_serial=%u pending=%u result=%s",
                  g_hash_table_size(self->presented_pts), self->presented_total, self->serial,
                  g_hash_table_size(self->unpresented), gst_flow_get_name(result));
        g_mutex_unlock(&self->lock);
        if (result != GST_FLOW_OK) { gst_event_unref(event); return FALSE; }
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
        g_mutex_lock(&self->lock);
        self->flushing = TRUE;
        SignageVideoMessage hide = { .type = SIGNAGE_VIDEO_HIDE };
        if (self->socket >= 0 && !self->disconnected) send_message(self, &hide);
        /* Serial is never reset: late outcomes cannot alias post-seek SHOWs.
         * Keep held references until independent RELEASED retirement. */
        g_hash_table_remove_all(self->unpresented);
        g_hash_table_remove_all(self->identities);
        g_cond_broadcast(&self->receiver_wake);
        g_mutex_unlock(&self->lock);
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        g_mutex_lock(&self->lock);
        g_hash_table_remove_all(self->presented_pts);
        g_mutex_unlock(&self->lock);
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_LATENCY) {
        GstClockTime latency; gst_event_parse_latency(event, &latency);
        g_message("Video negotiated latency: %.3f ms", latency / (double)GST_MSECOND);
    }
    return GST_BASE_SINK_CLASS(admiral_plane_sink_parent_class)->event(base, event);
}

static gboolean admiral_plane_sink_unlock(GstBaseSink *base)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    g_mutex_lock(&self->lock);
    self->flushing = TRUE;
    g_cond_broadcast(&self->receiver_wake);
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static gboolean admiral_plane_sink_unlock_stop(GstBaseSink *base)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    g_mutex_lock(&self->lock);
    self->flushing = FALSE;
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static gboolean
admiral_plane_sink_set_caps(GstBaseSink *base, GstCaps *caps)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    GstVideoInfo info;
    gboolean ok;
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drm;
        ok = gst_video_info_dma_drm_from_caps(&drm, caps) && drm.drm_fourcc == DRM_FORMAT_NV12 &&
             drm.drm_modifier == DRM_FORMAT_MOD_LINEAR && gst_video_info_dma_drm_to_video_info(&drm, &info);
    } else {
        ok = gst_video_info_from_caps(&info, caps) && GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_NV12;
    }
    if (!ok) {
        GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Unsupported video caps for the overlay plane"), ("%" GST_PTR_FORMAT, caps));
        return FALSE;
    }
    g_mutex_lock(&self->lock);
    self->info = info;
    self->have_info = TRUE;
    g_mutex_unlock(&self->lock);
    GST_VIDEO_SINK_WIDTH(self) = GST_VIDEO_INFO_WIDTH(&info);
    GST_VIDEO_SINK_HEIGHT(self) = GST_VIDEO_INFO_HEIGHT(&info);
    /* Measured message->commit (~16ms) + flip (~13ms) at 60Hz. The
     * two-credit atomic pipeline can take two source periods to latch.
     * Report that real render delay through LATENCY, not a PTS offset. */
    if (GST_VIDEO_INFO_FPS_N(&info) > 0) {
        GstClockTime pipeline_budget = gst_util_uint64_scale(2 * GST_SECOND,
            GST_VIDEO_INFO_FPS_D(&info), GST_VIDEO_INFO_FPS_N(&info));
        gst_base_sink_set_render_delay(base, pipeline_budget);
        /* current decode + two queued SHOW credits require three periods of
         * processing budget; render delay remains two vblank periods. */
        gst_base_sink_set_processing_deadline(base, gst_util_uint64_scale(3 * GST_SECOND,
            GST_VIDEO_INFO_FPS_D(&info), GST_VIDEO_INFO_FPS_N(&info)));
    } else {
        /* Renegotiation to unknown rate must not inherit the previous budget. */
        gst_base_sink_set_render_delay(base, 0);
        gst_base_sink_set_processing_deadline(base, 0);
    }
    return TRUE;
}

static gboolean
admiral_plane_sink_propose_allocation(GstBaseSink *base, GstQuery *query)
{
    (void)base;
    GstCaps *caps = NULL;
    gboolean need_pool = FALSE;
    gst_query_parse_allocation(query, &caps, &need_pool);
    GstVideoInfo info;
    gsize size = 0;
    if (caps && !gst_video_is_dma_drm_caps(caps) && gst_video_info_from_caps(&info, caps)) size = GST_VIDEO_INFO_SIZE(&info);
    gst_query_add_allocation_pool(query, NULL, (guint)size, MIN_POOL_BUFFERS, 0);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, NULL);
    return TRUE;
}

static gboolean
admiral_plane_sink_start(GstBaseSink *base)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    g_mutex_lock(&self->lock);
    gboolean ok = self->fd >= 0 && self->socket >= 0 && self->mode_width > 0 && self->mode_height > 0;
    self->disconnected = FALSE;
    self->flushing = FALSE;
    self->awaiting_fb = self->awaiting_serial = 0;
    self->presented_total = 0;
    g_hash_table_remove_all(self->unpresented);
    g_hash_table_remove_all(self->identities);
    g_hash_table_remove_all(self->presented_pts);
    self->hidden = TRUE;
    if (ok && !self->receiver) {
        self->receiver_stop = FALSE;
        self->receiver = g_thread_new("video-releases", release_receiver, self);
    }
    g_mutex_unlock(&self->lock);
    if (!ok) GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, ("No brokered DRM descriptor for the browser-committed video plane"), (NULL));
    return ok;
}

/* Hide, wait until the browser no longer scans out our framebuffers, then free them. */
static gboolean
admiral_plane_sink_stop(GstBaseSink *base)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    g_mutex_lock(&self->lock);
    if (!self->receiver && !self->have_info && !g_hash_table_size(self->held)) {
        g_mutex_unlock(&self->lock);
        return TRUE;
    }
    self->receiver_stop = TRUE;
    g_cond_signal(&self->receiver_wake);
    GThread *receiver = self->receiver;
    self->receiver = NULL;
    g_mutex_unlock(&self->lock);
    if (receiver) g_thread_join(receiver);
    g_mutex_lock(&self->lock);
    if (self->socket >= 0 && !self->disconnected) {
        SignageVideoMessage hide = { .type = SIGNAGE_VIDEO_HIDE };
        if (send_message(self, &hide)) {
            gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
            while (g_hash_table_size(self->held) && !self->disconnected && g_get_monotonic_time() < deadline)
                drain_releases(self, 50);
        }
        if (g_hash_table_size(self->held))
            GST_WARNING_OBJECT(self, "%u video framebuffers not released by the browser in time", g_hash_table_size(self->held));
    }
    gst_buffer_replace(&self->last, NULL);
    self->last_fb = 0;
    if (g_hash_table_size(self->held)) {
        /* EOF/timeouts are not proof of KMS retirement. Do not drop the only
         * owner or permit pool reuse. Terminate the WebKit process so its
         * supervisor recreates the connection/pipeline instead of accumulating
         * unreachable quarantines across every failed scene transition. */
        g_error("Unretired video buffers: terminating web process for safe recovery");
    } else prune_framebuffers(self, TRUE);
    self->have_info = FALSE;
    self->incoming_length = 0;
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static void
admiral_plane_sink_set_render_rectangle(GstVideoOverlay *overlay, gint x, gint y, gint width, gint height)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)overlay;
    g_mutex_lock(&self->lock);
    /* WebKit sets the rectangle on every UI frame; only a change matters. */
    if (self->render.x != x || self->render.y != y || self->render.w != width || self->render.h != height) {
        self->render = (GstVideoRectangle){ x, y, width, height };
        GST_DEBUG_OBJECT(self, "Render rectangle %d,%d %dx%d", x, y, width, height);
        if (self->last && self->have_info && self->socket >= 0 && !self->disconnected) show_locked(self, self->last);
    }
    g_mutex_unlock(&self->lock);
}

static void
admiral_plane_sink_expose(GstVideoOverlay *overlay)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)overlay;
    g_mutex_lock(&self->lock);
    if (self->last && self->have_info && self->socket >= 0 && !self->disconnected) show_locked(self, self->last);
    g_mutex_unlock(&self->lock);
}

static void
admiral_plane_sink_set_window_handle(GstVideoOverlay *overlay, guintptr handle)
{
    (void)overlay; (void)handle; /* the plane belongs to the browser's CRTC */
}

static void
admiral_plane_sink_overlay_init(GstVideoOverlayInterface *iface)
{
    iface->set_render_rectangle = admiral_plane_sink_set_render_rectangle;
    iface->expose = admiral_plane_sink_expose;
    iface->set_window_handle = admiral_plane_sink_set_window_handle;
}

void
admiral_plane_sink_configure(GstElement *element, gint drm_fd, gint socket_fd, gint mode_width, gint mode_height)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)element;
    g_mutex_lock(&self->lock);
    /* Child owns duplicate descriptors, so parent teardown cannot race recv or
     * let a reused descriptor number refer to an unrelated connection. */
    if (self->fd >= 0) close(self->fd);
    if (self->socket >= 0) close(self->socket);
    self->fd = dup(drm_fd);
    self->socket = dup(socket_fd);
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 };
    if (self->socket >= 0) setsockopt(self->socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    self->mode_width = mode_width;
    self->mode_height = mode_height;
    g_mutex_unlock(&self->lock);
}

static void
admiral_plane_sink_finalize(GObject *object)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)object;
    /* Normally stop already joined; also cover failed/partial state transitions. */
    admiral_plane_sink_stop(GST_BASE_SINK(self));
    if (self->socket >= 0) close(self->socket);
    if (self->fd >= 0) close(self->fd);
    g_hash_table_unref(self->held);
    g_hash_table_unref(self->unpresented);
    g_hash_table_unref(self->identities);
    g_hash_table_unref(self->presented_pts);
    g_hash_table_unref(self->framebuffers);
    gst_buffer_replace(&self->last, NULL);
    g_cond_clear(&self->receiver_wake);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(admiral_plane_sink_parent_class)->finalize(object);
}

static void
admiral_plane_sink_class_init(AdmiralPlaneSinkClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = admiral_plane_sink_finalize;
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Admiral browser-committed video plane", "Sink/Video",
        "Hands DMA-BUF video frames to the signage browser, which commits them with the UI", "Admiral");
    gst_element_class_add_static_pad_template(element, &sink_template);
    element->query = admiral_plane_sink_query;
    GstBaseSinkClass *base = GST_BASE_SINK_CLASS(klass);
    base->set_caps = admiral_plane_sink_set_caps;
    base->propose_allocation = admiral_plane_sink_propose_allocation;
    base->start = admiral_plane_sink_start;
    base->stop = admiral_plane_sink_stop;
    base->event = admiral_plane_sink_event;

    base->unlock = admiral_plane_sink_unlock;
    base->unlock_stop = admiral_plane_sink_unlock_stop;
    GST_VIDEO_SINK_CLASS(klass)->show_frame = admiral_plane_sink_show_frame;
}

static void
admiral_plane_sink_init(AdmiralPlaneSink *self)
{
    g_mutex_init(&self->lock);
    g_cond_init(&self->receiver_wake);
    self->fd = self->socket = -1;
    self->hidden = TRUE;
    /* Initial budget before CAPS/preroll, refined from source rate in set_caps.
     * Prevent playbin's first pre-preroll LATENCY query from selecting zero. */
    gst_base_sink_set_render_delay(GST_BASE_SINK(self), GST_SECOND / 30);
    gst_base_sink_set_processing_deadline(GST_BASE_SINK(self), GST_SECOND / 30);
    self->framebuffers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->held = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, held_free);
    self->stats = g_strcmp0(g_getenv("SIGNAGE_VIDEO_STATS"), "1") == 0;
    self->unpresented = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->identities = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    self->presented_pts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    /* Experiment knobs: SIGNAGE_VIDEO_QOS=0 disables QoS events upstream; SIGNAGE_VIDEO_MAX_LATENESS_MS. */
    if (g_strcmp0(g_getenv("SIGNAGE_VIDEO_QOS"), "0") == 0) gst_base_sink_set_qos_enabled(GST_BASE_SINK(self), FALSE);
    const gchar *lateness = g_getenv("SIGNAGE_VIDEO_MAX_LATENESS_MS");
    if (lateness && *lateness) gst_base_sink_set_max_lateness(GST_BASE_SINK(self), g_ascii_strtoll(lateness, NULL, 10) * GST_MSECOND);
}
