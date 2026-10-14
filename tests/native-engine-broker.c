/* SPDX-License-Identifier: MIT
 * Broker admission with the persistent video engine (actual drm-broker.c, socketpairs).
 */
#include "../native/drm-broker.c"
#include <assert.h>

static unsigned handled;
static void handler(const SignageVideoMessage *message, gpointer data) { (void)message; (void)data; handled++; }

static GSocketConnection *pair(int *peer)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    GSocket *socket = g_socket_new_from_fd(fds[0], NULL);
    assert(socket);
    GSocketConnection *connection = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    *peer = fds[1];
    return connection;
}

/* Returns the received metadata (or NULL on EOF) and whether a descriptor came with it. */
static char *hello_reply(int peer, gboolean *got_fd)
{
    char payload[1024] = {0};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof control);
    struct iovec iov = { .iov_base = payload, .iov_len = sizeof payload - 1 };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes };
    ssize_t n = recvmsg(peer, &message, MSG_DONTWAIT);
    *got_fd = FALSE;
    for (struct cmsghdr *c = n > 0 ? CMSG_FIRSTHDR(&message) : NULL; c; c = CMSG_NXTHDR(&message, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); close(fd); *got_fd = TRUE;
        }
    return n > 0 ? g_strndup(payload, (gsize)n) : NULL;
}

int main(void)
{
    broker.fd = open("/dev/null", O_RDWR);
    assert(broker.fd >= 0);
    broker.metadata = g_strdup("{\"connector_id\":1,\"crtc_id\":2,\"browser_plane_id\":3,\"video_plane_id\":4,\"blend_configured\":true");
    broker.video_handler = handler;

    /* 1. The engine claims the active generation with its explicit role. */
    int engine_peer; GSocketConnection *engine = pair(&engine_peer);
    assert(accept_client(engine, SIGNAGE_BROKER_ENGINE_TOKEN));
    gboolean fd = FALSE; char *meta = hello_reply(engine_peer, &fd);
    assert(meta && fd && strstr(meta, "\"role\":\"engine\"") && strstr(meta, "\"standby\":false"));
    g_free(meta);
    guint64 engine_generation = broker.active;
    assert(engine_generation && broker.engine == engine_generation);

    /* 2. An HTML <video> sink is refused while the engine owns the decoder: no descriptor,
     *    no metadata, connection closed (the page sees a media error, never a 2nd decoder). */
    int html_peer; GSocketConnection *html = pair(&html_peer);
    assert(accept_client(html, 0));
    g_object_unref(html);
    char byte;
    assert(recv(html_peer, &byte, 1, 0) == 0);
    assert(broker.active == engine_generation && broker.refused_html == 1);
    close(html_peer);

    /* 3. Native standby (second decoder) cannot be reserved while the engine is active. */
    assert(signage_drm_broker_video_reserve() == 0);

    /* 4. A second engine connection is refused too (one engine, one decoder). */
    int dup_peer; GSocketConnection *dup = pair(&dup_peer);
    assert(accept_client(dup, SIGNAGE_BROKER_ENGINE_TOKEN));
    g_object_unref(dup);
    assert(recv(dup_peer, &byte, 1, 0) == 0);
    close(dup_peer);

    /* 5. After the engine generation retires, ordinary HTML video is admitted again
     *    (static HTML / legacy lifecycle unaffected when the engine is off). */
    signage_drm_broker_video_retired(engine_generation);
    assert(broker.engine == 0 && broker.active == 0);
    close(engine_peer); g_object_unref(engine);
    int again_peer; GSocketConnection *again = pair(&again_peer);
    assert(accept_client(again, 0));
    meta = hello_reply(again_peer, &fd);
    assert(meta && fd && strstr(meta, "\"role\":\"html\""));
    g_free(meta);
    assert(broker.active && broker.engine == 0);
    for (unsigned i = 0; i < 2; i++) drop_client(&broker.clients[i], FALSE);
    close(again_peer); g_object_unref(again);
    g_free(broker.metadata); close(broker.fd);
    g_print("native-engine-broker: OK\n");
    return 0;
}
