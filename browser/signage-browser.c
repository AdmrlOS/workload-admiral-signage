/* SPDX-License-Identifier: MIT
 * Admiral's small WPE WebKit shell. DMA-BUF frames are scanned out directly;
 * there is no GLES texture copy that could discard holepunch alpha.
 * Written against the public libwpe, WPEBackend-fdo, WebKit and libdrm APIs.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glib-unix.h>
#include <inttypes.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <wpe/webkit.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm.h>
#include "input.h"
#include "../native/drm-broker.h"

#define VERSION "0.1.0"

typedef struct {
    struct wl_resource *resource;
    uint32_t fb;
    uint32_t handles[4];
    uint8_t n_planes;
} Frame;

typedef struct {
    int fd;
    int render_fd;
    char *device;
    uint32_t connector, crtc, crtc_index, primary;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    uint32_t connector_crtc, crtc_mode, crtc_active;
    uint32_t plane_fb, plane_crtc, plane_src_x, plane_src_y, plane_src_w, plane_src_h;
    uint32_t plane_x, plane_y, plane_w, plane_h;
    struct gbm_device *gbm;
    EGLDisplay egl;
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    WebKitWebView *view;
    SignageInput *input;
    GMainLoop *loop;
    GHashTable *handles;
    Frame *current, *pending, *queued;
    gint64 submitted_at;
    guint fd_source, watchdog_source, sigterm_source, sigint_source;
    guint64 presented;
    gboolean modeset;
    gboolean holepunch;
    gboolean broker_started;
    int exit_code;
    const char *uri;
    char *failure;
    char *holepunch_error;
} Browser;

static unsigned mode_refresh(const drmModeModeInfo *mode)
{
    if (mode->vrefresh) return mode->vrefresh;
    if (!mode->htotal || !mode->vtotal) return 0;
    uint64_t numerator = (uint64_t)mode->clock * 1000;
    uint64_t denominator = (uint64_t)mode->htotal * mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) numerator *= 2;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) denominator *= 2;
    if (mode->vscan > 1) denominator *= mode->vscan;
    return (unsigned)((numerator + denominator / 2) / denominator);
}

static void write_status(Browser *browser)
{
    const char *path = g_getenv("SIGNAGE_BROWSER_STATUS");
    if (!path || !*path) path = "/run/admiral-signage/browser-status.json";
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
#define JSON_STRING(key, value) do { json_builder_set_member_name(builder, key); json_builder_add_string_value(builder, (value) ? (value) : ""); } while (0)
#define JSON_INT(key, value) do { json_builder_set_member_name(builder, key); json_builder_add_int_value(builder, (gint64)(value)); } while (0)
#define JSON_BOOL(key, value) do { json_builder_set_member_name(builder, key); json_builder_add_boolean_value(builder, value); } while (0)
    JSON_STRING("browser", "signage-browser");
    JSON_STRING("version", VERSION);
    JSON_STRING("device", browser->device);
    JSON_STRING("uri", browser->uri);
    JSON_STRING("error", browser->failure);
    JSON_STRING("holepunch_error", browser->holepunch_error);
    JSON_INT("connector_id", browser->connector);
    JSON_INT("crtc_id", browser->crtc);
    JSON_INT("primary_plane_id", browser->primary);
    JSON_INT("width", browser->mode.hdisplay);
    JSON_INT("height", browser->mode.vdisplay);
    JSON_INT("refresh", mode_refresh(&browser->mode));
    JSON_INT("presented_frames", browser->presented);
    JSON_BOOL("direct_scanout", browser->presented > 0);
    JSON_BOOL("alpha_preserved", browser->presented > 0 && browser->holepunch);
    JSON_BOOL("broker_ready", browser->broker_started);
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize length;
    char *data = json_generator_to_data(generator, &length);
    char *temporary = g_strconcat(path, ".tmp", NULL);
    if (g_file_set_contents(temporary, data, length, NULL)) {
        if (rename(temporary, path) != 0) unlink(temporary);
    }
    g_free(temporary);
    g_free(data);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);
}

static void fail(Browser *browser, const char *message)
{
    if (!browser->failure) browser->failure = g_strdup(message);
    browser->exit_code = 1;
    g_printerr("signage-browser: %s\n", message);
    write_status(browser);
    if (browser->loop) g_main_loop_quit(browser->loop);
}

static uint32_t property_id(int fd, uint32_t object, uint32_t type, const char *name, uint64_t *value)
{
    drmModeObjectProperties *properties = drmModeObjectGetProperties(fd, object, type);
    uint32_t found = 0;
    if (!properties) return 0;
    for (uint32_t i = 0; i < properties->count_props; i++) {
        drmModePropertyRes *property = drmModeGetProperty(fd, properties->props[i]);
        if (property && strcmp(property->name, name) == 0) {
            found = property->prop_id;
            if (value) *value = properties->prop_values[i];
        }
        if (property) drmModeFreeProperty(property);
        if (found) break;
    }
    drmModeFreeObjectProperties(properties);
    return found;
}

static gboolean supports_format(drmModePlane *plane, uint32_t format)
{
    for (uint32_t i = 0; i < plane->count_formats; i++)
        if (plane->formats[i] == format) return TRUE;
    return FALSE;
}

static gboolean parse_mode(const char *text, unsigned *width, unsigned *height, unsigned *refresh)
{
    char trailing;
    if (!text || !*text) return TRUE;
    if (sscanf(text, "%ux%u@%u%c", width, height, refresh, &trailing) == 3)
        return *width > 0 && *height > 0 && *refresh > 0;
    *refresh = 0;
    return sscanf(text, "%ux%u%c", width, height, &trailing) == 2 && *width > 0 && *height > 0;
}

static gboolean select_output(Browser *browser, drmModeRes *resources)
{
    const char *wanted_connector = g_getenv("SIGNAGE_DRM_CONNECTOR");
    const char *wanted_primary = g_getenv("SIGNAGE_DRM_PRIMARY_PLANE");
    const char *wanted_mode = g_getenv("SIGNAGE_DRM_MODE");
    unsigned width = 0, height = 0, refresh = 0;
    if (!parse_mode(wanted_mode, &width, &height, &refresh)) {
        g_printerr("Invalid SIGNAGE_DRM_MODE; expected WIDTHxHEIGHT[@REFRESH]\n");
        return FALSE;
    }
    drmModePlaneRes *planes = drmModeGetPlaneResources(browser->fd);
    if (!planes) return FALSE;
    gboolean selected = FALSE;
    for (int ci = 0; ci < resources->count_connectors && !selected; ci++) {
        drmModeConnector *connector = drmModeGetConnector(browser->fd, resources->connectors[ci]);
        if (!connector) continue;
        char connector_name[80], connector_id[32];
        snprintf(connector_name, sizeof connector_name, "%s-%u", drmModeGetConnectorTypeName(connector->connector_type), connector->connector_type_id);
        snprintf(connector_id, sizeof connector_id, "%u", connector->connector_id);
        if (connector->connection != DRM_MODE_CONNECTED || !connector->count_modes ||
            (wanted_connector && *wanted_connector && strcmp(wanted_connector, connector_name) && strcmp(wanted_connector, connector_id))) {
            drmModeFreeConnector(connector);
            continue;
        }
        int mode_index = -1;
        for (int mi = 0; mi < connector->count_modes; mi++) {
            drmModeModeInfo *mode = &connector->modes[mi];
            if (width && (mode->hdisplay != width || mode->vdisplay != height || (refresh && (unsigned)mode_refresh(mode) != refresh))) continue;
            if (mode_index < 0 || (mode->type & DRM_MODE_TYPE_PREFERRED)) mode_index = mi;
            if (mode->type & DRM_MODE_TYPE_PREFERRED) break;
        }
        if (mode_index < 0) {
            g_printerr("Connector %s has no matching mode\n", connector_name);
            drmModeFreeConnector(connector);
            continue;
        }
        uint32_t possible_crtcs = 0;
        uint32_t preferred_crtc = 0;
        for (int ei = 0; ei < connector->count_encoders; ei++) {
            drmModeEncoder *encoder = drmModeGetEncoder(browser->fd, connector->encoders[ei]);
            if (!encoder) continue;
            possible_crtcs |= encoder->possible_crtcs;
            if (encoder->encoder_id == connector->encoder_id) preferred_crtc = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
        for (int pass = 0; pass < 2 && !selected; pass++) {
            for (int ri = 0; ri < resources->count_crtcs && ri < 32 && !selected; ri++) {
                uint32_t crtc = resources->crtcs[ri];
                if (!(possible_crtcs & (1u << ri)) || (pass == 0 && crtc != preferred_crtc) || (pass == 1 && crtc == preferred_crtc)) continue;
                for (uint32_t pi = 0; pi < planes->count_planes && !selected; pi++) {
                    drmModePlane *plane = drmModeGetPlane(browser->fd, planes->planes[pi]);
                    if (!plane) continue;
                    uint64_t type = 0;
                    char id[32];
                    snprintf(id, sizeof id, "%u", plane->plane_id);
                    gboolean match = (plane->possible_crtcs & (1u << ri)) &&
                        property_id(browser->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) &&
                        type == DRM_PLANE_TYPE_PRIMARY && supports_format(plane, DRM_FORMAT_ARGB8888) &&
                        (!wanted_primary || !*wanted_primary || !strcmp(wanted_primary, id));
                    if (match) {
                        browser->connector = connector->connector_id;
                        browser->crtc = crtc;
                        browser->crtc_index = ri;
                        browser->primary = plane->plane_id;
                        browser->mode = connector->modes[mode_index];
                        selected = TRUE;
                        g_message("Selected %s connector=%u CRTC=%u primary=%u mode=%ux%u@%u", connector_name, browser->connector, crtc, browser->primary, browser->mode.hdisplay, browser->mode.vdisplay, mode_refresh(&browser->mode));
                    }
                    drmModeFreePlane(plane);
                }
            }
        }
        drmModeFreeConnector(connector);
    }
    drmModeFreePlaneResources(planes);
    return selected;
}

static gboolean try_device(Browser *browser, const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return FALSE;
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0 ||
        (!drmIsMaster(fd) && drmSetMaster(fd) != 0)) {
        g_printerr("Cannot acquire atomic DRM master on %s: %s\n", path, g_strerror(errno));
        close(fd);
        return FALSE;
    }
    browser->fd = fd;
    drmModeRes *resources = drmModeGetResources(fd);
    gboolean selected = resources && select_output(browser, resources);
    if (resources) drmModeFreeResources(resources);
    if (!selected) {
        drmDropMaster(fd);
        close(fd);
        browser->fd = -1;
        return FALSE;
    }
    browser->device = g_strdup(path);
    return TRUE;
}

static gboolean open_display(Browser *browser)
{
    const char *device = g_getenv("SIGNAGE_DRM_DEVICE");
    if (device && *device) {
        if (!try_device(browser, device)) return FALSE;
    } else {
        for (unsigned i = 0; i < 16 && browser->fd < 0; i++) {
            char path[64];
            snprintf(path, sizeof path, "/dev/dri/card%u", i);
            try_device(browser, path);
        }
        if (browser->fd < 0) return FALSE;
    }
#define PROPERTY(field, object, type, name) do { browser->field = property_id(browser->fd, object, type, name, NULL); if (!browser->field) { g_printerr("Required atomic property %s missing\n", name); return FALSE; } } while (0)
    PROPERTY(connector_crtc, browser->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    PROPERTY(crtc_mode, browser->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    PROPERTY(crtc_active, browser->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    PROPERTY(plane_fb, browser->primary, DRM_MODE_OBJECT_PLANE, "FB_ID");
    PROPERTY(plane_crtc, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    PROPERTY(plane_src_x, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_X");
    PROPERTY(plane_src_y, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    PROPERTY(plane_src_w, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_W");
    PROPERTY(plane_src_h, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_H");
    PROPERTY(plane_x, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    PROPERTY(plane_y, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    PROPERTY(plane_w, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    PROPERTY(plane_h, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    if (drmModeCreatePropertyBlob(browser->fd, &browser->mode, sizeof browser->mode, &browser->mode_blob) != 0) return FALSE;
    return TRUE;
}

static void handle_ref(Browser *browser, uint32_t handle)
{
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(browser->handles, GUINT_TO_POINTER(handle)));
    g_hash_table_insert(browser->handles, GUINT_TO_POINTER(handle), GUINT_TO_POINTER(count + 1));
}

static void handle_unref(Browser *browser, uint32_t handle)
{
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(browser->handles, GUINT_TO_POINTER(handle)));
    if (count > 1) g_hash_table_insert(browser->handles, GUINT_TO_POINTER(handle), GUINT_TO_POINTER(count - 1));
    else {
        g_hash_table_remove(browser->handles, GUINT_TO_POINTER(handle));
        struct drm_gem_close close_request = { .handle = handle };
        drmIoctl(browser->fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    }
}

static void release_frame(Browser *browser, Frame *frame)
{
    if (!frame) return;
    if (frame->fb) drmModeRmFB(browser->fd, frame->fb);
    for (unsigned i = 0; i < frame->n_planes; i++) {
        gboolean unique = TRUE;
        for (unsigned j = 0; j < i; j++) if (frame->handles[j] == frame->handles[i]) unique = FALSE;
        if (unique && frame->handles[i]) handle_unref(browser, frame->handles[i]);
    }
    if (browser->exportable && frame->resource)
        wpe_view_backend_exportable_fdo_dispatch_release_buffer(browser->exportable, frame->resource);
    g_free(frame);
}

static gboolean submit_frame(Browser *browser, Frame *frame)
{
    drmModeAtomicReq *request = drmModeAtomicAlloc();
    if (!request) return FALSE;
    gboolean valid = TRUE;
#define ADD(object, property, value) do { if (drmModeAtomicAddProperty(request, object, property, value) < 0) valid = FALSE; } while (0)
    if (!browser->modeset) {
        ADD(browser->connector, browser->connector_crtc, browser->crtc);
        ADD(browser->crtc, browser->crtc_mode, browser->mode_blob);
        ADD(browser->crtc, browser->crtc_active, 1);
    }
    ADD(browser->primary, browser->plane_fb, frame->fb);
    ADD(browser->primary, browser->plane_crtc, browser->crtc);
    ADD(browser->primary, browser->plane_src_x, 0);
    ADD(browser->primary, browser->plane_src_y, 0);
    ADD(browser->primary, browser->plane_src_w, (uint64_t)browser->mode.hdisplay << 16);
    ADD(browser->primary, browser->plane_src_h, (uint64_t)browser->mode.vdisplay << 16);
    ADD(browser->primary, browser->plane_x, 0);
    ADD(browser->primary, browser->plane_y, 0);
    ADD(browser->primary, browser->plane_w, browser->mode.hdisplay);
    ADD(browser->primary, browser->plane_h, browser->mode.vdisplay);
    uint32_t mode_flag = browser->modeset ? 0 : DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (valid && !browser->modeset)
        valid = drmModeAtomicCommit(browser->fd, request, mode_flag | DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0;
    if (valid)
        valid = drmModeAtomicCommit(browser->fd, request, mode_flag | DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, browser) == 0;
    int saved_errno = errno;
    drmModeAtomicFree(request);
    if (!valid) {
        char *message = g_strdup_printf("Atomic browser plane commit failed: %s; inspect DE35 format/modifier/alpha support", g_strerror(saved_errno));
        fail(browser, message);
        g_free(message);
        return FALSE;
    }
    browser->modeset = TRUE;
    browser->pending = frame;
    browser->submitted_at = g_get_monotonic_time();
    return TRUE;
}

/* Activate the selected CRTC before WebKit can preroll its video sink. */
static gboolean initial_scanout(Browser *browser)
{
    struct drm_mode_create_dumb create = {
        .width = browser->mode.hdisplay, .height = browser->mode.vdisplay, .bpp = 32
    };
    if (drmIoctl(browser->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) return FALSE;
    Frame *frame = g_new0(Frame, 1);
    frame->n_planes = 1;
    frame->handles[0] = create.handle;
    handle_ref(browser, create.handle);
    struct drm_mode_map_dumb map = { .handle = create.handle };
    gboolean valid = drmIoctl(browser->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0;
    if (valid) {
        uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, browser->fd, map.offset);
        valid = pixels != MAP_FAILED;
        if (valid) {
            for (uint64_t i = 0; i < create.size / 4; i++) pixels[i] = 0xff09131c;
            munmap(pixels, create.size);
        }
    }
    uint32_t strides[4] = { create.pitch, 0, 0, 0 }, offsets[4] = { 0, 0, 0, 0 };
    if (valid) valid = drmModeAddFB2(browser->fd, browser->mode.hdisplay, browser->mode.vdisplay,
                                    DRM_FORMAT_ARGB8888, frame->handles, strides, offsets, &frame->fb, 0) == 0;
    drmModeAtomicReq *request = valid ? drmModeAtomicAlloc() : NULL;
    if (!request) valid = FALSE;
    if (valid) {
        ADD(browser->connector, browser->connector_crtc, browser->crtc);
        ADD(browser->crtc, browser->crtc_mode, browser->mode_blob);
        ADD(browser->crtc, browser->crtc_active, 1);
        ADD(browser->primary, browser->plane_fb, frame->fb);
        ADD(browser->primary, browser->plane_crtc, browser->crtc);
        ADD(browser->primary, browser->plane_src_x, 0);
        ADD(browser->primary, browser->plane_src_y, 0);
        ADD(browser->primary, browser->plane_src_w, (uint64_t)browser->mode.hdisplay << 16);
        ADD(browser->primary, browser->plane_src_h, (uint64_t)browser->mode.vdisplay << 16);
        ADD(browser->primary, browser->plane_x, 0);
        ADD(browser->primary, browser->plane_y, 0);
        ADD(browser->primary, browser->plane_w, browser->mode.hdisplay);
        ADD(browser->primary, browser->plane_h, browser->mode.vdisplay);
        if (valid) valid = drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0;
        if (valid) valid = drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
    }
    if (request) drmModeAtomicFree(request);
    if (!valid) { release_frame(browser, frame); return FALSE; }
    browser->current = frame;
    browser->modeset = TRUE;
    g_message("Initial ARGB browser mode active before video broker starts");
    return TRUE;
}

static void frame_presented(int fd, unsigned sequence, unsigned seconds, unsigned microseconds, void *data)
{
    (void)fd; (void)sequence; (void)seconds; (void)microseconds;
    Browser *browser = data;
    if (!browser->pending) return;
    Frame *retired = browser->current;
    browser->current = browser->pending;
    browser->pending = NULL;
    release_frame(browser, retired);
    browser->presented++;
    if (browser->presented == 1)
        g_message("First ARGB DMA-BUF browser frame scanned out on plane %u; alpha preserved without GLES copy", browser->primary);
    if (browser->presented == 1 || browser->presented % 120 == 0) write_status(browser);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(browser->exportable);
    if (browser->queued) {
        Frame *queued = browser->queued;
        browser->queued = NULL;
        if (!submit_frame(browser, queued)) release_frame(browser, queued);
    }
}

static gboolean drm_ready(gint fd, GIOCondition condition, gpointer data)
{
    Browser *browser = data;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        fail(browser, "DRM device connection lost");
        browser->fd_source = 0;
        return G_SOURCE_REMOVE;
    }
    drmEventContext context = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = frame_presented };
    if (drmHandleEvent(fd, &context) != 0 && errno != EAGAIN && errno != EINTR) {
        fail(browser, "Cannot read DRM pageflip completion events");
        browser->fd_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void export_dmabuf(void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource)
{
    Browser *browser = data;
    Frame *frame = g_new0(Frame, 1);
    frame->resource = resource->buffer_resource;
    frame->n_planes = MIN(resource->n_planes, 4);
    gboolean valid = resource->width == browser->mode.hdisplay && resource->height == browser->mode.vdisplay &&
        resource->n_planes > 0 && resource->n_planes <= 4 && resource->format == DRM_FORMAT_ARGB8888;
    gboolean explicit_modifiers = FALSE, all_linear_or_implicit = TRUE;
    for (unsigned i = 0; valid && i < resource->n_planes; i++) {
        if (drmPrimeFDToHandle(browser->fd, resource->fds[i], &frame->handles[i]) != 0) { valid = FALSE; break; }
        gboolean unique = TRUE;
        for (unsigned j = 0; j < i; j++) if (frame->handles[j] == frame->handles[i]) unique = FALSE;
        if (unique) handle_ref(browser, frame->handles[i]);
        if (resource->modifiers[i] != DRM_FORMAT_MOD_INVALID) explicit_modifiers = TRUE;
        if (resource->modifiers[i] != DRM_FORMAT_MOD_INVALID && resource->modifiers[i] != DRM_FORMAT_MOD_LINEAR) all_linear_or_implicit = FALSE;
    }
    /* Only a fully imported, validated buffer may reach the linear fallback. */
    if (valid) {
        if (explicit_modifiers) {
            valid = drmModeAddFB2WithModifiers(browser->fd, resource->width, resource->height, resource->format,
                frame->handles, resource->strides, resource->offsets, resource->modifiers, &frame->fb, DRM_MODE_FB_MODIFIERS) == 0;
            if (!valid && all_linear_or_implicit)
                valid = drmModeAddFB2(browser->fd, resource->width, resource->height, resource->format,
                    frame->handles, resource->strides, resource->offsets, &frame->fb, 0) == 0;
        } else {
            valid = drmModeAddFB2(browser->fd, resource->width, resource->height, resource->format,
                frame->handles, resource->strides, resource->offsets, &frame->fb, 0) == 0;
        }
    }
    if (!valid) {
        char *message = g_strdup_printf("Cannot scan out WPE DMA-BUF: size=%ux%u format=0x%08x planes=%u modifier=0x%016" PRIx64 " (%s). ARGB8888 alpha and a display-compatible modifier are required", resource->width, resource->height, resource->format, resource->n_planes, resource->modifiers[0], g_strerror(errno));
        release_frame(browser, frame);
        fail(browser, message);
        g_free(message);
        return;
    }
    if (browser->pending) {
        release_frame(browser, browser->queued);
        browser->queued = frame;
    } else if (!submit_frame(browser, frame)) release_frame(browser, frame);
}

static void export_legacy(void *data, struct wl_resource *resource)
{
    Browser *browser = data;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(browser->exportable, resource);
    fail(browser, "WPE exported a legacy EGL Wayland buffer instead of DMA-BUF; direct alpha-preserving scanout is unavailable");
}

static void export_shm(void *data, struct wpe_fdo_shm_exported_buffer *buffer)
{
    Browser *browser = data;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(browser->exportable, buffer);
    fail(browser, "WPE exported a software SHM buffer; this GPU development workload requires DMA-BUF rendering");
}

static gboolean watchdog(gpointer data)
{
    Browser *browser = data;
    if (browser->pending && g_get_monotonic_time() - browser->submitted_at > 5 * G_USEC_PER_SEC) {
        fail(browser, "No DRM pageflip completion within five seconds; buffers retained until scanout is disabled");
        browser->watchdog_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean quit_signal(gpointer data)
{
    Browser *browser = data;
    g_main_loop_quit(browser->loop);
    return G_SOURCE_CONTINUE;
}

static bool fullscreen(void *data, bool enabled)
{
    Browser *browser = data;
    if (enabled) wpe_view_backend_dispatch_did_enter_fullscreen(browser->backend);
    else wpe_view_backend_dispatch_did_exit_fullscreen(browser->backend);
    return true;
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    if (event == WEBKIT_LOAD_FINISHED) g_message("Page loaded: %s", webkit_web_view_get_uri(view));
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error, gpointer data)
{
    (void)view; (void)event; (void)data;
    g_warning("Page load failed for %s: %s", uri, error->message);
    return FALSE;
}

static void process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)view;
    Browser *browser = data;
    char *message = g_strdup_printf("WebKit web process terminated, reason=%d", reason);
    fail(browser, message);
    g_free(message);
}

static gboolean initialize_webkit(Browser *browser)
{
    const char *render_device = g_getenv("SIGNAGE_RENDER_DEVICE");
    if (render_device && *render_device) {
        browser->render_fd = open(render_device, O_RDWR | O_CLOEXEC);
        if (browser->render_fd < 0) {
            fail(browser, "Cannot open SIGNAGE_RENDER_DEVICE; check the GPU render-node path and access");
            return FALSE;
        }
        g_message("Using GPU render device %s separately from KMS display %s", render_device, browser->device);
    }
    browser->gbm = gbm_create_device(browser->render_fd >= 0 ? browser->render_fd : browser->fd);
    if (!browser->gbm) { fail(browser, "Cannot create a GBM device for the selected DRM display"); return FALSE; }
    browser->egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, browser->gbm, NULL);
    EGLint major, minor;
    if (browser->egl == EGL_NO_DISPLAY || !eglInitialize(browser->egl, &major, &minor)) {
        fail(browser, "Cannot initialize GBM EGL; check Mesa Panfrost/kmsro and render-node access");
        return FALSE;
    }
    g_message("EGL %d.%d vendor=%s version=%s", major, minor, eglQueryString(browser->egl, EGL_VENDOR), eglQueryString(browser->egl, EGL_VERSION));
    if (!wpe_loader_init("libWPEBackend-fdo-1.0.so") || !wpe_fdo_initialize_for_egl_display(browser->egl)) {
        fail(browser, "Cannot initialize WPEBackend-fdo with the GBM EGL display");
        return FALSE;
    }
    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_buffer_resource = export_legacy, .export_dmabuf_resource = export_dmabuf, .export_shm_buffer = export_shm
    };
    browser->exportable = wpe_view_backend_exportable_fdo_create(&client, browser, browser->mode.hdisplay, browser->mode.vdisplay);
    if (!browser->exportable) { fail(browser, "Cannot create the WPE exportable backend"); return FALSE; }
    browser->backend = wpe_view_backend_exportable_fdo_get_view_backend(browser->exportable);
    WebKitWebViewBackend *backend = webkit_web_view_backend_new(browser->backend, NULL, NULL);
    browser->view = webkit_web_view_new(backend);
    if (!browser->view) { fail(browser, "Cannot create the WebKit web view"); return FALSE; }
    WebKitColor background = { 0, 0, 0, 0 };
    webkit_web_view_set_background_color(browser->view, &background);
    WebKitSettings *settings = webkit_web_view_get_settings(browser->view);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_media_playback_allows_inline(settings, TRUE);
    /* A DOM cursor keeps USB-pointer controls usable without a cursor KMS plane. */
    const char *cursor_source = "(()=>{let c=document.createElement('div');c.style.cssText='position:fixed;width:12px;height:12px;border:2px solid white;border-radius:50%;background:#11374466;pointer-events:none;z-index:2147483647;display:none';document.documentElement.appendChild(c);document.addEventListener('mousemove',e=>{c.style.display='block';c.style.left=(e.clientX-6)+'px';c.style.top=(e.clientY-6)+'px'})})()";
    WebKitUserScript *cursor = webkit_user_script_new(cursor_source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(browser->view), cursor);
    webkit_user_script_unref(cursor);
    g_signal_connect(browser->view, "load-changed", G_CALLBACK(load_changed), browser);
    g_signal_connect(browser->view, "load-failed", G_CALLBACK(load_failed), browser);
    g_signal_connect(browser->view, "web-process-terminated", G_CALLBACK(process_terminated), browser);
    wpe_view_backend_set_fullscreen_handler(browser->backend, fullscreen, browser);
    wpe_view_backend_add_activity_state(browser->backend, wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
    wpe_view_backend_set_target_refresh_rate(browser->backend, mode_refresh(&browser->mode) * 1000);
    browser->input = signage_input_start(browser->backend, browser->mode.hdisplay, browser->mode.vdisplay);
    webkit_web_view_load_uri(browser->view, browser->uri);
    return TRUE;
}

