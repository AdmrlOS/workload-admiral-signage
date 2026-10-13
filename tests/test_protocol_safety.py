"""Execute production browser state transitions without DRM hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class ProtocolSafetyTests(unittest.TestCase):
    def test_malformed_reshow_does_not_retire_live_fb(self):
        source = (ROOT/'browser/signage-browser.c').read_text()
        types = source[source.index('typedef struct {\n    uint32_t fb, serial;'):source.index('/* Per reporting interval;')]
        release = source[source.index('static void video_release_if_unused('):source.index('static gboolean submit_commit(')]
        handler = source[source.index('static void video_message('):source.index('static void setup_video_plane(')]
        header = (ROOT/'native/drm-broker.h').read_text()
        protocol = header[header.index('enum {'):header.index('/* message is NULL')]
        harness = r'''
#include <glib.h>
#include <stdint.h>
#include <assert.h>
''' + protocol + types + r'''
typedef struct { VideoPlane video; } Browser;
static unsigned releases, cancellations, disconnects;
static guint64 generation = 1;
static guint64 signage_drm_broker_video_generation(void) { return generation; }
static void signage_drm_broker_video_disconnect(void) { disconnects++; }
static void signage_drm_broker_video_released(uint32_t fb, uint32_t serial) { (void)fb; (void)serial; releases++; }
static void signage_drm_broker_video_cancelled(uint32_t fb, uint32_t serial) { (void)fb; (void)serial; cancellations++; }
static gboolean submit_commit(Browser *b) { (void)b; return TRUE; }
''' + release + handler + r'''
int main(void) {
 Browser b={0}; b.video.plane=1; b.video.serials=g_hash_table_new(g_direct_hash,g_direct_equal);
 b.video.current.fb=7;
 g_hash_table_insert(b.video.serials,GUINT_TO_POINTER(7),GUINT_TO_POINTER(1));
 SignageVideoMessage malformed={.type=SIGNAGE_VIDEO_SHOW,.fb=7,.serial=2};
 video_message(&malformed,&b);
 assert(releases==0); /* cancellation is NOT scanout retirement */
 assert(cancellations==1);
 b.video.current.fb=0; video_release_if_unused(&b,7); assert(releases==1);
 SignageVideoMessage show={.type=SIGNAGE_VIDEO_SHOW,.fb=8,.serial=3,.src_w=1,.src_h=1,.crtc_w=1,.crtc_h=1};
 video_message(&show,&b); assert(b.video.dirty);
 SignageVideoMessage hide={.type=SIGNAGE_VIDEO_HIDE};
 video_message(&hide,&b); assert(cancellations==2); assert(releases==2); b.video.dirty=FALSE;
 show.serial=4; video_message(&show,&b);
 show.serial=5; video_message(&show,&b);
 assert(b.video.waiting.length==1);
 show.serial=6; video_message(&show,&b); assert(disconnects==1);
 video_message(&hide,&b); assert(cancellations==4);
 b.video.pending=(VideoState){.fb=8,.serial=7,.generation=1}; b.video.pending_valid=TRUE;
 video_message(&hide,&b); assert(cancellations==5);
 b.video.pending_valid=FALSE;
 generation=2; b.video.dirty=FALSE;
 b.video.pending=(VideoState){.fb=8,.serial=1,.generation=1}; b.video.pending_valid=TRUE;
 unsigned before=cancellations; video_message(&hide,&b); assert(cancellations==before);
 b.video.pending_valid=FALSE; b.video.dirty=FALSE;
 show.serial=UINT32_MAX; video_message(&show,&b);
 show.serial=0; video_message(&show,&b); assert(disconnects==2);
 video_message(&hide,&b);
 g_hash_table_unref(b.video.serials);
 return 0;
}
'''
        flags = subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory); (p/'test.c').write_text(harness)
            subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test'),*flags],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=30)

class BrokerBackpressureTests(unittest.TestCase):
    def test_eagain_preserves_entire_records(self):
        source = (ROOT/'native/drm-broker.c').read_text()
        start = source.index('static gboolean video_writable(') if 'static gboolean video_writable(' in source else source.index('static void\nvideo_ack(')
        ack = source[start:source.index('static gboolean\nprepare_socket(')]
        header=(ROOT/'native/drm-broker.h').read_text()
        protocol=header[header.index('enum {'):header.index('/* message is NULL')]
        queue_types = source[source.index('typedef struct {\n    SignageVideoMessage message;'):source.index('static Broker broker')] if 'SignageVideoMessage message;\n    gsize offset;' in source else ''
        drop = source[source.index('static void\ndrop_client('):source.index('static gboolean\nclient_readable(')]
        harness=r'''
#include <gio/gio.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
''' + protocol + 'typedef void (*SignageVideoHandler)(const SignageVideoMessage *, gpointer);\n' + (queue_types if queue_types else 'typedef struct { GSocketConnection *client; GQueue outgoing; GSource *write_source; guint64 generation; } Broker;\n') + r'''
static Broker broker;
static int interrupted;
static gboolean constrain;
static ssize_t constrained_send(int fd, const void *data, size_t len, int flags) {
 if(constrain && !interrupted++) { errno=EINTR; return -1; }
 return send(fd,data,constrain && len>7 ? 7 : len,flags);
}
#define send constrained_send
''' + drop + ack + r'''
int main(void) {
 int fd[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);
 int size=1024; setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size));
 GError *error=NULL; GSocket *socket=g_socket_new_from_fd(fd[0],&error); assert(socket);
 broker.client=g_socket_connection_factory_create_connection(socket); g_object_unref(socket);
 char fill[4096]={0}; while(send(fd[0],fill,sizeof(fill),MSG_DONTWAIT)>0) {}
 assert(errno==EAGAIN);
 constrain=TRUE;
 signage_drm_broker_video_presented(7,1);
 while(recv(fd[1],fill,sizeof(fill),MSG_DONTWAIT)>0) {}
 SignageVideoMessage msg; size_t got=0;
 for(int i=0;i<100 && got<sizeof(msg);i++) {
  while(g_main_context_iteration(NULL,FALSE)) {}
  ssize_t n=recv(fd[1],(char *)&msg+got,sizeof(msg)-got,MSG_DONTWAIT);
  if(n>0) got+=(size_t)n;
  g_usleep(1000);
 }
 assert(got==sizeof(msg)); assert(msg.type==SIGNAGE_VIDEO_PRESENTED && msg.fb==7 && msg.serial==1);
 assert(broker.client);
 constrain=FALSE;
 while(send(fd[0],fill,sizeof(fill),MSG_DONTWAIT)>0) {}
 for(unsigned i=2;i<200;i++) signage_drm_broker_video_presented(7,i);
 assert(!broker.client); assert(broker.outgoing.length==0); assert(!broker.write_source);
 close(fd[1]); return 0;
}
'''
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0'],text=True).split()
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory); (p/'test.c').write_text(harness)
            subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test'),*flags],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=30)

class RecoveryTests(unittest.TestCase):
    def test_unretired_stop_enforces_process_recovery(self):
        source=(ROOT/'native/gst-admiral-videoplane.c').read_text()
        start=source.index('    if (g_hash_table_size(self->held)) {',source.index('admiral_plane_sink_stop('))
        block=source[start:source.index('    self->have_info = FALSE;',start)]
        harness=r'''
#include <glib.h>
#define GST_ERROR_OBJECT(...) ((void)0)
static void held_free(gpointer p) { g_free(p); }
typedef struct { GHashTable *held, *framebuffers; } Sink;
static void prune_framebuffers(Sink *self, gboolean all) { (void)self; (void)all; }
int main(void) { Sink s={0}, *self=&s; s.held=g_hash_table_new(g_direct_hash,g_direct_equal);
g_hash_table_insert(s.held,GUINT_TO_POINTER(1),GUINT_TO_POINTER(1));
''' + block + 'return 0; }\n'
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory); (p/'test.c').write_text(harness)
            subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test'),*flags],check=True)
            result=subprocess.run([str(p/'test')],timeout=10)
            self.assertLess(result.returncode,0,'unsafe retirement must terminate, not leak unreachable tables and continue')

if __name__ == '__main__': unittest.main()
