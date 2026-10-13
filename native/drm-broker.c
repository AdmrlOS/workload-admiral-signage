/* SPDX-License-Identifier: MIT
 * Share the browser's existing DRM master with the local GStreamer holepunch adapter.
 * Never open a second card descriptor or acquire/drop DRM master here.
 */
#define _GNU_SOURCE
#include "drm-broker.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Its own directory: the browser binds it into the WebKit sandbox, where the
 * holepunch sink runs, without exposing the rest of the runtime directory. */
#define BROKER_DIR SIGNAGE_VIDEO_RUNTIME_DIR
#define BROKER_PATH BROKER_DIR "/drm.sock"

typedef struct {
    drmModePropertyRes *info;
    uint64_t current;
} PlaneProperty;

typedef struct {
    SignageVideoMessage message;
    gsize offset;
} VideoTx;

typedef struct {
    GSocketConnection *client;
    GSource *client_source, *write_source;
    guint8 pending[sizeof(SignageVideoMessage)];
    gsize pending_length;
    GQueue outgoing;
    guint64 generation;
    uint32_t last_serial;
} BrokerClient;

typedef struct {
    GSocketService *service;
    int fd;
    char *metadata; /* without the trailing "}\n"; see send_metadata() */
    ino_t socket_inode;
    dev_t socket_device;
    guint video_plane;
    SignageVideoHandler video_handler;
    gpointer video_data;
    BrokerClient clients[2]; /* includes disconnected/retiring owners */
    guint64 generation, active, reserved;
} Broker;

static Broker broker = { .fd = -1 };

gboolean
signage_drm_holepunch_enabled(void)
{
    return g_strcmp0(g_getenv("SIGNAGE_VIDEO_MODE"), "composited") != 0;
}

static void
property_clear(PlaneProperty *property)
{
    if (property->info)
        drmModeFreeProperty(property->info);
    property->info = NULL;
}

static gboolean
property_get(int fd, uint32_t plane_id, const char *name, PlaneProperty *result)
{
    drmModeObjectProperties *properties = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!properties)
        return FALSE;
    gboolean found = FALSE;
    for (uint32_t i = 0; i < properties->count_props; i++) {
        drmModePropertyRes *info = drmModeGetProperty(fd, properties->props[i]);
        if (!info)
            continue;
        if (g_strcmp0(info->name, name) == 0) {
            result->info = info;
            result->current = properties->prop_values[i];
            found = TRUE;
            break;
        }
        drmModeFreeProperty(info);
    }
    drmModeFreeObjectProperties(properties);
    return found;
}

static gboolean
property_limits(const PlaneProperty *property, uint64_t *minimum, uint64_t *maximum)
{
    if (property->info->flags & DRM_MODE_PROP_IMMUTABLE) {
        *minimum = *maximum = property->current;
        return TRUE;
    }
    if (!(property->info->flags & DRM_MODE_PROP_RANGE) || property->info->count_values < 2)
        return FALSE;
    *minimum = property->info->values[0];
    *maximum = property->info->values[1];
    return *minimum <= *maximum;
}

static gboolean
property_enum(const PlaneProperty *property, const char *name, uint64_t *value)
{
    for (int i = 0; i < property->info->count_enums; i++) {
        if (g_strcmp0(property->info->enums[i].name, name) == 0) {
            *value = property->info->enums[i].value;
            return !(property->info->flags & DRM_MODE_PROP_IMMUTABLE) || property->current == *value;
        }
    }
    return FALSE;
}

static gboolean
plane_has_format(const drmModePlane *plane, uint32_t format)
{
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == format)
            return TRUE;
    }
    return FALSE;
}

static gboolean
property_add(drmModeAtomicReq *request, uint32_t plane_id,
             const PlaneProperty *property, uint64_t desired)
{
    if (property->info->flags & DRM_MODE_PROP_IMMUTABLE)
        return desired == property->current;
    return drmModeAtomicAddProperty(request, plane_id, property->info->prop_id, desired) >= 0;
}

