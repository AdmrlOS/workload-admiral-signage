"""Exercise the actual receiver and stream parser with GLib/socketpair on macOS."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class ReleaseReceiverTests(unittest.TestCase):
    def test_stalled_render_partial_stale_releases_and_stop_race(self):
        source = (ROOT / 'native/gst-admiral-videoplane.c').read_text()
        parser = source[source.index('static void\ndrain_releases('):source.index('static void\nremove_framebuffer(')]
        harness = r'''
#include <glib.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>
#define SIGNAGE_VIDEO_RELEASED 3
#define GST_WARNING_OBJECT(...) ((void)0)
typedef struct { guint32 type, fb, serial; } SignageVideoMessage;
typedef struct { guint32 serial; } HeldFrame;
typedef struct { GMutex lock; GCond receiver_wake; gboolean receiver_stop, disconnected;
int socket; GHashTable *held; guint8 incoming[sizeof(SignageVideoMessage)]; gsize incoming_length; } AdmiralPlaneSink;
''' + parser + r'''
int main(void) {
for (int iteration=0; iteration<100; iteration++) {
 int fd[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd)==0);
 AdmiralPlaneSink self={.socket=fd[0]}; g_mutex_init(&self.lock); g_cond_init(&self.receiver_wake);
 self.held=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
 HeldFrame *held=g_new0(HeldFrame,1); held->serial=2;
 g_hash_table_insert(self.held,GUINT_TO_POINTER(7),held);
 GThread *thread=g_thread_new("test-releases",release_receiver,&self);
 SignageVideoMessage old={3,7,1}, current={3,7,2};
 assert(write(fd[1],&old,sizeof(old))==sizeof(old)); g_usleep(15000);
 g_mutex_lock(&self.lock); assert(g_hash_table_size(self.held)==1); g_mutex_unlock(&self.lock);
 assert(write(fd[1],&current,2)==2); g_usleep(7000);
 assert(write(fd[1],((char*)&current)+2,sizeof(current)-2)==sizeof(current)-2);
 gint64 deadline=g_get_monotonic_time()+1000000;
 gboolean released=FALSE;
 while(g_get_monotonic_time()<deadline) {
  g_mutex_lock(&self.lock); released=g_hash_table_size(self.held)==0; g_mutex_unlock(&self.lock);
  if(released) break; g_usleep(1000);
 }
 assert(released); /* no show_frame, expose, or decoder activity occurred */
 close(fd[1]); /* EOF concurrent with stop */
 g_mutex_lock(&self.lock); self.receiver_stop=TRUE; g_cond_signal(&self.receiver_wake); g_mutex_unlock(&self.lock);
 g_thread_join(thread); close(fd[0]); g_hash_table_unref(self.held); g_cond_clear(&self.receiver_wake);g_mutex_clear(&self.lock);
}
return 0;
}
'''
        if subprocess.run(['pkg-config','--exists','glib-2.0'], check=False).returncode:
            self.skipTest("GLib development package required; run native lifecycle test in image")
        flags = subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); (path/'test.c').write_text(harness)
            subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test'),*flags],check=True)
            subprocess.run([str(path/'test')],check=True,timeout=30)
