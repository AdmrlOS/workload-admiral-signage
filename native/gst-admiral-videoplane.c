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
#include "video-ledger.h"
#include <stdio.h>

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
    gboolean have_info, disconnected, standby, prepared, activated_preroll;
    guint64 generation;
    guint64 last_interval_us;
    /* Geometry-only refreshes (rectangle change, expose) of a stream that is delivering frames
     * are folded into the next frame's SHOW instead of spending a display slot of their own:
     * the plane takes one commit per vblank, so a separate re-SHOW at a source rate equal to
     * the refresh rate delays every following frame by a whole vblank and the lateness never
     * recovers (measured: p50 +76 ms, 454 late-QoS events, 7 frames dropped per 10 s with a
     * rectangle change every 400 ms). An idle stream (paused/still frame) is refreshed by the
     * receiver thread once no frame has arrived for the window. defer_us < 0: derive from the
     * frame interval; 0: always refresh immediately. */
    gint64 last_frame_us, defer_us;
    gboolean geometry_pending;
    GstVideoRectangle render; /* CRTC coordinates; w == 0 means "whole mode" */
    GHashTable *framebuffers; /* "inode:offsets:strides" -> fb */
    GHashTable *held;         /* fb -> HeldFrame, until the browser releases it */
    GstBuffer *last;          /* re-shown when the rectangle changes */
    gboolean last_presented;  /* PRESENTED was received for `last` */
    gboolean last_identity_sent; /* a SHOW carrying `last`'s PTS identity was sent: every later re-show is geometry only */
    guint32 last_fb, serial;
    guint32 last_presented_serial;
    gboolean presentation_failed; /* cancellation is not successful EOS */
    gboolean hidden;
    gboolean flushing;
    guint32 awaiting_fb, awaiting_serial;
    GHashTable *unpresented; /* serial -> fb, bounded two-frame pipeline */
    GHashTable *identities, *presented_pts; /* serial -> PTS; unique decoded PTS per seek */
    guint64 presented_total;
    /* Persistent video engine client: report PRESENTED identities on the bus, keep the
     * last frame through flushes, never re-SHOW an already submitted buffer, and support
     * explicit invisibility (warm-up/hidden) without holding decoder buffers. */
    gboolean engine, invisible, restore_on_visible;
    GArray *notify; /* PresentedNote, posted outside the lock by the receiver */
    guint8 incoming[sizeof(SignageVideoMessage)];
    gsize incoming_length;
    /* Per-second render diagnostics (SIGNAGE_VIDEO_STATS=1). */
    gboolean stats;
    gint64 stats_at, blocked_us, render_us;
    guint frames, max_held;
    VideoLedger *ledger;
    GMutex ledger_lock;
} AdmiralPlaneSink;

typedef struct { GstVideoSinkClass parent_class; } AdmiralPlaneSinkClass;
typedef struct { guint64 pts; guint32 serial, fb; } PresentedNote;

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
    SignageVideoMessage owned = *message;
    owned.generation = self->generation;
    const guint8 *bytes = (const guint8 *)&owned;
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

/* Engine sinks: remember a successful PRESENTED identity (lock held). */
static void
note_presented(AdmiralPlaneSink *self, guint64 pts, guint32 serial, guint32 fb)
{
    if (!self->engine || !self->notify || self->notify->len >= 256) return;
    PresentedNote note = { pts, serial, fb };
    g_array_append_val(self->notify, note);
}

/* Post remembered identities as element messages without holding the lock (bus sync
 * handlers may call back into the sink). Called with the lock held; returns with it. */
static void
post_presented_unlocked(AdmiralPlaneSink *self)
{
    if (!self->engine || !self->notify || !self->notify->len) return;
    GArray *notes = self->notify;
    self->notify = g_array_new(FALSE, FALSE, sizeof(PresentedNote));
    guint64 total = self->presented_total;
    g_mutex_unlock(&self->lock);
    for (guint i = 0; i < notes->len; i++) {
        const PresentedNote *n = &g_array_index(notes, PresentedNote, i);
        GstStructure *s = gst_structure_new("admiral-presented", "pts", G_TYPE_UINT64, n->pts,
            "serial", G_TYPE_UINT, n->serial, "fb", G_TYPE_UINT, n->fb, "presented_total", G_TYPE_UINT64, total, NULL);
        gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self), s));
    }
    g_array_unref(notes);
    g_mutex_lock(&self->lock);
}