static gboolean
property_verify(int fd, uint32_t plane_id, const char *name, uint64_t desired)
{
    PlaneProperty property = { 0 };
    gboolean ok = property_get(fd, plane_id, name, &property) && property.current == desired;
    property_clear(&property);
    return ok;
}

/* Only advertise a plane after the kernel accepts and confirms alpha/zpos. */
static gboolean
configure_planes(int fd, uint32_t browser_id, uint32_t video_id,
                 PlaneProperty *browser_zpos, PlaneProperty *browser_blend,
                 uint64_t blend_value, uint64_t browser_top)
{
    PlaneProperty video_zpos = { 0 };
    uint64_t video_minimum, video_maximum;
    if (!property_get(fd, video_id, "zpos", &video_zpos) ||
        !property_limits(&video_zpos, &video_minimum, &video_maximum) || video_minimum >= browser_top) {
        property_clear(&video_zpos);
        return FALSE;
    }
    drmModeAtomicReq *request = drmModeAtomicAlloc();
    if (!request) {
        property_clear(&video_zpos);
        return FALSE;
    }
    gboolean ok = property_add(request, browser_id, browser_zpos, browser_top) &&
                  property_add(request, browser_id, browser_blend, blend_value) &&
                  property_add(request, video_id, &video_zpos, video_minimum);
    /* A global alpha property is optional; when present, avoid dimming the UI. */
    PlaneProperty alpha = { 0 };
    uint64_t alpha_minimum, alpha_maximum;
    gboolean have_alpha = property_get(fd, browser_id, "alpha", &alpha);
    if (have_alpha) {
        ok = ok && property_limits(&alpha, &alpha_minimum, &alpha_maximum) &&
             property_add(request, browser_id, &alpha, alpha_maximum);
    }
    if (ok && drmModeAtomicGetCursor(request) > 0)
        ok = drmModeAtomicCommit(fd, request, DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0;
    if (ok && drmModeAtomicGetCursor(request) > 0)
        ok = drmModeAtomicCommit(fd, request, 0, NULL) == 0;
    if (ok) {
        ok = property_verify(fd, browser_id, "zpos", browser_top) &&
             property_verify(fd, browser_id, "pixel blend mode", blend_value) &&
             property_verify(fd, video_id, "zpos", video_minimum);
        if (have_alpha)
            ok = ok && property_verify(fd, browser_id, "alpha", alpha_maximum);
    }
    if (ok)
        g_message("Holepunch planes: browser=%u zpos=%" G_GUINT64_FORMAT
                  " ARGB8888/Pre-multiplied, video=%u zpos=%" G_GUINT64_FORMAT " NV12",
                  browser_id, browser_top, video_id, video_minimum);
    else
        g_debug("Holepunch plane %u rejected: %s", video_id, g_strerror(errno));
    property_clear(&alpha);
    property_clear(&video_zpos);
    drmModeAtomicFree(request);
    return ok;
}

static gboolean
select_video_plane(int fd, uint32_t crtc_index, uint32_t browser_id,
                   uint32_t *video_id, GError **error)
{
    PlaneProperty browser_zpos = { 0 }, browser_blend = { 0 }, browser_type = { 0 };
    drmModePlane *browser_plane = drmModeGetPlane(fd, browser_id);
    uint64_t browser_minimum, browser_top, blend_value;
    gboolean browser_ok = browser_plane && crtc_index < 32 &&
        (browser_plane->possible_crtcs & (1u << crtc_index)) &&
        plane_has_format(browser_plane, DRM_FORMAT_ARGB8888) &&
        property_get(fd, browser_id, "type", &browser_type) && browser_type.current == DRM_PLANE_TYPE_PRIMARY &&
        property_get(fd, browser_id, "zpos", &browser_zpos) &&
        property_limits(&browser_zpos, &browser_minimum, &browser_top) &&
        property_get(fd, browser_id, "pixel blend mode", &browser_blend) &&
        property_enum(&browser_blend, "Pre-multiplied", &blend_value);
    if (browser_plane)
        drmModeFreePlane(browser_plane);
    property_clear(&browser_type);
    if (!browser_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Holepunch browser plane %u requires a compatible primary plane with ARGB8888, zpos and Pre-multiplied pixel blend mode", browser_id);
        property_clear(&browser_zpos);
        property_clear(&browser_blend);
        return FALSE;
    }

    uint32_t requested = 0;
    const char *override = g_getenv("SIGNAGE_VIDEO_PLANE_ID");
    if (override && *override) {
        char *end = NULL;
        guint64 parsed = g_ascii_strtoull(override, &end, 10);
        if (*override == '-' || !end || *end || parsed == 0 || parsed > G_MAXUINT32) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid SIGNAGE_VIDEO_PLANE_ID");
            property_clear(&browser_zpos);
            property_clear(&browser_blend);
            return FALSE;
        }
        requested = parsed;
    }

    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    gboolean found = FALSE;
    for (uint32_t i = 0; planes && i < planes->count_planes && !found; i++) {
        uint32_t id = planes->planes[i];
        if (id == browser_id || (requested && id != requested))
            continue;
        drmModePlane *plane = drmModeGetPlane(fd, id);
        PlaneProperty type = { 0 };
        gboolean candidate = plane && crtc_index < 32 &&
            (plane->possible_crtcs & (1u << crtc_index)) && plane_has_format(plane, DRM_FORMAT_NV12) &&
            property_get(fd, id, "type", &type) && type.current == DRM_PLANE_TYPE_OVERLAY;
        if (plane)
            drmModeFreePlane(plane);
        property_clear(&type);
        if (candidate && configure_planes(fd, browser_id, id, &browser_zpos, &browser_blend, blend_value, browser_top)) {
            *video_id = id;
            found = TRUE;
        }
    }
    if (planes)
        drmModeFreePlaneResources(planes);
    property_clear(&browser_zpos);
    property_clear(&browser_blend);
    if (!found)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "No compatible NV12 overlay%s with video zpos below browser plane %u; verify DE35 plane/blend support",
                    requested ? " matching SIGNAGE_VIDEO_PLANE_ID" : "", browser_id);
    return found;
}

