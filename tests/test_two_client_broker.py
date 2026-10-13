"""Compile the real broker transport and exercise two independent socketpairs."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class TwoClientBroker(unittest.TestCase):
 def test_generation_routing(self):
  s=(ROOT/'native/drm-broker.c').read_text()
  self.assertIn('BrokerClient clients[2]',s, 'second connection must not replace active')
  h=(ROOT/'native/drm-broker.h').read_text()
  self.assertIn('uint64_t generation;',h)
  types=s[s.index('typedef struct {\n    SignageVideoMessage message;'):s.index('gboolean\nsignage_drm_holepunch_enabled')]
  transport=s[s.index('static void\ndrop_client('):s.index('static gboolean\naccept_client(')]
  ack=s[s.index('static gboolean video_writable('):s.index('static gboolean\nprepare_socket(')]
  harness='''
#include <gio/gio.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
'''+h[h.index('enum {'):h.index('/* message is NULL')]+'''typedef void (*SignageVideoHandler)(const SignageVideoMessage *, gpointer);
'''+types+transport+ack+'''
int main(void) {
 int f[2][2];
 for(unsigned i=0;i<2;i++) {
  assert(!socketpair(AF_UNIX,SOCK_STREAM,0,f[i]));
  GSocket *socket=g_socket_new_from_fd(f[i][0],NULL); assert(socket);
  broker.clients[i].client=g_socket_connection_factory_create_connection(socket);
  g_object_unref(socket); broker.clients[i].generation=10+i;
 }
 signage_drm_broker_video_ack(11,SIGNAGE_VIDEO_PRESENTED,7,1);
 SignageVideoMessage m;
 assert(recv(f[1][1],&m,sizeof(m),0)==sizeof(m));
 assert(m.generation==11 && m.fb==7 && m.serial==1);
 assert(recv(f[0][1],&m,sizeof(m),MSG_DONTWAIT)<0 && errno==EAGAIN);
 signage_drm_broker_video_ack(9,SIGNAGE_VIDEO_RELEASED,7,1);
 assert(recv(f[1][1],&m,sizeof(m),MSG_DONTWAIT)<0 && errno==EAGAIN);
 for(unsigned i=0;i<2;i++) { drop_client(&broker.clients[i],FALSE); close(f[i][1]); }
 return 0;
}
'''
  callbacks='''
static unsigned received, hides;
static guint64 last_generation;
static void handler(const SignageVideoMessage *m,gpointer data) {
 (void)data; received++; last_generation=m->generation; if(m->type==SIGNAGE_VIDEO_HIDE) hides++;
}
'''
  harness=harness.replace(types+transport+ack,types+callbacks+transport+ack)
  harness=harness.replace(' for(unsigned i=0;i<2;i++) { drop_client', ''' broker.video_handler=handler;
 SignageVideoMessage show={.type=SIGNAGE_VIDEO_SHOW,.generation=10,.fb=100,.serial=1};
 assert(write(f[0][1],&show,3)==3);
 client_readable(g_socket_connection_get_socket(broker.clients[0].client),G_IO_IN,&broker.clients[0]);
 assert(received==0 && broker.clients[0].pending_length==3);
 SignageVideoMessage hide={.type=SIGNAGE_VIDEO_HIDE,.generation=11};
 assert(write(f[1][1],&hide,sizeof(hide))==sizeof(hide));
 client_readable(g_socket_connection_get_socket(broker.clients[1].client),G_IO_IN,&broker.clients[1]);
 assert(received==1 && hides==1 && last_generation==11 && broker.clients[0].client);
 assert(write(f[0][1],(char *)&show+3,sizeof(show)-3)==sizeof(show)-3);
 client_readable(g_socket_connection_get_socket(broker.clients[0].client),G_IO_IN,&broker.clients[0]);
 assert(received==2 && last_generation==10);
 // Serial replay disconnects only its owner; generation is not reset/reused.
 assert(write(f[0][1],&show,sizeof(show))==sizeof(show));
 client_readable(g_socket_connection_get_socket(broker.clients[0].client),G_IO_IN,&broker.clients[0]);
 assert(!broker.clients[0].client && broker.clients[1].client);
 broker.active=11; assert(signage_drm_broker_video_reserve()==0);
 signage_drm_broker_video_retired(10); broker.generation=11;
 assert(signage_drm_broker_video_reserve()==12);
 assert(signage_drm_broker_video_reserve()==0);
 for(unsigned i=0;i<2;i++) { drop_client''')
  flags=subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0'],text=True).split()
  with tempfile.TemporaryDirectory() as d:
   p=Path(d); (p/'test.c').write_text(harness)
   subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test'),*flags],check=True)
   subprocess.run([str(p/'test')],check=True,timeout=30)