static gboolean stream_is_delivering(AdmiralPlaneSink *self, gint64 now);
static GstFlowReturn reshow_last_locked(AdmiralPlaneSink *self);

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
        if (message.generation != self->generation) continue;
        if (message.type == SIGNAGE_VIDEO_PREPARED && self->standby && message.fb == self->last_fb &&
            message.serial == self->serial) {
            self->prepared = TRUE; g_cond_broadcast(&self->receiver_wake); continue;
        }
        if (message.type == SIGNAGE_VIDEO_ACTIVATED && self->standby && message.fb == self->last_fb &&
            message.serial == self->serial && self->last_presented_serial == message.serial) {
            self->standby = FALSE; self->activated_preroll = TRUE; g_cond_broadcast(&self->receiver_wake); continue;
        }
        if (message.type == SIGNAGE_VIDEO_CANCELLED) {
            if (message.fb && GPOINTER_TO_UINT(g_hash_table_lookup(self->unpresented, GUINT_TO_POINTER(message.serial))) == message.fb) {
                self->presentation_failed = TRUE;
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
                if (self->ledger) {
                    g_mutex_lock(&self->ledger_lock);
                    video_ledger_record(self->ledger, g_get_monotonic_time(),
                        pts ? *pts : GST_CLOCK_TIME_NONE, 0, 3, message.serial, message.fb);
                    g_mutex_unlock(&self->ledger_lock);
                }
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
                if (pts && *pts != GST_CLOCK_TIME_NONE) note_presented(self, *pts, message.serial, message.fb); /* before pts is freed */
                if (message.serial == self->serial) self->last_presented = TRUE;
                g_hash_table_remove(self->identities, GUINT_TO_POINTER(message.serial));
                self->awaiting_fb = self->awaiting_serial = 0;
                self->last_presented_serial = message.serial;
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
        post_presented_unlocked(self);
        if (self->receiver_stop) break;
        /* Idle stream with a deferred geometry change: refresh it now. A credit is free, so
         * the re-SHOW cannot block this thread (it is the one that drains acknowledgements). */
        if (self->geometry_pending && !stream_is_delivering(self, g_get_monotonic_time())) {
            self->geometry_pending = FALSE;
            if (self->last && self->have_info && self->socket >= 0 && !self->disconnected && !self->flushing) {
                if (g_hash_table_size(self->unpresented) <= 1) reshow_last_locked(self);
                else self->geometry_pending = TRUE;
            }
        }
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
show_locked(AdmiralPlaneSink *self, GstBuffer *buffer, gboolean reshow_call);

/* Geometry/visibility refresh of the frame currently on screen (rectangle change, expose,
 * visibility restore). wait_credit() drops the lock, so the streaming thread can SHOW a newer
 * frame meanwhile: show_locked() then abandons this stale refresh instead of putting the older
 * frame back on screen. The caller's `last` pointer may also be replaced during that wait, so
 * the refresh owns a reference. Lock held. */
static GstFlowReturn
reshow_last_locked(AdmiralPlaneSink *self)
{
    GstBuffer *buffer = gst_buffer_ref(self->last);
    GstFlowReturn result = show_locked(self, buffer, TRUE);
    gst_buffer_unref(buffer);
    return result;
}

/* True while real frames are arriving, so a geometry refresh can ride on the next one. */
static gboolean
stream_is_delivering(AdmiralPlaneSink *self, gint64 now)
{
    gint64 window = self->defer_us;
    if (window < 0) window = MAX((gint64)(self->last_interval_us ? self->last_interval_us : 16667) * 5 / 2, 20000);
    return self->engine && window > 0 && self->last_frame_us && now - self->last_frame_us < window;
}

/* Rectangle change / expose of the frame on screen. Lock held. */
static GstFlowReturn
refresh_geometry_locked(AdmiralPlaneSink *self)
{
    if (stream_is_delivering(self, g_get_monotonic_time())) { self->geometry_pending = TRUE; return GST_FLOW_OK; }
    self->geometry_pending = FALSE;
    return reshow_last_locked(self);
}

static GstFlowReturn
show_locked(AdmiralPlaneSink *self, GstBuffer *buffer, gboolean reshow_call)
{
    admiral_plane_sink_record(GST_ELEMENT(self), 1, GST_BUFFER_PTS(buffer), g_hash_table_size(self->unpresented));
    if (self->disconnected) {
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("The browser stopped accepting video frames"), (NULL));
        return GST_FLOW_ERROR;
    }
    drain_releases(self, 0);
    GError *error = NULL;
    /* Invisible engine sink (startup warm-up or hidden component): consume the frame
     * without a SHOW, without holding the decoder buffer and without making it `last`. */
    if (self->engine && self->invisible) return GST_FLOW_OK;
    /* A PAUSED preroll sends exactly one retained frame. Never spend a second
     * credit, advance decoder playback or re-SHOW it before native activation. */
    if (self->standby && self->serial) {
        if (buffer == self->last) return GST_FLOW_OK;
        gint64 deadline=g_get_monotonic_time()+G_USEC_PER_SEC;
        while (self->standby && !self->disconnected && !self->flushing)
            if (!g_cond_wait_until(&self->receiver_wake,&self->lock,deadline)) return GST_FLOW_ERROR;
        if (self->flushing) return GST_FLOW_FLUSHING;
        if (self->disconnected) return GST_FLOW_ERROR;
    }
    if (self->activated_preroll) {
        self->activated_preroll=FALSE;
        if (buffer==self->last) return GST_FLOW_OK; /* already PRESENTED at activation */
    }
    GstFlowReturn credit = wait_credit(self, 1);
    if (credit != GST_FLOW_OK) return credit;
    if (reshow_call && buffer != self->last) return GST_FLOW_OK; /* a newer frame won the race */
    guint32 fb = framebuffer_for(self, buffer, &error);
    if (!fb) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), (NULL));
        g_clear_error(&error);
        return GST_FLOW_ERROR;
    }
    /* Re-SHOWing the frame already on screen (rectangle change, expose, visibility restore)
     * moves/refreshes the plane; it must not report a second PRESENTED identity for that PTS. */
    gboolean reshow = self->engine && buffer == self->last && self->last_identity_sent;
    if (buffer != self->last) { gst_buffer_replace(&self->last, buffer); self->last_presented = FALSE; self->last_identity_sent = FALSE; }
    self->last_fb = fb;
    GstClockTime interval = GST_BUFFER_DURATION(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(interval) || !interval) interval = GST_VIDEO_INFO_FPS_N(&self->info) > 0 ?
        gst_util_uint64_scale(GST_SECOND, GST_VIDEO_INFO_FPS_D(&self->info), GST_VIDEO_INFO_FPS_N(&self->info)) : 0;
    self->last_interval_us = interval ? (interval + GST_USECOND - 1) / GST_USECOND : 0;
    if (self->serial == G_MAXUINT32) {
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("Video identity exhausted; recreate pipeline"), (NULL));
        return GST_FLOW_ERROR;
    }
    SignageVideoMessage message = { .type = SIGNAGE_VIDEO_SHOW, .fb = fb, .serial = self->serial + 1 };
    if (!compute_geometry(self, buffer, &message)) {
        g_message("Native geometry empty generation=%" G_GUINT64_FORMAT " rect=%d,%d,%d,%d mode=%d,%d",self->generation,self->render.x,self->render.y,self->render.w,self->render.h,self->mode_width,self->mode_height);
        /* Entirely off screen: hide, but keep the frame for when it returns. */
        if (!self->hidden) {
            SignageVideoMessage hide = { .type = SIGNAGE_VIDEO_HIDE };
            if (!send_message(self, &hide)) return GST_FLOW_ERROR;
            self->hidden = TRUE;
        }
        return GST_FLOW_OK;
    }
    self->serial = message.serial;
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
    guint64 *pts = g_new(guint64, 1); *pts = reshow ? GST_CLOCK_TIME_NONE : GST_BUFFER_PTS(buffer);
    if (message.serial <= 5) g_message("Video identity: serial=%u pts=%" G_GUINT64_FORMAT " duration=%" G_GUINT64_FORMAT, message.serial, *pts, GST_BUFFER_DURATION(buffer));
    g_hash_table_insert(self->identities, GUINT_TO_POINTER(message.serial), pts);
    if (!send_message(self, &message)) return GST_FLOW_ERROR;
    if (!reshow) self->last_identity_sent = TRUE;
    if (!reshow_call) { self->last_frame_us = g_get_monotonic_time(); self->geometry_pending = FALSE; }
    if (self->ledger) {
        g_mutex_lock(&self->ledger_lock);
        video_ledger_record(self->ledger, g_get_monotonic_time(),
            GST_BUFFER_PTS(buffer), g_hash_table_size(self->held), 2, message.serial, message.fb);
        g_mutex_unlock(&self->ledger_lock);
    }
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
    /* BaseSink renders the prerolled buffer again on PAUSED->PLAYING. For the engine that
     * frame was already submitted (seek/pause preview); re-SHOWing it would duplicate its
     * PRESENTED identity. `last` holds a reference, so the pointer cannot be a recycled
     * pool buffer carrying a different frame. */
    if (self->engine && buffer == self->last && self->serial && !self->hidden) {
        g_mutex_unlock(&self->lock);
        return GST_FLOW_OK;
    }
    gint64 start = g_get_monotonic_time();
    GstFlowReturn result = self->have_info && self->fd >= 0 && self->socket >= 0 ? show_locked(self, buffer, FALSE) : GST_FLOW_NOT_NEGOTIATED;
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