static void
drop_client(BrokerClient *client, gboolean notify)
{
    if (!client->client)
        return;
    if (client->write_source) {
        g_source_destroy(client->write_source);
        g_source_unref(client->write_source);
        client->write_source = NULL;
    }
    g_queue_clear_full(&client->outgoing, g_free);
    if (client->client_source) {
        g_source_destroy(client->client_source);
        g_source_unref(client->client_source);
        client->client_source = NULL;
    }
    g_clear_object(&client->client);
    client->pending_length = 0;
    if (notify && broker.video_handler)
        { SignageVideoMessage gone = { .type = SIGNAGE_VIDEO_DISCONNECTED, .generation = client->generation };
        broker.video_handler(&gone, broker.video_data); }
}

static gboolean
client_readable(GSocket *socket, GIOCondition condition G_GNUC_UNUSED, gpointer data)
{
    BrokerClient *client = data;
    int fd = g_socket_get_fd(socket);
    for (;;) {
        ssize_t received = recv(fd, client->pending + client->pending_length,
                                sizeof(client->pending) - client->pending_length, MSG_DONTWAIT);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return G_SOURCE_CONTINUE;
        if (received <= 0) {
            g_message("Holepunch sink disconnected from the DRM broker%s%s", received < 0 ? ": " : "",
                      received < 0 ? g_strerror(errno) : "");
            /* The source is destroyed inside drop_client(); returning REMOVE is then a no-op. */
            drop_client(client, TRUE);
            return G_SOURCE_REMOVE;
        }
        client->pending_length += (gsize) received;
        if (client->pending_length < sizeof(client->pending))
            continue;
        SignageVideoMessage message;
        memcpy(&message, client->pending, sizeof(message));
        client->pending_length = 0;
        if (message.generation != client->generation ||
            (message.type != SIGNAGE_VIDEO_SHOW && message.type != SIGNAGE_VIDEO_HIDE && message.type != SIGNAGE_VIDEO_ENDPOINT) ||
            (message.type == SIGNAGE_VIDEO_SHOW && (!message.serial || message.serial <= client->last_serial))) {
            g_warning("Holepunch sink sent unknown message type %u; disconnecting it", message.type);
            drop_client(client, TRUE);
            return G_SOURCE_REMOVE;
        }
        if (message.type == SIGNAGE_VIDEO_SHOW) client->last_serial = message.serial;
        if (broker.video_handler)
            broker.video_handler(&message, broker.video_data);
        if (!client->client)
            return G_SOURCE_REMOVE;
    }
}

