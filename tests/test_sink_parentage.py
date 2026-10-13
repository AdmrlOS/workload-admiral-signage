"""WebKit initializes WesterosVideoSink before parenting it into playbin."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ParentageTests(unittest.TestCase):
 def test_unparented_placeholder_never_opens_legacy_drm(self):
  s=(ROOT/'native/gst-admiral-holepunch.c').read_text()
  init=s[s.index('static void\nadmiral_sink_init('):s.index('static gboolean\nplugin_init(')]
  self.assertIn('if (self->kms && self->fd < 0) gst_element_set_locked_state(self->kms, TRUE)',s,
                'unparented WebKit READY probe must not initialize legacy kmssink')

 def test_paused_standby_delivers_real_preroll(self):
  s=(ROOT/'native/gst-admiral-videoplane.c').read_text()
  self.assertIn('base->preroll = admiral_plane_sink_preroll;',s,
                'PAUSED standby must submit retained first DMA-BUF, not wait for PLAYING')

 def test_unparented_ready_defers_broker_until_paused(self):
  s=(ROOT/'native/gst-admiral-holepunch.c').read_text()
  start=s.find('static gboolean\nadmiral_sink_needs_setup(')
  self.assertGreaterEqual(start,0,'missing deferred parentage setup gate')
  f=s[start:s.index('\nstatic GstStateChangeReturn',start)]
  harness='''#include <assert.h>
typedef int gboolean;
typedef int gint;
typedef int GstStateChange;
#define GST_STATE_CHANGE_NULL_TO_READY 1
#define GST_STATE_CHANGE_READY_TO_PAUSED 2
#define GST_STATE_CHANGE_PAUSED_TO_PLAYING 3
'''+f+'''
int main(void) {
 assert(!admiral_sink_needs_setup(1,0,-1));
 assert(admiral_sink_needs_setup(1,1,-1));
 assert(admiral_sink_needs_setup(2,1,-1));
 assert(!admiral_sink_needs_setup(2,0,-1));
 assert(!admiral_sink_needs_setup(2,1,4));
 assert(!admiral_sink_needs_setup(3,1,4));
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'gate.c').write_text(harness)
   subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p/'gate.c'),'-o',str(p/'gate')],check=True)
   subprocess.run([str(p/'gate')],check=True)