/* GstVideoSink's default preroll may be suppressed by WebKit's show-preroll
 * policy. Standby preparation is an explicit retained DMA-BUF submission,
 * not a visible render; route it independently of that presentation policy. */
static GstFlowReturn admiral_plane_sink_preroll(GstBaseSink *base, GstBuffer *buffer)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)base;
    g_mutex_lock(&self->lock);
    gboolean standby = self->standby;
    g_mutex_unlock(&self->lock);
    g_message("Native preroll generation=%" G_GUINT64_FORMAT " standby=%d pts=%" G_GUINT64_FORMAT,self->generation,standby,GST_BUFFER_PTS(buffer));
    if (standby) return admiral_plane_sink_show_frame(GST_VIDEO_SINK(base), buffer);
    return GST_BASE_SINK_CLASS(admiral_plane_sink_parent_class)->preroll(base, buffer);
}

static GstFlowReturn wait_presented(AdmiralPlaneSink *self)
{
    GstFlowReturn result = wait_credit(self, 0);
    if (result == GST_FLOW_OK && (!self->serial || self->presentation_failed ||
                                 self->last_presented_serial != self->serial)) {
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE,
            ("Video EOS endpoint was not successfully presented"),
            ("final=%u presented=%u cancelled=%d", self->serial,
             self->last_presented_serial, self->presentation_failed));
        return GST_FLOW_ERROR;
    }
    return result;
}