static gboolean
accept_client(GSocketConnection *connection, uint64_t requested)
{
    int socket_fd = g_socket_get_fd(g_socket_connection_get_socket(connection));
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0 || peer.uid != geteuid()) {
        g_warning("Rejected DRM broker peer with a different effective UID");
        return TRUE;
    }
    /* First record binds this connection to an explicitly reserved generation.
     * No second peer may claim active or displace a retiring owner. */
    BrokerClient *client = NULL;
    for (unsigned i = 0; i < 2; i++) {
        if (requested && requested == broker.reserved && broker.clients[i].generation == requested && !broker.clients[i].client)
            client = &broker.clients[i];
        if (!requested && !broker.active && !broker.clients[i].generation && broker.generation != G_MAXUINT64) {
            client = &broker.clients[i]; break;
        }
    }
    if (!client) return TRUE;
    if (!requested) { client->generation = ++broker.generation; broker.active = client->generation; }
    int fd_copy = fcntl(broker.fd, F_DUPFD_CLOEXEC, 3);
    if (fd_copy < 0) {
        if (!requested) { client->generation=0; broker.active=0; }
        return TRUE;
    }
    g_autofree char *metadata = g_strdup_printf("%s,\"protocol\":2,\"generation\":%" G_GUINT64_FORMAT ",\"standby\":%s,\"video_frames\":%s}\n", broker.metadata,
        client->generation, requested ? "true" : "false", broker.video_handler ? "true" : "false");
    struct iovec payload = { .iov_base = metadata, .iov_len = strlen(metadata) };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = { 0 };
    struct msghdr message = {
        .msg_iov = &payload, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
    };
    struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
    rights->cmsg_level = SOL_SOCKET;
    rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(rights), &fd_copy, sizeof(fd_copy));
    ssize_t sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd_copy);
    if (sent != (ssize_t) payload.iov_len) {
        g_warning("DRM broker metadata transfer failed: %s", g_strerror(errno));
        if (!requested) { client->generation=0; broker.active=0; }
        return TRUE;
    }
    if (!broker.video_handler) {
        client->generation=0; broker.active=0;
        return TRUE; /* legacy kmssink keeps its exclusive file lock */
    }
    GSocket *socket = g_socket_connection_get_socket(connection);
    g_socket_set_blocking(socket, FALSE);
    client->client = g_object_ref(connection);
    client->client_source = g_socket_create_source(socket, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback(client->client_source, (GSourceFunc) G_CALLBACK(client_readable), client, NULL);
    g_source_attach(client->client_source, NULL);
    return TRUE;
}

/* Handshake has its own bounded parser/source. Never block the active committer
 * on a connecting peer (including a half-written reservation token). */
