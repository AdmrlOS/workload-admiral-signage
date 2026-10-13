"""Compile and execute the production LATENCY query without hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class VideoLatencyTests(unittest.TestCase):
    def test_live_nonlive_sync_preroll_and_finite_maximum(self):
        source=(ROOT/'native/gst-admiral-videoplane.c').read_text()
        query=source[source.index('static gboolean admiral_plane_sink_query'):source.index('static gboolean admiral_plane_sink_event')]
        harness=r'''
#include <glib.h>
#include <assert.h>
#define GST_CLOCK_TIME_NONE G_MAXUINT64
#define GST_MSECOND 1000000
#define GST_QUERY_LATENCY 1
#define GST_QUERY_TYPE(q) ((q)->type)
#define GST_BASE_SINK(e) (e)
#define GST_ELEMENT_CLASS(c) (c)
typedef guint64 GstClockTime;
typedef struct { gboolean sync; guint64 delay, processing; } GstBaseSink;
typedef GstBaseSink GstElement;
typedef struct { int type; gboolean live; guint64 min,max; } GstQuery;
static gboolean ok=TRUE, upstream, live;
static guint64 minimum,maximum;
static gboolean parent_query(GstElement *e,GstQuery *q) { (void)e;(void)q;return TRUE; }
static struct { gboolean (*query)(GstElement *,GstQuery *); } parent={parent_query}, *admiral_plane_sink_parent_class=&parent;
static gboolean gst_base_sink_query_latency(GstBaseSink *b,gboolean *l,gboolean *u,guint64 *min,guint64 *max) { (void)b;*l=live;*u=upstream;*min=minimum;*max=maximum;return ok; }
static guint64 gst_base_sink_get_render_delay(GstBaseSink *b) { return b->delay; }
static guint64 gst_base_sink_get_processing_deadline(GstBaseSink *b) { return b->processing; }
static gboolean gst_base_sink_get_sync(GstBaseSink *b) { return b->sync; }
static void gst_query_set_latency(GstQuery *q,gboolean l,guint64 min,guint64 max) { q->live=l;q->min=min;q->max=max; }
''' + query + r'''
int main(void) {
 GstElement e={.sync=TRUE,.delay=33333333,.processing=50000000}; GstQuery q={.type=1};
 live=TRUE; upstream=FALSE; minimum=e.delay; maximum=100000000;
 assert(admiral_plane_sink_query(&e,&q)); assert(q.min==83333333 && q.max==150000000);
 upstream=TRUE; minimum=83333333; maximum=150000000;
 assert(admiral_plane_sink_query(&e,&q)); assert(q.min==83333333 && q.max==150000000);
 maximum=GST_CLOCK_TIME_NONE; assert(admiral_plane_sink_query(&e,&q)); assert(q.max==GST_CLOCK_TIME_NONE);
 live=FALSE; upstream=FALSE; minimum=0; e.sync=FALSE;
 assert(admiral_plane_sink_query(&e,&q)); assert(q.min==0 && !q.live);
 ok=FALSE; e.sync=TRUE; assert(admiral_plane_sink_query(&e,&q)); assert(q.min==83333333);
 e.sync=FALSE; assert(admiral_plane_sink_query(&e,&q)); assert(!q.live && q.min==0);
 return 0;
}
'''
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'test.c').write_text(harness)
            subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test'),*flags],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=10)