/* Caller holds lock; only EOS/stop performs I/O, not presentation callbacks. */
static void dump_ledger(AdmiralPlaneSink *self)
{
    if (!self->ledger || !self->ledger->total) return;
    g_mutex_lock(&self->ledger_lock);
    const gchar *dir = g_getenv("SIGNAGE_VIDEO_LEDGER_DIR");
    gchar *path = g_strdup_printf("%s/plane-%ld-%p-g%" G_GUINT64_FORMAT "-%u.csv", dir ? dir : "/run", (long)getpid(), (void *)self, self->generation, self->serial);
    FILE *stream = fopen(path, "w");
    if (stream) {
        fprintf(stream, "# generation=%" G_GUINT64_FORMAT " total=%" G_GUINT64_FORMAT " capacity=%u\nmono_us,pts,value,stage,serial,fb\n", self->generation, self->ledger->total, VIDEO_LEDGER_CAPACITY);
        for (guint64 i = 0; ; i++) {
            const VideoLedgerRecord *r = video_ledger_at(self->ledger, i);
            if (!r) break;
            fprintf(stream, "%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",%u,%u,%u\n",
                    r->mono_us, r->pts, r->value, r->stage, r->serial, r->fb);
        }
        fclose(stream);
    }
    g_free(path);
    self->ledger->total = 0;
    g_mutex_unlock(&self->ledger_lock);
}

void admiral_plane_sink_record(GstElement *element, guint stage, guint64 pts, guint64 value)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)element;
    if (!self->ledger) return;
    g_mutex_lock(&self->ledger_lock);
    video_ledger_record(self->ledger, g_get_monotonic_time(), pts, value, stage, 0, 0);
    g_mutex_unlock(&self->ledger_lock);
}

static GstPadProbeReturn ledger_upstream(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event && GST_EVENT_TYPE(event) == GST_EVENT_QOS) {
        GstQOSType type; gdouble proportion; GstClockTimeDiff diff; GstClockTime timestamp;
        gst_event_parse_qos(event, &type, &proportion, &diff, &timestamp);
        (void)type; (void)proportion;
        admiral_plane_sink_record(data, 6, timestamp, (guint64)diff);
    }
    return GST_PAD_PROBE_OK;
}