static unsigned pending_hellos;
typedef struct { GSocketConnection *connection; uint64_t requested; size_t offset; gint64 deadline; GSource *source; guint timeout; } ClientHello;
static void hello_free(gpointer data) {
    ClientHello *hello=data;
    if (hello->timeout) g_source_remove(hello->timeout);
    pending_hellos--; g_object_unref(hello->connection); g_free(hello);
}
static gboolean hello_timeout(gpointer data) {
    ClientHello *hello=data; hello->timeout=0;
    g_source_destroy(hello->source); return G_SOURCE_REMOVE;
}
static gboolean hello_readable(GSocket *socket, GIOCondition condition G_GNUC_UNUSED, gpointer data) {
    ClientHello *hello=data;
    ssize_t n=recv(g_socket_get_fd(socket),(char *)&hello->requested+hello->offset,sizeof(hello->requested)-hello->offset,MSG_DONTWAIT);
    if (n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) return G_SOURCE_CONTINUE;
    if (n<=0) return G_SOURCE_REMOVE;
    hello->offset+=(size_t)n;
    if (hello->offset<sizeof(hello->requested)) return G_SOURCE_CONTINUE;
    accept_client(hello->connection,hello->requested);
    return G_SOURCE_REMOVE;
}
static gboolean
on_incoming(GSocketService *service G_GNUC_UNUSED, GSocketConnection *connection,
            GObject *source G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    /* At most two unregistered handshakes, each expires in one second. */
    if (pending_hellos>=2) return TRUE;
    pending_hellos++;
    ClientHello *hello=g_new0(ClientHello,1);
    hello->connection=g_object_ref(connection);
    GSocket *socket=g_socket_connection_get_socket(connection);
    g_socket_set_blocking(socket,FALSE);
    hello->source=g_socket_create_source(socket,G_IO_IN|G_IO_HUP|G_IO_ERR,NULL);
    g_source_set_callback(hello->source,(GSourceFunc)G_CALLBACK(hello_readable),hello,hello_free);
    hello->timeout=g_timeout_add(1000,hello_timeout,hello);
    g_source_attach(hello->source,NULL); g_source_unref(hello->source);
    return TRUE;
}

guint
signage_drm_broker_video_plane(void)
{
    return broker.service ? broker.video_plane : 0;
}

void
signage_drm_broker_set_video_handler(SignageVideoHandler handler, gpointer data)
{
    if (!handler)
        for (unsigned i = 0; i < 2; i++) drop_client(&broker.clients[i], FALSE);
    broker.video_handler = handler;
    broker.video_data = data;
}

gboolean
signage_drm_broker_video_handler_active(void)
{
    return broker.video_handler != NULL;
}

static gboolean video_writable(GSocket *socket, GIOCondition condition, gpointer data);

