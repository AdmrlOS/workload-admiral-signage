from pathlib import Path
import unittest
R=Path(__file__).resolve().parents[1]
class NativeStandbyWiring(unittest.TestCase):
 def test_renderer_reuses_bounded_spare_on_owner_context(self):
  s=(R/'browser/standby-renderer.h').read_text()
  self.assertIn('native-warm-pool',s)
  self.assertIn('browser->spare_view',s)
  self.assertIn('recycle_stage_view(browser, browser->retiring_view)',s)
  self.assertIn('stage=browser->spare_view; browser->spare_view=NULL;',s)

 def test_capacity_is_uri_specific_and_fails_closed(self):
  s=(R/'browser/standby-renderer.h').read_text()
  self.assertIn('SIGNAGE_STANDBY_CMA_BY_PATH',s)
  self.assertIn('standby_capacity(uri,',s)

 def test_physical_standby_is_wired(self):
  b=(R/'browser/signage-browser.c').read_text()+(R/'browser/standby-renderer.h').read_text()
  self.assertIn('signage_standby_ready(',b)
  self.assertIn('signage_standby_flipped(',b)
  self.assertIn('root->visible_view == browser',b)
  self.assertIn('DRM_MODE_ATOMIC_TEST_ONLY',b)
  s=(R/'native/gst-admiral-videoplane.c').read_text()
  self.assertIn('SIGNAGE_VIDEO_ENDPOINT',s)
  p=(R/'www/player/player.js').read_text()
  self.assertIn('native-stage',p)
 def test_low_rate_decoder_lead_is_time_bounded_and_4k_stays_four_buffers(self):
  # PiP (24 fps) lost frames when a ~250-400 ms decoder hiccup outlasted a
  # four-buffer (~170 ms) lead; the lead must be time-sized but byte-capped.
  v=(R/'native/gst-admiral-videoplane.c').read_text()
  h=(R/'native/gst-admiral-holepunch.c').read_text()
  self.assertIn('SIGNAGE_DECODE_LEAD_MS',v)
  self.assertIn('lead_ms = 700, cap_mb = 48',v)
  self.assertIn('MIN_POOL_BUFFERS',v)
  self.assertIn('SIGNAGE_DELIVERY_QUEUE_MS',h)
  self.assertIn('queue_ms = 600, queue_mb = 48',h)
  self.assertIn('"leaky", 0',h)  # never leaky
 def test_flip_trace_is_opt_in_and_logs_only_anomalies(self):
  # export -> commit -> kernel stamp -> receipt ring; must not log per flip.
  b=(R/'browser/signage-browser.c').read_text()
  self.assertIn('SIGNAGE_FLIP_TRACE',b)
  self.assertIn('exported_us = export_started',b)
  self.assertIn('received - flip_trace_cur.submitted > 40000',b)
 def test_stall_trace_is_opt_in_watchdog_without_in_stall_logging(self):
  b=(R/'browser/signage-browser.c').read_text()
  self.assertIn('SIGNAGE_STALL_TRACE',b)
  self.assertIn('/proc/self/task/%d/%s',b)
  self.assertIn('Main-loop stall',b)
  self.assertIn('stall user-stack',b)
  self.assertIn('pthread_kill(stall_main_thread, SIGUSR2)',b)
  self.assertIn('stall thread state=',b)
  self.assertIn('/proc/pressure/%s',b)

 def test_playback_guard_forbids_creation_and_times_pooled_loads(self):
  b=(R/'browser/signage-browser.c').read_text()
  r=(R/'browser/standby-renderer.h').read_text()
  self.assertIn('Playback guard: WebView creation during active playback',b)
  self.assertIn('webkit_created_in_playback',b)
  self.assertIn('native-playback',r)
  self.assertIn('SIGNAGE_PLAYBACK_WEBKIT',r)
  self.assertIn('refuse',r.lower())
  # the only navigation in the standby path is the timed pooled one
  self.assertEqual(r.count('webkit_web_view_load_uri('),1)
  self.assertIn('PLAYBACK_LOAD_BUDGET_US',r)

 def test_cancelled_stage_returns_to_pool_instead_of_forcing_later_creation(self):
  r=(R/'browser/standby-renderer.h').read_text()
  self.assertIn('retain_or_destroy_staged(browser,browser->staged_view)',r)
  self.assertIn('retain_or_destroy_staged(browser,stage)',r)
  self.assertNotIn('destroy_stage_view(browser->staged_view); browser->staged_view=NULL;\n            if (!signage_drm',r)

 def test_gem_close_is_off_the_presentation_thread_and_never_reuses_a_closing_handle(self):
  b=(R/'browser/signage-browser.c').read_text()
  self.assertIn('drm_gem_dma_free',b)  # documents the measured main-thread kernel stack
  self.assertIn('HANDLE_KEEPALIVE_MAX',b)
  self.assertIn('reap_thread',b)
  # the only GEM_CLOSE sites are the reaper thread and its no-thread fallback
  self.assertEqual(b.count('DRM_IOCTL_GEM_CLOSE'),2)
  hu=b[b.index('static void handle_unref'):b.index('static gboolean idle_handle_age_tick')]
  self.assertNotIn('DRM_IOCTL_GEM_CLOSE',hu)
  self.assertIn('reap_blocks(frame->handles[i])',b)