/* FLUSH_START bookkeeping (lock held). Serial is never reset: late outcomes cannot alias
 * post-seek SHOWs. Held references stay until independent RELEASED retirement.
 *
 * An engine sink keeps its SHOW credits (`unpresented`) across the flush: the browser still
 * owns those frames (one in flight, one queued) until it sends their PRESENTED/CANCELLED. The
 * first post-flush SHOW would otherwise be a third outstanding frame and overflow the
 * browser's two-slot FIFO, which disconnects the sink (observed after 48 min: engine abort
 * -5 at a clip cut). The post-flush streaming thread simply waits for the browser's
 * acknowledgements in wait_credit(), which is the intended backpressure. An HTML sink sends
 * HIDE instead, which makes the browser cancel (and acknowledge) everything synchronously, so
 * it may forget its credits. Only identities (PTS of the flushed timeline) are dropped. */
static void
flush_start_locked(AdmiralPlaneSink *self)
{
    self->flushing = TRUE;
    if (!self->engine) {
        SignageVideoMessage hide = { .type = SIGNAGE_VIDEO_HIDE };
        /* The engine flushes for seeks and immediate switches inside one presentation:
         * keep the last frame on the plane until the next SHOW replaces it. */
        if (self->socket >= 0 && !self->disconnected) send_message(self, &hide);
        g_hash_table_remove_all(self->unpresented);
    }
    g_hash_table_remove_all(self->identities);
    g_cond_broadcast(&self->receiver_wake);
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
        if (result == GST_FLOW_OK) {
            SignageVideoMessage endpoint = { .type = SIGNAGE_VIDEO_ENDPOINT,
                .fb = self->last_fb, .serial = self->last_presented_serial, .interval_us = self->last_interval_us };
            if (!endpoint.interval_us || !send_message(self, &endpoint)) result = GST_FLOW_ERROR;
        }
        dump_ledger(self);
        g_message("Video EOS drain: unique_pts=%u presented=%" G_GUINT64_FORMAT " last_serial=%u pending=%u result=%s",
                  g_hash_table_size(self->presented_pts), self->presented_total, self->serial,
                  g_hash_table_size(self->unpresented), gst_flow_get_name(result));
        g_mutex_unlock(&self->lock);
        if (result != GST_FLOW_OK) { gst_event_unref(event); return FALSE; }
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
        g_mutex_lock(&self->lock);
        flush_start_locked(self);
        g_mutex_unlock(&self->lock);
    }
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        g_mutex_lock(&self->lock);
        self->presentation_failed = FALSE;
        self->last_presented_serial = 0;
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
    guint min_buffers = MIN_POOL_BUFFERS;
    if (size && caps) {
        /* Decoder lead is bounded by its capture pool, not by the queue: at 24 fps
         * four buffers are only ~170 ms, so a ~250-400 ms decoder hiccup starves
         * the sink and cascades into sink/decoder QoS drops. Ask for ~SIGNAGE_
         * DECODE_LEAD_MS of frames (default 700), capped by SIGNAGE_DECODE_LEAD_MB
         * (default 48 MB) so 4K keeps the old four-buffer pool. */
        guint64 lead_ms = 700, cap_mb = 48;
        const gchar *e = g_getenv("SIGNAGE_DECODE_LEAD_MS");
        if (e && *e) lead_ms = g_ascii_strtoull(e, NULL, 10);
        e = g_getenv("SIGNAGE_DECODE_LEAD_MB");
        if (e && *e) cap_mb = g_ascii_strtoull(e, NULL, 10);
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        gint fn = 0, fd = 1;
        if (lead_ms && s && gst_structure_get_fraction(s, "framerate", &fn, &fd) && fn > 0 && fd > 0) {
            guint64 want = (lead_ms * (guint64)fn + 1000ULL * fd - 1) / (1000ULL * fd);
            guint64 cap = (cap_mb * 1000 * 1000) / size;
            if (want > cap) want = cap;
            if (want > 24) want = 24;
            if (want > min_buffers) min_buffers = (guint)want;
        }
    }
    gst_query_add_allocation_pool(query, NULL, (guint)size, min_buffers, 0);
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
    self->presentation_failed = FALSE;
    self->last_presented_serial = 0;
    self->awaiting_fb = self->awaiting_serial = 0;
    self->presented_total = 0;
    g_hash_table_remove_all(self->unpresented);
    g_hash_table_remove_all(self->identities);
    g_hash_table_remove_all(self->presented_pts);
    self->hidden = TRUE;
    self->geometry_pending = FALSE;
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
    dump_ledger(self);
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
        if (self->last && self->have_info && self->socket >= 0 && !self->disconnected) refresh_geometry_locked(self);
    }
    g_mutex_unlock(&self->lock);
}

