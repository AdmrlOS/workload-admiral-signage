/* SPDX-License-Identifier: MIT */
#pragma once

#include <gst/gst.h>

/* A video sink that imports decoded DMA-BUF frames as framebuffers on the
 * browser's brokered DRM descriptor and asks the browser to put them on the
 * overlay plane in its own atomic commits (see drm-broker.h). */
GType admiral_plane_sink_get_type(void);
void admiral_plane_sink_set_generation(GstElement *element, guint64 generation, gboolean standby);
/* Persistent video engine client: posts "admiral-presented" element messages
 * (pts, serial, fb, presented_total) only for SHOWs the browser reported PRESENTED
 * after the matching flip, keeps the last frame through flushes and never re-SHOWs an
 * already submitted buffer. */
void admiral_plane_sink_set_engine(GstElement *element, gboolean engine);
/* Engine only: FALSE consumes frames without SHOW (warm-up/hidden) and HIDEs once. */
void admiral_plane_sink_set_visible(GstElement *element, gboolean visible);
/* Diagnostic stages: 1 render, 2 SHOW, 3 PRESENTED, 4 decoder out,
 * 5 sink input, 6 upstream QoS timestamp/jitter. Disabled by default. */
void admiral_plane_sink_record(GstElement *sink, guint stage, guint64 pts, guint64 value);

/* Borrowed: drm_fd and socket_fd stay owned by the caller and must outlive the
 * sink's PAUSED->READY transition. mode_* is the CRTC size used for clipping. */
void admiral_plane_sink_configure(GstElement *sink, gint drm_fd, gint socket_fd,
                                  gint mode_width, gint mode_height);
