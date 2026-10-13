/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <stdint.h>

/* Broker socket and video-status.json; shared with the sandboxed web process. */
#define SIGNAGE_VIDEO_RUNTIME_DIR "/run/admiral-signage/video"

/* Unset SIGNAGE_VIDEO_MODE defaults to the strict holepunch path. */
gboolean signage_drm_holepunch_enabled(void);

/* ui_* is the browser framebuffer size; mode_* is the CRTC size the primary plane
 * scales it to. WebKit video rectangles are in UI coordinates. */
/* The descriptor remains the browser's descriptor; no new DRM device is opened. */
gboolean signage_drm_broker_start(int drm_fd,
                                  guint connector_id,
                                  guint crtc_id,
                                  guint crtc_index,
                                  guint primary_plane_id,
                                  guint ui_width,
                                  guint ui_height,
                                  guint mode_width,
                                  guint mode_height,
                                  GError **error);
void signage_drm_broker_stop(void);

/* Video frame protocol. After the descriptor transfer the sink keeps the broker
 * connection open and sends SHOW (a framebuffer it created on the shared
 * descriptor, with plane source and CRTC rectangles) or HIDE. The browser puts
 * each SHOW into its next atomic commit together with the UI, so the CRTC sees
 * one commit per vblank, and answers RELEASED once a framebuffer is no longer
 * scanned out or queued. Both ends run from the same image build, so the
 * message is a fixed native-endian struct. The broker metadata advertises the
 * protocol with "video_frames":true; without it the sink commits by itself. */
enum {
    SIGNAGE_VIDEO_SHOW = 1,
    SIGNAGE_VIDEO_HIDE = 2,
    SIGNAGE_VIDEO_RELEASED = 3,
    SIGNAGE_VIDEO_PRESENTED = 4,
    SIGNAGE_VIDEO_CANCELLED = 5, /* terminal SHOW outcome, never retirement */
};

typedef struct {
    uint32_t type;
    uint32_t fb;
    /* SHOW: increments per message. RELEASED: the latest serial the browser had
     * for fb; the sink ignores it if it has since shown fb again. */
    uint32_t serial;
    uint32_t src_x, src_y, src_w, src_h; /* 16.16 fixed point, as KMS SRC_* */
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
} SignageVideoMessage;

/* message is NULL when the sink disconnects; the browser then hides the video. */
typedef void (*SignageVideoHandler)(const SignageVideoMessage *message, gpointer data);

/* Overlay plane chosen and configured by signage_drm_broker_start(), or 0. */
guint signage_drm_broker_video_plane(void);
/* Enables the frame protocol for sinks connecting from now on (NULL disables). */
void signage_drm_broker_set_video_handler(SignageVideoHandler handler, gpointer data);
gboolean signage_drm_broker_video_handler_active(void);
/* Tells the connected sink that fb is no longer on screen or queued. */
void signage_drm_broker_video_released(uint32_t fb, uint32_t serial);
void signage_drm_broker_video_presented(uint32_t fb, uint32_t serial);
void signage_drm_broker_video_cancelled(uint32_t fb, uint32_t serial);
guint64 signage_drm_broker_video_generation(void);
void signage_drm_broker_video_disconnect(void);