static void
admiral_plane_sink_expose(GstVideoOverlay *overlay)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)overlay;
    g_mutex_lock(&self->lock);
    if (self->last && self->have_info && self->socket >= 0 && !self->disconnected) refresh_geometry_locked(self);
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

void admiral_plane_sink_set_generation(GstElement *element, guint64 generation, gboolean standby)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)element;
    g_mutex_lock(&self->lock);
    self->generation = generation; self->standby = standby; self->prepared = FALSE;
    g_mutex_unlock(&self->lock);
}

void admiral_plane_sink_set_engine(GstElement *element, gboolean engine)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)element;
    g_mutex_lock(&self->lock);
    self->engine = engine;
    if (engine && !self->notify) self->notify = g_array_new(FALSE, FALSE, sizeof(PresentedNote));
    g_mutex_unlock(&self->lock);
    /* QoS and BaseSink's lateness policy stay at their defaults for the engine sink: a frame
     * that really reaches the sink late must be reported upstream (and counted), never hidden.
     * Frame loss on a rectangle change / commit stall is fixed at its cause (credit pacing),
     * not by switching late-frame accounting off. SIGNAGE_VIDEO_QOS=0 / _MAX_LATENESS_MS
     * remain explicit diagnostic knobs (applied in init) and are never set here. */
}

/* Engine visibility. Hiding removes the plane once (HIDE); the browser then retires the
 * held framebuffers through the normal RELEASED path. A frame that was on screen when
 * hidden (e.g. paused) is presented again when shown; frames consumed while invisible are
 * never shown later. */
void admiral_plane_sink_set_visible(GstElement *element, gboolean visible)
{
    AdmiralPlaneSink *self = (AdmiralPlaneSink *)element;
    g_mutex_lock(&self->lock);
    if (!visible && !self->invisible) {
        self->invisible = TRUE;
        self->restore_on_visible = self->last != NULL && !self->hidden;
        if (!self->hidden && self->socket >= 0 && !self->disconnected) {
            SignageVideoMessage hide = { .type = SIGNAGE_VIDEO_HIDE };
            send_message(self, &hide);
        }
        self->hidden = TRUE;
    } else if (visible && self->invisible) {
        self->invisible = FALSE;
        if (self->restore_on_visible && self->last && self->have_info && self->socket >= 0 && !self->disconnected)
            reshow_last_locked(self);
        self->restore_on_visible = FALSE;
    }
    g_mutex_unlock(&self->lock);
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
    if (self->notify) g_array_unref(self->notify);
    g_free(self->ledger);
    g_mutex_clear(&self->ledger_lock);
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
    base->preroll = admiral_plane_sink_preroll;
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
    g_mutex_init(&self->ledger_lock);
    g_cond_init(&self->receiver_wake);
    self->fd = self->socket = -1;
    self->hidden = TRUE;
    self->defer_us = -1;
    /* Initial budget before CAPS/preroll, refined from source rate in set_caps.
     * Prevent playbin's first pre-preroll LATENCY query from selecting zero. */
    gst_base_sink_set_render_delay(GST_BASE_SINK(self), GST_SECOND / 30);
    gst_base_sink_set_processing_deadline(GST_BASE_SINK(self), GST_SECOND / 30);
    self->framebuffers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->held = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, held_free);
    self->stats = g_strcmp0(g_getenv("SIGNAGE_VIDEO_STATS"), "1") == 0;
    if (g_getenv("SIGNAGE_VIDEO_LEDGER_DIR")) {
        self->ledger = g_new0(VideoLedger, 1);
        gst_pad_add_probe(GST_BASE_SINK_PAD(self), GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
                          ledger_upstream, self, NULL);
    }
    self->unpresented = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->identities = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    self->presented_pts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    /* Experiment knobs: SIGNAGE_VIDEO_QOS=0 disables QoS events upstream; SIGNAGE_VIDEO_MAX_LATENESS_MS. */
    if (g_strcmp0(g_getenv("SIGNAGE_VIDEO_QOS"), "0") == 0) gst_base_sink_set_qos_enabled(GST_BASE_SINK(self), FALSE);
    const gchar *lateness = g_getenv("SIGNAGE_VIDEO_MAX_LATENESS_MS");
    if (lateness && *lateness) gst_base_sink_set_max_lateness(GST_BASE_SINK(self), g_ascii_strtoll(lateness, NULL, 10) * GST_MSECOND);
}