static void cleanup(Browser *browser)
{
    if (browser->fd_source) g_source_remove(browser->fd_source);
    if (browser->watchdog_source) g_source_remove(browser->watchdog_source);
    if (browser->sigterm_source) g_source_remove(browser->sigterm_source);
    if (browser->sigint_source) g_source_remove(browser->sigint_source);
    signage_input_stop(browser->input);
    signage_drm_broker_stop();
    if (browser->modeset) {
        gboolean disabled = FALSE;
        drmModeAtomicReq *request = drmModeAtomicAlloc();
        if (request) {
            drmModeAtomicAddProperty(request, browser->primary, browser->plane_fb, 0);
            drmModeAtomicAddProperty(request, browser->primary, browser->plane_crtc, 0);
            drmModeAtomicAddProperty(request, browser->connector, browser->connector_crtc, 0);
            drmModeAtomicAddProperty(request, browser->crtc, browser->crtc_active, 0);
            drmModeAtomicAddProperty(request, browser->crtc, browser->crtc_mode, 0);
            /* The WebKit video process can still own the brokered overlay. */
            drmModePlaneRes *planes = drmModeGetPlaneResources(browser->fd);
            if (planes) {
                for (uint32_t i = 0; i < planes->count_planes; i++) {
                    uint64_t attached_crtc = 0;
                    uint32_t plane = planes->planes[i];
                    uint32_t crtc_property = property_id(browser->fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &attached_crtc);
                    if (plane != browser->primary && attached_crtc == browser->crtc && crtc_property) {
                        uint32_t fb_property = property_id(browser->fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
                        if (fb_property) drmModeAtomicAddProperty(request, plane, fb_property, 0);
                        drmModeAtomicAddProperty(request, plane, crtc_property, 0);
                    }
                }
                drmModeFreePlaneResources(planes);
            }
            disabled = drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
            drmModeAtomicFree(request);
        }
        if (!disabled) {
            /* Never return a buffer to WebKit while KMS might still read it.
             * The kernel and WPE resources remain owned until process exit.
             */
            g_warning("Could not disable scanout; retaining displayed/pending DMA-BUF resources until process exit");
            return;
        }
    }
    release_frame(browser, browser->queued);
    release_frame(browser, browser->pending);
    release_frame(browser, browser->current);
    if (browser->view) g_object_unref(browser->view);
    if (browser->exportable) wpe_view_backend_exportable_fdo_destroy(browser->exportable);
    if (browser->egl != EGL_NO_DISPLAY) eglTerminate(browser->egl);
    if (browser->gbm) gbm_device_destroy(browser->gbm);
    if (browser->render_fd >= 0) close(browser->render_fd);
    if (browser->mode_blob) drmModeDestroyPropertyBlob(browser->fd, browser->mode_blob);
    if (browser->fd >= 0) { drmDropMaster(browser->fd); close(browser->fd); }
    if (browser->handles) g_hash_table_unref(browser->handles);
    if (browser->loop) g_main_loop_unref(browser->loop);
    g_free(browser->device);
    g_free(browser->failure);
    g_free(browser->holepunch_error);
}

static void help(void)
{
    puts("Usage: signage-browser [--version | --help] [URL]\n"
         "Owned WPE WebKit DRM/GBM shell for Admiral display development.\n"
         "Default URL: http://127.0.0.1:8080/\n\n"
         "SIGNAGE_DRM_DEVICE          Explicit primary DRM node (otherwise connected card auto-detected)\n"
         "SIGNAGE_RENDER_DEVICE       Optional GPU render node for GBM/EGL, separate from KMS\n"
         "SIGNAGE_DRM_CONNECTOR       Connector name (HDMI-A-1) or numeric ID\n"
         "SIGNAGE_DRM_PRIMARY_PLANE   Numeric ARGB8888 primary-plane ID\n"
         "SIGNAGE_DRM_MODE            WIDTHxHEIGHT[@REFRESH] (otherwise preferred connector mode)\n"
         "SIGNAGE_VIDEO_MODE          holepunch (default) or composited\n"
         "SIGNAGE_INPUT               0 disables native keyboard/pointer input\n"
         "SIGNAGE_INPUT_SEAT          udev input seat (default seat0)\n"
         "SIGNAGE_KEYBOARD_LAYOUT     XKB layout (default system/us)\n"
         "SIGNAGE_BROWSER_STATUS      Native JSON status path (default /run/admiral-signage/browser-status.json)\n"
         "WEBKIT_INSPECTOR_SERVER     Remote inspector listen address\n\n"
         "Requires atomic DRM master, GBM EGL and ARGB8888 DMA-BUF scanout.\n"
         "No window manager or Cog dependency. Native touchscreen input is not implemented.");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        printf("signage-browser %s (WPE WebKit %u.%u.%u)\n", VERSION, webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) { help(); return 0; }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) { help(); return 2; }
    Browser browser = { .fd = -1, .render_fd = -1, .egl = EGL_NO_DISPLAY, .uri = argc == 2 ? argv[1] : "http://127.0.0.1:8080/" };
    browser.holepunch = signage_drm_holepunch_enabled();
    browser.handles = g_hash_table_new(g_direct_hash, g_direct_equal);
    browser.loop = g_main_loop_new(NULL, FALSE);
    if (!open_display(&browser)) {
        fail(&browser, "No connected atomic DRM display with ARGB8888 primary plane; check /dev/dri access, DRM master, connector and DE35 support");
        cleanup(&browser);
        return 1;
    }
    if (!initial_scanout(&browser)) {
        fail(&browser, "Cannot establish the initial atomic ARGB browser scanout; check DE35 primary-plane modesetting");
        cleanup(&browser);
        return 1;
    }
    if (browser.holepunch) {
        GError *error = NULL;
        if (!signage_drm_broker_start(browser.fd, browser.connector, browser.crtc, browser.crtc_index, browser.primary,
                                     browser.mode.hdisplay, browser.mode.vdisplay, &error)) {
            browser.holepunch_error = g_strdup(error ? error->message : "DRM holepunch broker initialization failed");
            g_warning("Holepunch unavailable: %s; continuing the browser diagnostic screen", browser.holepunch_error);
            g_clear_error(&error);
        } else browser.broker_started = TRUE;
    }
    browser.fd_source = g_unix_fd_add(browser.fd, G_IO_IN | G_IO_ERR | G_IO_HUP, drm_ready, &browser);
    browser.watchdog_source = g_timeout_add_seconds(1, watchdog, &browser);
    browser.sigterm_source = g_unix_signal_add(SIGTERM, quit_signal, &browser);
    browser.sigint_source = g_unix_signal_add(SIGINT, quit_signal, &browser);
    write_status(&browser);
    if (initialize_webkit(&browser) && !browser.failure) g_main_loop_run(browser.loop);
    int result = browser.exit_code;
    cleanup(&browser);
    return result;
}