static gboolean video_tx_drain(BrokerClient *client)
{
    while (client->client && !g_queue_is_empty(&client->outgoing)) {
        VideoTx *tx = g_queue_peek_head(&client->outgoing);
        int fd = g_socket_get_fd(g_socket_connection_get_socket(client->client));
        ssize_t n = send(fd, (guint8 *)&tx->message + tx->offset,
                         sizeof(tx->message) - tx->offset, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return TRUE;
        if (n <= 0) { drop_client(client, TRUE); return FALSE; }
        tx->offset += (gsize)n;
        if (tx->offset == sizeof(tx->message)) g_free(g_queue_pop_head(&client->outgoing));
    }
    return TRUE;
}

static gboolean video_writable(GSocket *socket G_GNUC_UNUSED, GIOCondition condition G_GNUC_UNUSED, gpointer data)
{
    BrokerClient *client = data;
    if (!video_tx_drain(client)) return G_SOURCE_REMOVE;
    if (!g_queue_is_empty(&client->outgoing)) return G_SOURCE_CONTINUE;
    GSource *source = client->write_source;
    client->write_source = NULL;
    g_source_unref(source);
    return G_SOURCE_REMOVE;
}

void
signage_drm_broker_video_ack(guint64 generation, uint32_t type, uint32_t fb, uint32_t serial)
{
    BrokerClient *client = NULL;
    for (unsigned i = 0; i < 2; i++)
        if (broker.clients[i].generation == generation) client = &broker.clients[i];
    if (!client) return;
    if (!client->client || !fb)
        return;
    /* Never drop half a stream record. Congestion is a normal disconnect/hide,
     * not a false retirement. Bound memory even for a peer that never reads. */
    if (client->outgoing.length >= 64) { drop_client(client, TRUE); return; }
    VideoTx *tx = g_new0(VideoTx, 1);
    tx->message = (SignageVideoMessage){ .type = type, .generation = generation, .fb = fb, .serial = serial };
    g_queue_push_tail(&client->outgoing, tx);
    if (!video_tx_drain(client) || g_queue_is_empty(&client->outgoing) || client->write_source) return;
    GSocket *socket = g_socket_connection_get_socket(client->client);
    client->write_source = g_socket_create_source(socket, G_IO_OUT | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback(client->write_source, (GSourceFunc)G_CALLBACK(video_writable), client, NULL);
    g_source_attach(client->write_source, NULL);
}

void signage_drm_broker_video_released(uint32_t fb, uint32_t serial)
{
    signage_drm_broker_video_ack(broker.active, SIGNAGE_VIDEO_RELEASED, fb, serial);
}

void signage_drm_broker_video_presented(uint32_t fb, uint32_t serial)
{
    signage_drm_broker_video_ack(broker.active, SIGNAGE_VIDEO_PRESENTED, fb, serial);
}

void signage_drm_broker_video_cancelled(uint32_t fb, uint32_t serial)
{
    signage_drm_broker_video_ack(broker.active, SIGNAGE_VIDEO_CANCELLED, fb, serial);
}

guint64 signage_drm_broker_video_generation(void) { return broker.active; }
void signage_drm_broker_video_disconnect_generation(guint64 generation) {
    for (unsigned i = 0; i < 2; i++) if (broker.clients[i].generation == generation) drop_client(&broker.clients[i], TRUE);
}
void signage_drm_broker_video_disconnect(void) { signage_drm_broker_video_disconnect_generation(broker.active); }
gboolean signage_drm_broker_video_tx_idle(guint64 generation) {
    for (unsigned i=0;i<2;i++) if (broker.clients[i].generation==generation) return g_queue_is_empty(&broker.clients[i].outgoing);
    return TRUE;
}
gboolean signage_drm_broker_video_connected(guint64 generation) {
    for (unsigned i=0;i<2;i++) if (broker.clients[i].generation==generation) return broker.clients[i].client!=NULL;
    return FALSE;
}
void signage_drm_broker_video_retired(guint64 generation) {
    for (unsigned i = 0; i < 2; i++) if (broker.clients[i].generation == generation) {
        drop_client(&broker.clients[i], FALSE);
        memset(&broker.clients[i], 0, sizeof(BrokerClient));
    }
    if (broker.active == generation) broker.active = 0;
    if (broker.reserved == generation) broker.reserved = 0;
}
void signage_drm_broker_video_activate(guint64 generation) { broker.active = generation; broker.reserved = 0; }
guint64 signage_drm_broker_video_reserve(void) {
    if (!broker.active || broker.reserved || broker.generation == G_MAXUINT64) return 0;
    for (unsigned i = 0; i < 2; i++) if (!broker.clients[i].generation) {
        broker.reserved = ++broker.generation;
        broker.clients[i].generation = broker.reserved;
        return broker.reserved;
    }
    return 0;
}

static gboolean
prepare_socket(GError **error)
{
    if (g_mkdir_with_parents(BROKER_DIR, 0700) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create %s: %s", BROKER_DIR, g_strerror(errno));
        return FALSE;
    }
    struct stat st;
    if (lstat(BROKER_DIR, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "DRM broker directory must be owned by the browser and not writable by group/others");
        return FALSE;
    }
    if (lstat(BROKER_PATH, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unsafe pre-existing DRM broker path");
            return FALSE;
        }
        /* Refuse to replace another running browser's broker. */
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (probe < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot inspect existing DRM broker: %s", g_strerror(errno));
            return FALSE;
        }
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        g_strlcpy(address.sun_path, BROKER_PATH, sizeof(address.sun_path));
        int connected = connect(probe, (struct sockaddr *) &address, sizeof(address));
        int probe_error = errno;
        close(probe);
        if (connected == 0 || probe_error == EINPROGRESS || probe_error == EAGAIN) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE, "Another browser already owns the DRM broker socket");
            return FALSE;
        }
        if (probe_error != ECONNREFUSED && probe_error != ENOENT) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(probe_error), "Cannot inspect existing DRM broker: %s", g_strerror(probe_error));
            return FALSE;
        }
        if (probe_error == ENOENT)
            return TRUE;
        if (unlink(BROKER_PATH) < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot remove stale DRM broker socket: %s", g_strerror(errno));
            return FALSE;
        }
    } else if (errno != ENOENT) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot inspect DRM broker socket: %s", g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

