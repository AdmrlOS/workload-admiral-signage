/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <stdint.h>

/* Unset SIGNAGE_VIDEO_MODE defaults to the strict holepunch path. */
gboolean signage_drm_holepunch_enabled(void);

/* The descriptor remains the browser's descriptor; no new DRM device is opened. */
gboolean signage_drm_broker_start(int drm_fd,
                                  guint connector_id,
                                  guint crtc_id,
                                  guint crtc_index,
                                  guint primary_plane_id,
                                  guint width,
                                  guint height,
                                  GError **error);
void signage_drm_broker_stop(void);
