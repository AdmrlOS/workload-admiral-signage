"""Execute the production native UI/video gate with instrumented KMS calls.
No display is emulated as real hardware: this tests ordering/ownership only.
"""
from pathlib import Path
import subprocess,tempfile,unittest
R=Path(__file__).resolve().parents[1]
class NativeActivation(unittest.TestCase):
 def test_testonly_endpoint_interval_ui_fence_and_matching_flip(self):
  browser=(R/'browser/signage-browser.c').read_text()
  types=browser[browser.index('typedef struct {\n    uint32_t fb, serial;'):browser.index('/* Per reporting interval;')]
  production=(R/'browser/standby-renderer.h').read_text()
  funcs=production[production.index('static gboolean generation_busy('):production.index('static gboolean standby_tick(')]
  h=r'''
#include <glib.h>
#include <stdint.h>
#include <assert.h>
#include "standby-protocol.h"
'''+types+r'''
typedef struct { uint32_t fb; gboolean ready; void *owner; } Frame;
typedef struct { VideoPlane video; void *staged_view,*retiring_view; guint64 cancelled_generation,dead_generation,stage_ui_generation; int64_t stage_deadline; gboolean stage_ui_ready,activation_requested,commit_pending; Frame *queued,*current,*pending; int fd; uint32_t crtc; } Browser;
typedef struct {unsigned properties;} drmModeAtomicReq;
#define DRM_MODE_ATOMIC_TEST_ONLY 0x100
static unsigned tests,commits,acks,notices;
static gboolean kms_ok=TRUE;
static drmModeAtomicReq *drmModeAtomicAlloc(void){return g_new0(drmModeAtomicReq,1);}
static void drmModeAtomicFree(drmModeAtomicReq *r){g_free(r);}
static int drmModeAtomicAddProperty(drmModeAtomicReq *r,uint32_t o,uint32_t p,uint64_t v){(void)o;(void)p;(void)v;r->properties++;return 0;}
static int drmModeAtomicCommit(int fd,drmModeAtomicReq *r,uint32_t flags,void *data){(void)fd;(void)data;assert(flags==DRM_MODE_ATOMIC_TEST_ONLY && r->properties>=11);tests++;return kms_ok?0:-1;}
#define ADD(o,p,v) do { if(drmModeAtomicAddProperty(request,o,p,v)<0) valid=FALSE; } while(0)
static gboolean add_ui_state(Browser *b,drmModeAtomicReq *r,Frame *f){(void)b;assert(f->ready);r->properties++;return TRUE;}
static gboolean frame_ready(Frame *f){return f->ready;}
static void destroy_stage_view(Browser *b){(void)b;}
static void release_frame(Browser *b,Frame *f){(void)b;(void)f;}
static gboolean signage_drm_broker_video_tx_idle(guint64 g){(void)g;return TRUE;}
static gboolean signage_drm_broker_video_connected(guint64 g){(void)g;return TRUE;}
static guint64 signage_drm_broker_video_generation(void){return 1;}
static void signage_drm_broker_video_retired(guint64 g){(void)g;}
static void signage_drm_broker_video_ack(guint64 g,uint32_t type,uint32_t fb,uint32_t serial){assert(g==2 && fb==20 && serial==1);(void)type;acks++;}
#define SIGNAGE_VIDEO_PREPARED 6
static void native_notice(Browser *b,const char *e,guint64 g){(void)b;(void)e;(void)g;notices++;}
static gboolean submit_commit(Browser *b){assert(b->queued && b->video.dirty && b->video.next.fb==20);assert(b->video.gate.phase==SIGNAGE_STANDBY_COMMITTING);commits++;return TRUE;}
'''+funcs+r'''
int main(void){
 Browser b={0},stage={0}; Frame ui={.fb=30,.ready=FALSE};
 b.staged_view=&stage; stage.queued=&ui; b.stage_ui_ready=TRUE;
 b.video.current=(VideoState){.fb=10,.serial=600,.generation=1};
 b.video.standby=(VideoState){.fb=20,.serial=1,.generation=2};
 assert(signage_standby_prepare(&b.video.gate,1,2,2,200,100));
 standby_progress(&b); assert(!tests && !acks && !commits);
 ui.ready=TRUE; kms_ok=FALSE; standby_progress(&b); assert(tests==1 && !acks && !commits);
 kms_ok=TRUE; standby_progress(&b); assert(acks==1 && b.video.gate.phase==SIGNAGE_STANDBY_READY && !commits);
 b.activation_requested=TRUE;
 standby_progress(&b); assert(!commits); // no outgoing endpoint proof
 guint64 now=g_get_monotonic_time();
 assert(signage_standby_endpoint(&b.video.gate,1,600,10,now,100000));
 standby_progress(&b); assert(!commits); // incomplete final display interval
 b.video.gate.final_presented_at=now-100001;
 b.commit_pending=TRUE; standby_progress(&b); assert(!commits);
 b.commit_pending=FALSE; ui.ready=FALSE; standby_progress(&b); assert(!commits);
 ui.ready=TRUE; standby_progress(&b); assert(commits==1 && b.queued==&ui && !stage.queued && !b.video.standby.fb);
 assert(!signage_standby_flipped(&b.video.gate,1,1,20));
 assert(!signage_standby_flipped(&b.video.gate,2,2,20));
 assert(signage_standby_flipped(&b.video.gate,2,1,20));
 b.video.current=(VideoState){.fb=20,.serial=1,.generation=2}; b.video.dirty=FALSE;
 standby_progress(&b); assert(b.video.gate.phase==SIGNAGE_STANDBY_EMPTY && b.video.gate.active==2);
 return 0;
}
'''
  flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(h)
   subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(R/'native'),str(p/'test.c'),'-o',str(p/'test'),*flags],check=True)
   subprocess.run([str(p/'test')],check=True,timeout=20)
