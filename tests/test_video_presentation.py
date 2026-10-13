"""Presentation acknowledgements must not be confused with buffer retirement."""
import pathlib
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class VideoPresentationTests(unittest.TestCase):
    def test_flip_acks_exact_serial_and_sink_waits_before_eos(self):
        header = (ROOT/'native/drm-broker.h').read_text()
        browser = (ROOT/'browser/signage-browser.c').read_text()
        sink = (ROOT/'native/gst-admiral-videoplane.c').read_text()
        self.assertIn('SIGNAGE_VIDEO_PRESENTED = 4', header)
        flip = browser[browser.index('static void frame_presented('):browser.index('static void video_message(')]
        self.assertIn('signage_drm_broker_video_ack(video->current.generation, SIGNAGE_VIDEO_PRESENTED, video->current.fb, video->current.serial)', flip)
        self.assertIn('wait_presented(self)', sink)
        self.assertIn('GST_EVENT_EOS', sink)
        self.assertIn('base->event = admiral_plane_sink_event;', sink)
        self.assertIn('base->unlock = admiral_plane_sink_unlock;', sink)
        self.assertIn('base->unlock_stop = admiral_plane_sink_unlock_stop;', sink)

    def test_sink_reports_two_vblank_render_delay(self):
        sink = (ROOT/'native/gst-admiral-videoplane.c').read_text()
        self.assertIn('gst_base_sink_set_render_delay', sink)
        self.assertIn('2 * GST_SECOND', sink)
        self.assertIn('GST_VIDEO_INFO_FPS_N', sink)
        self.assertIn('element->query = admiral_plane_sink_query;', sink)
        self.assertIn('3 * GST_SECOND', sink)
    def test_live_latency_does_not_add_base_processing_budget_twice(self):
        sink = (ROOT/'native/gst-admiral-videoplane.c').read_text()
        query = sink[sink.index('static gboolean admiral_plane_sink_query'):sink.index('static gboolean admiral_plane_sink_event')]
        self.assertIn('gst_base_sink_query_latency', query)
        self.assertIn('upstream_live', query)
        self.assertIn('!upstream_live', query)
    def test_flush_fences_old_credits_and_serial_does_not_wrap(self):
        sink=(ROOT/'native/gst-admiral-videoplane.c').read_text()
        event=sink[sink.index('static gboolean admiral_plane_sink_event'):sink.index('static gboolean admiral_plane_sink_unlock')]
        self.assertIn('GST_EVENT_FLUSH_START',event)
        self.assertIn('g_hash_table_remove_all(self->unpresented)',event)
        self.assertIn('g_hash_table_remove_all(self->identities)',event)
        show=sink[sink.index('show_locked('):sink.index('admiral_plane_sink_show_frame(')]
        self.assertIn('self->serial == G_MAXUINT32',show)
        self.assertNotIn('self->serial = 0',event)
    def test_acceptance_trace_preserves_ack_order_without_fixed_clip_length(self):
        sink = (ROOT/'native/gst-admiral-videoplane.c').read_text()
        ack = sink[sink.index('if (message.type == SIGNAGE_VIDEO_PRESENTED)'):sink.index('if (message.type != SIGNAGE_VIDEO_RELEASED)')]
        self.assertIn('Video presented identity: serial=%u fb=%u pts=', ack)
        self.assertIn('SIGNAGE_VIDEO_TRACE_PRESENTED', sink)
        self.assertNotIn('Video missing PTS:', sink)
        self.assertNotIn('g_hash_table_size(self->presented_pts) < 600', sink)
if __name__ == '__main__': unittest.main()
