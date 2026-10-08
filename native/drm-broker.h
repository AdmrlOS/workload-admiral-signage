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