gboolean
signage_drm_broker_start(int fd, guint connector_id, guint crtc_id,
                       guint crtc_index, guint browser_plane_id,
                       guint ui_width, guint ui_height, guint mode_width, guint mode_height, GError **error)
{
    if (!signage_drm_holepunch_enabled())
        return TRUE;
    if (broker.service || fd < 0 || drmIsMaster(fd) != 1 || !ui_width || !ui_height || !mode_width || !mode_height) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Holepunch requires the browser's existing DRM master descriptor and valid mode dimensions");
        return FALSE;
    }
    uint32_t video_id;
    if (!select_video_plane(fd, crtc_index, browser_plane_id, &video_id, error) || !prepare_socket(error))
        return FALSE;
    broker.fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (broker.fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot duplicate browser DRM fd: %s", g_strerror(errno));
        return FALSE;
    }
    broker.video_plane = video_id;
    broker.metadata = g_strdup_printf("{\"connector_id\":%u,\"crtc_id\":%u,\"crtc_index\":%u,"
                                    "\"browser_plane_id\":%u,\"video_plane_id\":%u,\"width\":%u,\"height\":%u,\"mode_width\":%u,\"mode_height\":%u,\"ui_width\":%u,\"ui_height\":%u,\"blend_configured\":true",
                                    connector_id, crtc_id, crtc_index, browser_plane_id, video_id, mode_width, mode_height,
                                    mode_width, mode_height, ui_width, ui_height);
    broker.service = g_socket_service_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(BROKER_PATH);
    /* Bind with owner-only access from the outset, before starting service. */
    mode_t previous_umask = umask(0077);
    gboolean bound = g_socket_listener_add_address(G_SOCKET_LISTENER(broker.service), address,
                                                   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                                   NULL, NULL, error);
    umask(previous_umask);
    if (!bound) {
        signage_drm_broker_stop();
        return FALSE;
    }
    struct stat st;
    if (lstat(BROKER_PATH, &st) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot secure DRM broker socket: %s", g_strerror(errno));
        signage_drm_broker_stop();
        return FALSE;
    }
    broker.socket_inode = st.st_ino;
    broker.socket_device = st.st_dev;
    if (chmod(BROKER_PATH, 0600) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot secure DRM broker socket: %s", g_strerror(errno));
        signage_drm_broker_stop();
        return FALSE;
    }
    g_signal_connect(broker.service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(broker.service);
    g_message("Browser DRM broker ready at %s: %s}", BROKER_PATH, broker.metadata);
    return TRUE;
}

void
signage_drm_broker_stop(void)
{
    for (unsigned i = 0; i < 2; i++) { drop_client(&broker.clients[i], FALSE); memset(&broker.clients[i], 0, sizeof(BrokerClient)); }
    broker.video_handler = NULL;
    broker.video_data = NULL;
    broker.video_plane = 0;
    if (broker.service) {
        g_socket_service_stop(broker.service);
        g_socket_listener_close(G_SOCKET_LISTENER(broker.service));
        g_clear_object(&broker.service);
    }
    struct stat st;
    if (broker.socket_inode && lstat(BROKER_PATH, &st) == 0 &&
        st.st_ino == broker.socket_inode && st.st_dev == broker.socket_device)
        unlink(BROKER_PATH);
    broker.socket_inode = 0;
    broker.socket_device = 0;
    if (broker.fd >= 0)
        close(broker.fd);
    broker.fd = -1;
    g_clear_pointer(&broker.metadata, g_free);
}
