/* SPDX-License-Identifier: MIT
 * signage-video-engine: one persistent hardware decoder for dynamic HTML signage.
 *
 * One pipeline for the life of the process:
 *   appsrc (HEVC Annex-B access units, continuous timestamps) ! h265parse ! DECODER ! SINK
 * DECODER is v4l2slh265dec on the board; SINK is the westerossink adapter in
 * client-role=engine, which hands frames to the browser's single atomic DRM committer.
 * Clips are never separate pipelines: prepared assets (scripts/prepare-engine-asset.py)
 * share one SPS (compat_key), start every GOP with an IDR and never reorder, so the next
 * clip's IDR simply follows the previous clip's last frame with the next timestamp.
 *
 * Control: newline-delimited JSON on a unix socket (SIGNAGE_ENGINE_SOCKET). See
 * DESIGN.md and README "Persistent video engine" for the command/event protocol.
 * Test mode (SIGNAGE_ENGINE_SINK=test, TEST ONLY) replaces the adapter by an appsink that
 * hashes every presented frame; its acknowledgements are labelled and are not scanout
 * proof.
 */
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstbasesink.h>
#include <gst/video/video.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENGINE_PROTOCOL 1
#define MAX_LINE 65536
#define MAX_FED_RECORDS 32

typedef struct { guint64 offset; guint32 size; guint64 pts; gboolean key; } AccessUnit;
typedef enum { ASSET_LOADING, ASSET_READY, ASSET_FAILED } AssetState;

typedef struct {
    gint refs;
    gchar *id, *manifest, *es_path, *compat_key, *es_sha256, *code, *error;
    guint width, height, fps_n, fps_d;
    guint64 duration_ns, es_size;
    AccessUnit *aus; guint n_aus;
    GBytes *data;
    AssetState state;
    gint cancelled; /* atomic */
    gint pins;
    gint64 used_us;
    gboolean in_table, accounted;
} Asset;

typedef struct {
    guint64 id, play_id;
    Asset *asset;
    guint64 base;           /* timeline PTS of asset AU 0 */
    guint first_au;         /* first AU fed (seek target GOP) */
    guint64 start_pts, end_pts, last_pts; /* fed range [start,end); last fed AU */
    guint fed, presented;
    guint loop_index;
    gboolean complete, started, ended, flushed, warm, loop;
    gchar *client;
} Instance;

typedef struct {
    gboolean active, loop;
    Asset *asset;
    gchar *at, *client;
    guint64 play_id;
} Pending;

typedef struct {
    GSocketConnection *connection;
    GDataInputStream *input;
    GCancellable *cancel;
} Control;

typedef struct {
    GMutex lock;
    GMainLoop *loop;
    GstElement *pipeline, *src, *parse, *decoder, *sink;
    gboolean test_sink, sync;
    gchar *decoder_name, *socket_path;
    guint width, height, fps_n, fps_d;
    gchar *compat_key;
    /* feeder state (lock) */
    Instance *cur;
    guint feed_au, warm_limit;
    Pending queued;
    gboolean idle, waiting_noted, drained;
    guint64 fed_end, segment_start;
    GQueue fed;
    guint64 next_instance;
    gint want; /* atomic: appsrc wants data */
    gboolean seek_pending;
    Instance *seek_instance;
    /* main thread state */
    gboolean playing_intent, visible, ready_sent, warm_pending, warm_done, connected, recovering, released;
    gchar *owner;
    gint rect[4]; gboolean have_rect;
    gint alloc_queries, caps_events, sink_buffers; /* atomic */
    guint64 presented_total, rebuilds;
    /* Late-frame accounting from the pipeline's own QoS messages (sink and decoder). Never
     * suppressed: these are the numbers that make "0 drops" mean something. */
    guint64 qos_messages, qos_late_messages, qos_dropped_total;
    gint64 qos_max_jitter_ns;
    gint qos_events, qos_late_events; /* atomic: upstream QoS events leaving the sink */
    gint64 qos_event_max_jitter_ns;   /* atomic */
    GHashTable *qos_dropped_by; /* source element name -> its cumulative dropped count */
    guint rebuild_source;
    gchar *state;
    /* asset cache */
    GHashTable *assets;
    guint64 budget, used;
    GThreadPool *loader;
    gchar **roots;
    guint load_delay_ms;
    /* control */
    GSocketService *service;
    GList *controls;
    gpointer only; /* Control: deliver the next event to this connection only */
    /* test sink */
    FILE *hash_file;
    GstBuffer *test_last;
    gchar *warm_manifest;
} Engine;

static Engine engine_singleton;

/* ------------------------------------------------------------------ utilities */
static Asset *asset_ref(Asset *a) { if (a) g_atomic_int_inc(&a->refs); return a; }
static void asset_unref(Asset *a)
{
    if (!a || !g_atomic_int_dec_and_test(&a->refs)) return;
    g_free(a->id); g_free(a->manifest); g_free(a->es_path); g_free(a->compat_key); g_free(a->es_sha256);
    g_free(a->code); g_free(a->error); g_free(a->aus);
    if (a->data) g_bytes_unref(a->data);
    g_free(a);
}

static void instance_free(Instance *i)
{
    if (!i) return;
    asset_unref(i->asset); g_free(i->client); g_free(i);
}

static void pending_clear(Pending *p)
{
    if (p->asset) { p->asset->pins--; asset_unref(p->asset); }
    g_free(p->at); g_free(p->client);
    memset(p, 0, sizeof *p);
}

static guint64 env_u64(const char *name, guint64 fallback)
{
    const char *v = g_getenv(name);
    return v && *v ? g_ascii_strtoull(v, NULL, 10) : fallback;
}


static guint64 au_duration(const Asset *a, guint index)
{
    if (index + 1 < a->n_aus) return a->aus[index + 1].pts - a->aus[index].pts;
    return gst_util_uint64_scale(GST_SECOND, a->fps_d, a->fps_n);
}

/* ------------------------------------------------------------------ events */
static void broadcast(Engine *e, const gchar *line)
{
    for (GList *l = e->controls; l;) {
        Control *c = l->data; l = l->next;
        if (e->only && e->only != c) continue;
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c->connection));
        GError *error = NULL;
        if (!g_output_stream_write_all(out, line, strlen(line), NULL, NULL, &error) ||
            !g_output_stream_write_all(out, "\n", 1, NULL, NULL, &error)) {
            g_message("Engine control client write failed: %s", error ? error->message : "?");
            g_clear_error(&error);
            g_cancellable_cancel(c->cancel); /* reader callback removes it */
        }
    }
}

static JsonBuilder *event_begin(const gchar *name)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "event"); json_builder_add_string_value(b, name);
    json_builder_set_member_name(b, "mono_us"); json_builder_add_int_value(b, g_get_monotonic_time());
    return b;
}

static void event_end(Engine *e, JsonBuilder *b)
{
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b);
    gchar *text = json_to_string(root, FALSE);
    if (g_getenv("SIGNAGE_ENGINE_TRACE")) g_message("Engine event %s", text);
    broadcast(e, text);
    g_free(text); json_node_unref(root); g_object_unref(b);
}

#define ADD_STR(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_string_value(b, (v) ? (v) : ""); } while (0)
#define ADD_INT(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_int_value(b, (gint64)(v)); } while (0)
#define ADD_BOOL(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_boolean_value(b, (v)); } while (0)

static void emit_simple(Engine *e, const gchar *name, const gchar *asset, const gchar *code, const gchar *message, guint64 play_id)
{
    JsonBuilder *b = event_begin(name);
    if (asset) ADD_STR(b, "asset", asset);
    if (code) ADD_STR(b, "code", code);
    if (message) ADD_STR(b, "message", message);
    if (play_id) ADD_INT(b, "play_id", play_id);
    event_end(e, b);
}

/* Streaming-thread notes are routed through the bus to the main loop. */
static void post_note(Engine *e, const gchar *type, const gchar *asset, guint64 play_id)
{
    GstStructure *s = gst_structure_new("engine-note", "type", G_TYPE_STRING, type,
        "asset", G_TYPE_STRING, asset ? asset : "", "play_id", G_TYPE_UINT64, play_id, NULL);
    gst_element_post_message(e->pipeline, gst_message_new_application(GST_OBJECT(e->pipeline), s));
}

/* ------------------------------------------------------------------ assets */
static gboolean path_allowed(Engine *e, const gchar *path, gchar **resolved)
{
    char real[PATH_MAX];
    if (!path || !realpath(path, real)) return FALSE;
    for (gchar **root = e->roots; root && *root; root++) {
        char rr[PATH_MAX];
        if (!**root || !realpath(*root, rr)) continue;
        gsize n = strlen(rr);
        if (!strncmp(real, rr, n) && (real[n] == '/' || real[n] == 0)) { *resolved = g_strdup(real); return TRUE; }
    }
    return FALSE;
}

static gboolean fail_asset(Asset *a, const gchar *code, gchar *message)
{
    g_free(a->code); a->code = g_strdup(code);
    g_free(a->error); a->error = message;
    a->state = ASSET_FAILED;
    return FALSE;
}

/* Parses and validates a manifest against the engine profile (main thread; no ES I/O). */
static gboolean parse_manifest(Engine *e, Asset *a, const gchar *manifest_path)
{
    gchar *resolved = NULL;
    if (!path_allowed(e, manifest_path, &resolved))
        return fail_asset(a, "forbidden", g_strdup_printf("manifest %s is not under SIGNAGE_ENGINE_ASSET_ROOTS", manifest_path ? manifest_path : "(null)"));
    a->manifest = resolved;
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, resolved, &error)) {
        gboolean r = fail_asset(a, "manifest", g_strdup_printf("cannot read manifest: %s", error->message));
        g_error_free(error); g_object_unref(parser); return r;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *o = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    const char *need[] = { "engine_protocol", "codec", "width", "height", "fps_n", "fps_d", "compat_key", "es", "es_size", "es_sha256", "access_units", "max_num_reorder" };
    for (guint i = 0; o && i < G_N_ELEMENTS(need); i++) if (!json_object_has_member(o, need[i])) o = NULL;
    if (!o) { g_object_unref(parser); return fail_asset(a, "manifest", g_strdup("manifest is missing required fields")); }
    gboolean ok = TRUE;
    gchar *why = NULL;
    if (json_object_get_int_member(o, "engine_protocol") != ENGINE_PROTOCOL) why = g_strdup("unsupported engine_protocol");
    else if (g_strcmp0(json_object_get_string_member(o, "codec"), "hevc")) why = g_strdup("codec is not hevc");
    else if (json_object_get_int_member(o, "max_num_reorder") != 0) why = g_strdup("stream reorders pictures");
    if (!why) {
        a->width = json_object_get_int_member(o, "width"); a->height = json_object_get_int_member(o, "height");
        a->fps_n = json_object_get_int_member(o, "fps_n"); a->fps_d = json_object_get_int_member(o, "fps_d");
        if (a->width != e->width || a->height != e->height || !a->fps_d || a->fps_n * e->fps_d != e->fps_n * a->fps_d)
            why = g_strdup_printf("asset is %ux%u@%u/%u, engine profile is %ux%u@%u/%u", a->width, a->height, a->fps_n, a->fps_d,
                                  e->width, e->height, e->fps_n, e->fps_d);
    }
    if (why) { g_object_unref(parser); return fail_asset(a, "incompatible", why); }
    a->compat_key = g_strdup(json_object_get_string_member(o, "compat_key"));
    if (e->compat_key && g_strcmp0(e->compat_key, a->compat_key)) {
        g_object_unref(parser);
        return fail_asset(a, "incompatible", g_strdup("compat_key differs from the engine's decoder configuration (SPS); re-prepare the asset with the engine profile"));
    }
    a->es_sha256 = g_strdup(json_object_get_string_member(o, "es_sha256"));
    a->es_size = json_object_get_int_member(o, "es_size");
    const gchar *es = json_object_get_string_member(o, "es");
    if (!es || strchr(es, '/') || !strcmp(es, "..")) ok = FALSE;
    gchar *dir = g_path_get_dirname(resolved);
    a->es_path = ok ? g_build_filename(dir, es, NULL) : NULL;
    g_free(dir);
    JsonArray *units = json_object_get_array_member(o, "access_units");
    guint n = units ? json_array_get_length(units) : 0;
    if (!ok || !n || n > 1000000 || !a->es_size || a->es_size > G_MAXUINT32 * 4ULL) {
        g_object_unref(parser); return fail_asset(a, "manifest", g_strdup("invalid es name, size or access unit table"));
    }
    a->aus = g_new0(AccessUnit, n);
    guint64 expect = 0, prev_pts = 0;
    for (guint i = 0; i < n && ok; i++) {
        JsonObject *u = json_array_get_object_element(units, i);
        if (!u || !json_object_has_member(u, "offset") || !json_object_has_member(u, "size") || !json_object_has_member(u, "pts")) { ok = FALSE; break; }
        AccessUnit *au = &a->aus[i];
        au->offset = json_object_get_int_member(u, "offset");
        au->size = json_object_get_int_member(u, "size");
        au->pts = json_object_get_int_member(u, "pts");
        au->key = json_object_has_member(u, "key") && json_object_get_boolean_member(u, "key");
        ok = au->offset == expect && au->size > 4 && au->offset + au->size <= a->es_size && (i == 0 ? au->pts == 0 : au->pts > prev_pts);
        expect = au->offset + au->size; prev_pts = au->pts;
    }
    a->n_aus = n;
    g_object_unref(parser);
    if (!ok || expect != a->es_size || !a->aus[0].key)
        return fail_asset(a, "manifest", g_strdup("access unit table is not contiguous, monotonic and IDR-first"));
    a->duration_ns = a->aus[n - 1].pts + au_duration(a, n - 1);
    return TRUE;
}

/* Loader-thread failure: record the reason only; the main thread publishes the state. */
static gboolean read_fail(Asset *a, const gchar *code, gchar *message)
{
    g_free(a->code); a->code = g_strdup(code);
    g_free(a->error); a->error = message;
    return FALSE;
}

/* Loader thread: reads the ES into anonymous RAM, verifies SHA-256 and start codes. */
static gboolean asset_read(Engine *e, Asset *a)
{
    if (e->load_delay_ms) {
        for (guint waited = 0; waited < e->load_delay_ms && !g_atomic_int_get(&a->cancelled); waited += 10) g_usleep(10000);
    }
    FILE *f = g_fopen(a->es_path, "rb");
    if (!f) return read_fail(a, "missing", g_strdup_printf("cannot open %s: %s", a->es_path, g_strerror(errno)));
    guint8 *buffer = g_try_malloc(a->es_size);
    if (!buffer) { fclose(f); return read_fail(a, "memory", g_strdup("cannot allocate asset buffer")); }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    guint64 got = 0;
    while (got < a->es_size && !g_atomic_int_get(&a->cancelled)) {
        size_t chunk = MIN((guint64)(1 << 20), a->es_size - got);
        size_t n = fread(buffer + got, 1, chunk, f);
        if (!n) break;
        g_checksum_update(sum, buffer + got, n);
        got += n;
    }
    gboolean extra = fgetc(f) != EOF;
    fclose(f);
    if (g_atomic_int_get(&a->cancelled)) { g_free(buffer); g_checksum_free(sum); return read_fail(a, "cancelled", g_strdup("cancelled")); }
    gboolean ok = got == a->es_size && !extra && !g_strcmp0(g_checksum_get_string(sum), a->es_sha256);
    g_checksum_free(sum);
    if (!ok) { g_free(buffer); return read_fail(a, "integrity", g_strdup("elementary stream size/SHA-256 does not match the manifest")); }
    for (guint i = 0; i < a->n_aus; i++) {
        const guint8 *p = buffer + a->aus[i].offset;
        if (!((p[0] == 0 && p[1] == 0 && p[2] == 1) || (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1))) {
            g_free(buffer); return read_fail(a, "integrity", g_strdup_printf("access unit %u does not start with an Annex-B start code", i));
        }
    }
    a->data = g_bytes_new_take(buffer, a->es_size); /* state is published by the main thread */
    return TRUE;
}

static void cache_drop(Engine *e, Asset *a)
{
    if (!a->in_table || g_hash_table_lookup(e->assets, a->id) != a) return;
    a->in_table = FALSE;
    if (a->accounted) { e->used -= a->es_size; a->accounted = FALSE; }
    g_hash_table_remove(e->assets, a->id); /* drops the table reference */
}

/* Evicts least-recently-used unpinned ready assets until `need` bytes fit. */
static gboolean cache_reserve(Engine *e, guint64 need)
{
    while (e->used + need > e->budget) {
        Asset *victim = NULL;
        GHashTableIter it; gpointer k, v;
        g_hash_table_iter_init(&it, e->assets);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            Asset *a = v;
            if (a->state == ASSET_READY && a->pins == 0 && (!victim || a->used_us < victim->used_us)) victim = a;
        }
        if (!victim) return FALSE;
        g_message("Engine cache evicts %s (%" G_GUINT64_FORMAT " bytes)", victim->id, victim->es_size);
        cache_drop(e, victim);
    }
    return TRUE;
}

static gboolean load_done(gpointer data);
static void loader_thread(gpointer data, gpointer user)
{
    Asset *a = data; Engine *e = user;
    asset_read(e, a);
    g_idle_add(load_done, a); /* passes the loader's reference */
}

/* Returns a referenced asset (ready, loading or failed with code/error). */
static Asset *asset_get(Engine *e, const gchar *id, const gchar *manifest, gboolean warm_sync)
{
    Asset *a = id ? g_hash_table_lookup(e->assets, id) : NULL;
    if (a && a->state != ASSET_FAILED && (!manifest || !g_strcmp0(a->manifest, manifest) || !a->manifest)) {
        a->used_us = g_get_monotonic_time();
        return asset_ref(a);
    }
    if (a && a->state == ASSET_FAILED) cache_drop(e, a);
    a = g_new0(Asset, 1);
    a->refs = 1;
    a->id = g_strdup(id ? id : "");
    a->used_us = g_get_monotonic_time();
    if (!id || !*id) { fail_asset(a, "invalid", g_strdup("asset id required")); return a; }
    if (!parse_manifest(e, a, manifest)) return a;
    if (!cache_reserve(e, a->es_size)) {
        fail_asset(a, "over-budget", g_strdup_printf("asset needs %" G_GUINT64_FORMAT " bytes; cache budget %" G_GUINT64_FORMAT
                   " with %" G_GUINT64_FORMAT " pinned/in use", a->es_size, e->budget, e->used));
        return a;
    }
    a->state = ASSET_LOADING;
    e->used += a->es_size;
    a->accounted = a->in_table = TRUE;
    g_hash_table_replace(e->assets, a->id, asset_ref(a));
    if (warm_sync) {
        a->state = asset_read(e, a) ? ASSET_READY : ASSET_FAILED;
        if (a->state == ASSET_FAILED) cache_drop(e, a);
    }
    else g_thread_pool_push(e->loader, asset_ref(a), NULL);
    return a;
}

/* ------------------------------------------------------------------ feeder */
/* Timeline PTS now being presented (running time 0 == segment_start), or 0 if unknown. */
static guint64 now_pts(Engine *e)
{
    if (!e->pipeline) return 0;
    GstState state = GST_STATE(e->pipeline);
    /* Paused: the running time resumes from the pipeline start time. */
    GstClockTime running = state == GST_STATE_PLAYING ? gst_element_get_current_running_time(e->pipeline)
                                                      : gst_element_get_start_time(e->pipeline);
    return GST_CLOCK_TIME_IS_VALID(running) ? e->segment_start + running : 0;
}

/* Starts feeding `asset` from first_au. Continuous with the previous frame unless the
 * feeder was idle (holding the last frame), in which case it starts a short margin
 * after the current clock position so no frame is late. Lock held. */
static Instance *start_instance_locked(Engine *e, Asset *asset, guint first_au, guint64 play_id,
                                       const gchar *client, gboolean loop, guint loop_index)
{
    guint64 start = e->fed_end;
    if (e->idle || !e->cur) {
        guint64 margin = env_u64("SIGNAGE_ENGINE_RESTART_MARGIN_MS", 120) * GST_MSECOND;
        guint64 now = now_pts(e);
        if (now && now + margin > start) start = now + margin;
    }
    Instance *i = g_new0(Instance, 1);
    i->id = ++e->next_instance;
    i->play_id = play_id;
    i->asset = asset_ref(asset);
    i->first_au = first_au;
    i->base = start - asset->aus[first_au].pts;
    i->start_pts = i->end_pts = start;
    i->loop_index = loop_index;
    i->client = g_strdup(client);
    if (e->cur && !e->cur->complete && e->cur->fed) e->cur->complete = TRUE; /* cut at a GOP boundary */
    if (e->cur) e->cur->asset->pins--;
    asset->pins++;
    e->cur = i;
    e->feed_au = first_au;
    e->idle = FALSE;
    e->drained = FALSE;
    e->waiting_noted = FALSE;
    g_queue_push_tail(&e->fed, i);
    while (g_queue_get_length(&e->fed) > MAX_FED_RECORDS) {
        Instance *old = g_queue_peek_head(&e->fed);
        if (old == e->cur) break;
        instance_free(g_queue_pop_head(&e->fed));
    }
    i->loop = loop;
    return i;
}

static void start_queued_locked(Engine *e)
{
    Pending q = e->queued;
    memset(&e->queued, 0, sizeof e->queued);
    start_instance_locked(e, q.asset, 0, q.play_id, q.client, q.loop, 0);
    q.asset->pins--; /* the instance took its own pin */
    asset_unref(q.asset); g_free(q.at); g_free(q.client);
}

/* End of the current clip: decide what follows. FALSE: hold the last frame (idle). */
static gboolean advance_locked(Engine *e)
{
    Instance *cur = e->cur;
    cur->complete = TRUE;
    if (cur->warm) return FALSE;
    if (e->queued.active && e->queued.asset->state == ASSET_READY) { start_queued_locked(e); return TRUE; }
    if (e->queued.active && e->queued.asset->state == ASSET_FAILED) {
        post_note(e, "queued-failed", e->queued.asset->id, e->queued.play_id);
        pending_clear(&e->queued);
    }
    if (e->queued.active) {
        /* Requested asset is still loading: continue the current clip, never freeze. */
        if (!e->waiting_noted) post_note(e, "waiting", e->queued.asset->id, e->queued.play_id);
        Instance *i = start_instance_locked(e, cur->asset, 0, cur->play_id, cur->client, cur->loop, cur->loop_index + 1);
        e->waiting_noted = TRUE;
        (void)i;
        return TRUE;
    }
    if (cur->loop) {
        start_instance_locked(e, cur->asset, 0, cur->play_id, cur->client, TRUE, cur->loop_index + 1);
        return TRUE;
    }
    return FALSE;
}

static void push_au_locked(Engine *e)
{
    Instance *i = e->cur;
    Asset *a = i->asset;
    const AccessUnit *au = &a->aus[e->feed_au];
    gsize total = 0;
    const guint8 *base = g_bytes_get_data(a->data, &total);
    /* Zero-copy: the buffer holds a reference on the asset bytes until downstream is done. */
    GstBuffer *buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, (gpointer)base, total, au->offset, au->size,
                                                    g_bytes_ref(a->data), (GDestroyNotify)g_bytes_unref);
    guint64 pts = i->base + au->pts, duration = au_duration(a, e->feed_au);
    GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = duration;
    if (!au->key) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    i->last_pts = pts;
    i->end_pts = pts + duration;
    i->fed++;
    e->fed_end = pts + duration;
    e->feed_au++;
    if (i->warm && e->feed_au >= e->warm_limit) e->feed_au = a->n_aus; /* warm-up: first frames only */
    gst_app_src_push_buffer(GST_APP_SRC(e->src), buffer);
}

static void feed_locked(Engine *e)
{
    while (g_atomic_int_get(&e->want) && !e->seek_pending && e->cur) {
        Asset *a = e->cur->asset;
        if (e->feed_au >= a->n_aus) {
            if (!advance_locked(e)) { e->idle = TRUE; return; }
            continue;
        }
        if (e->feed_au > e->cur->first_au && a->aus[e->feed_au].key && e->queued.active &&
            !g_strcmp0(e->queued.at, "gop") && e->queued.asset->state == ASSET_READY) {
            e->cur->complete = TRUE;
            start_queued_locked(e);
            continue;
        }
        push_au_locked(e);
    }
}

/* Runs on appsrc's streaming thread: every previously pushed access unit has already gone
 * through h265parse and the decoder. When the timeline holds (nothing follows), drain the
 * decoder with a serialized still-frame event (never EOS) so its pipelined last frames are output;
 * the next clip starts with an IDR anyway. */
static void need_data(GstAppSrc *src, guint length, gpointer data)
{
    (void)length;
    Engine *e = data;
    g_atomic_int_set(&e->want, 1);
    g_mutex_lock(&e->lock);
    feed_locked(e);
    /* Buffers pushed by feed_locked() above are still queued in appsrc (this callback runs
     * before appsrc pops them): an event sent now would overtake them. Drain only on a call
     * that finds the queue empty, i.e. after every pushed AU went through parser+decoder. */
    gboolean drain = e->idle && !e->drained && !e->seek_pending && e->cur &&
                     gst_app_src_get_current_level_bytes(src) == 0;
    if (drain) e->drained = TRUE;
    g_mutex_unlock(&e->lock);
    if (drain) {
        GstPad *pad = gst_element_get_static_pad(GST_ELEMENT(src), "src");
        /* GstVideoDecoder (1.26) has no DRAIN-query handler, so that query was a no-op and the
         * stateless decoders' one-frame output delay kept the last frame (and the whole
         * startup preflight) waiting for a successor that never came. A still-frame event is
         * serialized, is not EOS (no pad block, no STREAMOFF/REQBUFS) and makes GstVideoDecoder
         * call the subclass drain, which outputs every queued picture. Hardware-verified. */
        GstEvent *still = gst_video_event_new_still_frame(TRUE);
        gst_pad_push_event(pad, still);
        gst_object_unref(pad);
    }
}

static void enough_data(GstAppSrc *src, gpointer data) { (void)src; g_atomic_int_set(&((Engine *)data)->want, 0); }

/* Flushing seeks land here on the streaming thread after the flush. */
static gboolean seek_data(GstAppSrc *src, guint64 offset, gpointer data)
{
    (void)src;
    Engine *e = data;
    g_mutex_lock(&e->lock);
    if (e->seek_pending) {
        for (GList *l = e->fed.head; l; l = l->next) {
            Instance *old = l->data;
            if (!old->ended) old->flushed = TRUE;
        }
        if (e->cur) { e->cur->complete = TRUE; e->cur->asset->pins--; e->cur = NULL; }
        e->segment_start = offset;
        Instance *next = e->seek_instance;
        e->seek_instance = NULL;
        e->seek_pending = FALSE;
        e->drained = FALSE;
        if (next) {
            next->asset->pins++;
            e->cur = next;
            e->feed_au = next->first_au;
            e->fed_end = next->base + next->asset->aus[next->first_au].pts;
            next->start_pts = next->end_pts = e->fed_end;
            g_queue_push_tail(&e->fed, next);
            e->idle = FALSE;
        } else {
            e->idle = TRUE;
            e->fed_end = offset;
        }
    }
    g_mutex_unlock(&e->lock);
    return TRUE;
}

/* ------------------------------------------------------------------ presentation */
static void emit_instance(Engine *e, const gchar *name, const Instance *i, guint64 pts, const gchar *reason)
{
    JsonBuilder *b = event_begin(name);
    ADD_STR(b, "asset", i->asset->id);
    ADD_INT(b, "instance", i->id);
    ADD_INT(b, "play_id", i->play_id);
    ADD_INT(b, "loop_index", i->loop_index);
    ADD_INT(b, "pts", pts);
    ADD_INT(b, "presented", i->presented);
    ADD_INT(b, "fed", i->fed);
    ADD_INT(b, "frames", i->asset->n_aus - i->first_au);
    ADD_STR(b, "source", e->test_sink ? "test-appsink" : "browser-presented");
    if (reason) ADD_STR(b, "reason", reason);
    event_end(e, b);
}

/* A frame with timeline PTS `pts` was presented (browser PRESENTED after the matching
 * flip, or the test sink). Main thread. */
static void on_presented(Engine *e, guint64 pts)
{
    g_mutex_lock(&e->lock);
    Instance *hit = NULL;
    for (GList *l = e->fed.head; l; l = l->next) {
        Instance *i = l->data;
        if (!i->flushed && i->fed && pts >= i->start_pts && pts < i->end_pts) { hit = i; break; }
    }
    if (!hit || hit->warm) { g_mutex_unlock(&e->lock); return; }
    e->presented_total++;
    hit->presented++;
    /* Earlier instances can no longer present anything. */
    GPtrArray *ended = g_ptr_array_new();
    for (GList *l = e->fed.head; l && l->data != hit; l = l->next) {
        Instance *old = l->data;
        if (old->started && !old->ended && !old->flushed && !old->warm) { old->ended = TRUE; g_ptr_array_add(ended, old); }
    }
    gboolean started = !hit->started;
    hit->started = TRUE;
    gboolean last = hit->complete && pts == hit->last_pts && !hit->ended;
    gboolean idle_after = last && e->cur == hit && e->idle;
    if (last) hit->ended = TRUE;
    for (guint k = 0; k < ended->len; k++) emit_instance(e, "ended", ended->pdata[k], pts, "handoff");
    if (started) emit_instance(e, hit->loop_index ? "looped" : "started", hit, pts, NULL);
    if (last) emit_instance(e, "ended", hit, pts, idle_after ? "complete" : "handoff");
    g_ptr_array_unref(ended);
    g_mutex_unlock(&e->lock);
}

static void kick(Engine *e)
{
    g_mutex_lock(&e->lock);
    feed_locked(e);
    g_mutex_unlock(&e->lock);
}

static void apply_visibility(Engine *e)
{
    if (!e->sink) return;
    if (!e->test_sink) g_object_set(e->sink, "visible", e->visible, NULL);
    else if (e->visible == FALSE) gst_buffer_replace(&e->test_last, NULL);
}

static void apply_state(Engine *e)
{
    if (!e->pipeline) return;
    gboolean play = e->playing_intent && e->visible;
    gst_element_set_state(e->pipeline, play || e->warm_pending ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

/* ------------------------------------------------------------------ test sink */
static void test_present(Engine *e, GstSample *sample, gboolean preroll)
{
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    gboolean visible;
    g_mutex_lock(&e->lock); visible = e->visible || e->warm_pending; g_mutex_unlock(&e->lock);
    if (!buffer || !e->visible) return; /* invisible: consumed, never shown */
    if (!preroll && buffer == e->test_last) return; /* prerolled frame already presented */
    (void)visible;
    gst_buffer_replace(&e->test_last, buffer);
    if (e->hash_file) {
        GstVideoInfo info; GstVideoFrame frame;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) && gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
            GChecksum *sum = g_checksum_new(G_CHECKSUM_MD5);
            for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES(&frame); p++) {
                const guint8 *row = GST_VIDEO_FRAME_PLANE_DATA(&frame, p);
                gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p);
                gint width = GST_VIDEO_FRAME_COMP_WIDTH(&frame, p) * GST_VIDEO_FRAME_COMP_PSTRIDE(&frame, p);
                for (gint y = 0; y < GST_VIDEO_FRAME_COMP_HEIGHT(&frame, p); y++) g_checksum_update(sum, row + (gsize)y * stride, width);
            }
            fprintf(e->hash_file, "%" G_GUINT64_FORMAT " %s\n", GST_BUFFER_PTS(buffer), g_checksum_get_string(sum));
            fflush(e->hash_file);
            g_checksum_free(sum);
            gst_video_frame_unmap(&frame);
        }
    }
    GstStructure *s = gst_structure_new("admiral-presented", "pts", G_TYPE_UINT64, GST_BUFFER_PTS(buffer),
                                        "source", G_TYPE_STRING, "test-appsink", NULL);
    gst_element_post_message(e->sink, gst_message_new_element(GST_OBJECT(e->sink), s));
}

static GstFlowReturn test_new_sample(GstAppSink *sink, gpointer data)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_FLUSHING;
    test_present(data, sample, FALSE);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn test_new_preroll(GstAppSink *sink, gpointer data)
{
    GstSample *sample = gst_app_sink_pull_preroll(sink);
    if (!sample) return GST_FLOW_FLUSHING;
    Engine *e = data;
    /* Mirror GstVideoSink show-preroll-frame for seeks while paused; the startup preroll
     * of a hidden warm-up is consumed invisibly. */
    if (e->visible) test_present(e, sample, TRUE);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* ------------------------------------------------------------------ pipeline */
static GstPadProbeReturn decoder_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad;
    Engine *e = data;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) {
        GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
        if (q && GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION) g_atomic_int_inc(&e->alloc_queries);
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
        if (ev && GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) g_atomic_int_inc(&e->caps_events);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn sink_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad; (void)info;
    g_atomic_int_inc(&((Engine *)data)->sink_buffers);
    return GST_PAD_PROBE_OK;
}

static void destroy_pipeline(Engine *e)
{
    if (!e->pipeline) return;
    gst_element_set_state(e->pipeline, GST_STATE_NULL);
    GstBus *bus = gst_element_get_bus(e->pipeline);
    gst_bus_remove_watch(bus); gst_object_unref(bus);
    gst_object_unref(e->pipeline);
    e->pipeline = e->src = e->parse = e->decoder = e->sink = NULL;
    gst_buffer_replace(&e->test_last, NULL);
    g_mutex_lock(&e->lock);
    if (e->cur) { e->cur->asset->pins--; e->cur = NULL; }
    g_queue_clear_full(&e->fed, (GDestroyNotify)instance_free);
    instance_free(e->seek_instance); e->seek_instance = NULL; e->seek_pending = FALSE;
    pending_clear(&e->queued);
    e->idle = TRUE; e->fed_end = e->segment_start = 0;
    g_mutex_unlock(&e->lock);
}

static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data);
static void schedule_rebuild(Engine *e, guint ms);

static GstPadProbeReturn qos_event_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad;
    Engine *e = data;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event && GST_EVENT_TYPE(event) == GST_EVENT_QOS) {
        GstQOSType type; gdouble proportion; GstClockTimeDiff diff; GstClockTime timestamp;
        gst_event_parse_qos(event, &type, &proportion, &diff, &timestamp);
        g_atomic_int_inc(&e->qos_events);
        if (diff > 5 * GST_MSECOND) g_atomic_int_inc(&e->qos_late_events);
        gint64 seen = __atomic_load_n(&e->qos_event_max_jitter_ns, __ATOMIC_RELAXED);
        while (diff > seen && !__atomic_compare_exchange_n(&e->qos_event_max_jitter_ns, &seen, (gint64)diff, FALSE, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
    }
    return GST_PAD_PROBE_OK;
}

static gboolean build_pipeline(Engine *e)
{
    e->pipeline = gst_pipeline_new("signage-video-engine");
    e->src = gst_element_factory_make("appsrc", "engine-src");
    e->parse = gst_element_factory_make("h265parse", "engine-parse");
    e->decoder = gst_element_factory_make(e->decoder_name, "engine-decoder");
    e->sink = e->test_sink ? gst_element_factory_make("appsink", "engine-sink") : gst_element_factory_make("westerossink", "engine-sink");
    if (!e->src || !e->parse || !e->decoder || !e->sink) {
        g_warning("Engine: missing element (appsrc/h265parse/%s/%s)", e->decoder_name, e->test_sink ? "appsink" : "westerossink");
        for (GstElement **el = (GstElement *[]){ e->src, e->parse, e->decoder, e->sink, NULL }; *el; el++) gst_object_unref(*el);
        gst_object_unref(e->pipeline); e->pipeline = e->src = e->parse = e->decoder = e->sink = NULL;
        return FALSE;
    }
    GstCaps *caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au", "width", G_TYPE_INT, (gint)e->width, "height", G_TYPE_INT, (gint)e->height,
        "framerate", GST_TYPE_FRACTION, (gint)e->fps_n, (gint)e->fps_d, NULL);
    g_object_set(e->src, "caps", caps, "format", GST_FORMAT_TIME, "is-live", FALSE, "do-timestamp", FALSE,
                 "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, "block", FALSE,
                 "max-bytes", (guint64)0, "max-buffers", (guint64)0,
                 "max-time", env_u64("SIGNAGE_ENGINE_FEED_MS", 500) * GST_MSECOND, NULL);
    gst_caps_unref(caps);
    /* Software comparison decoders: slice threads only, so no frame-threading delay. */
    if (g_str_has_prefix(e->decoder_name, "avdec_") && g_object_class_find_property(G_OBJECT_GET_CLASS(e->decoder), "thread-type"))
        gst_util_set_object_arg(G_OBJECT(e->decoder), "thread-type", "slice");
    GstAppSrcCallbacks callbacks = { .need_data = need_data, .enough_data = enough_data, .seek_data = seek_data };
    gst_app_src_set_callbacks(GST_APP_SRC(e->src), &callbacks, e, NULL);
    if (e->test_sink) {
        GstCaps *raw = gst_caps_from_string("video/x-raw");
        g_object_set(e->sink, "caps", raw, "sync", e->sync, "max-buffers", 2u, "drop", FALSE, "emit-signals", FALSE, NULL);
        gst_caps_unref(raw);
        GstAppSinkCallbacks sc = { .new_sample = test_new_sample, .new_preroll = test_new_preroll };
        gst_app_sink_set_callbacks(GST_APP_SINK(e->sink), &sc, e, NULL);
    } else {
        g_object_set(e->sink, "client-role", "engine", "visible", e->visible, NULL);
        if (e->have_rect) {
            gchar *r = g_strdup_printf("%d,%d,%d,%d", e->rect[0], e->rect[1], e->rect[2], e->rect[3]);
            g_object_set(e->sink, "rectangle", r, NULL); g_free(r);
        }
    }
    gst_bin_add_many(GST_BIN(e->pipeline), e->src, e->parse, e->decoder, e->sink, NULL);
    if (!gst_element_link_many(e->src, e->parse, e->decoder, e->sink, NULL)) {
        g_warning("Engine: cannot link appsrc ! h265parse ! %s ! sink", e->decoder_name);
        gst_object_unref(e->pipeline); e->pipeline = NULL; return FALSE;
    }
    GstPad *pad = gst_element_get_static_pad(e->decoder, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, decoder_probe, e, NULL);
    gst_object_unref(pad);
    pad = gst_element_get_static_pad(e->sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, sink_probe, e, NULL);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, qos_event_probe, e, NULL);
    gst_object_unref(pad);
    GstBus *bus = gst_element_get_bus(e->pipeline);
    gst_bus_add_watch(bus, bus_message, e);
    gst_object_unref(bus);
    return TRUE;
}

static void emit_ready(Engine *e, gboolean warm)
{
    if (e->ready_sent && !e->only) return;
    e->ready_sent = TRUE;
    e->warm_done = warm;
    g_free(e->state); e->state = g_strdup("ready");
    JsonBuilder *b = event_begin("engine-ready");
    ADD_INT(b, "protocol", ENGINE_PROTOCOL);
    ADD_BOOL(b, "warm", warm);
    ADD_INT(b, "alloc_queries", g_atomic_int_get(&e->alloc_queries));
    ADD_INT(b, "caps_events", g_atomic_int_get(&e->caps_events));
    ADD_STR(b, "decoder", e->decoder_name);
    ADD_STR(b, "sink", e->test_sink ? "test-appsink" : "westerossink(engine)");
    ADD_INT(b, "cache_budget", e->budget);
    ADD_INT(b, "rebuilds", e->rebuilds);
    event_end(e, b);
}

static gboolean flush_to(Engine *e, Instance *next, guint64 target_pts);
static gboolean warm_check(gpointer data)
{
    Engine *e = data;
    if (!e->warm_pending) return G_SOURCE_REMOVE;
    if (g_atomic_int_get(&e->sink_buffers) >= (gint)e->warm_limit && g_atomic_int_get(&e->alloc_queries) >= 1) {
        /* Discard anything of the warm-up still queued in the sink (it may be prerolled and
         * must never become visible), leaving an idle timeline with the pool allocated. */
        flush_to(e, NULL, e->fed_end + GST_SECOND);
        e->warm_pending = FALSE;
        g_message("Engine preflight complete: decoder pool allocated (%d allocation queries), %d warm frame(s) consumed invisibly",
                  g_atomic_int_get(&e->alloc_queries), g_atomic_int_get(&e->sink_buffers));
        apply_state(e);
        emit_ready(e, TRUE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Builds and starts the pipeline; with a warm-up asset, prerolls its first frames
 * invisibly so the decoder pool, broker generation and plane admission exist before any
 * timed playback. */
static gboolean start_engine(gpointer data)
{
    Engine *e = data;
    e->rebuild_source = 0;
    if (!build_pipeline(e)) { schedule_rebuild(e, 2000); return G_SOURCE_REMOVE; }
    Asset *warm = NULL;
    if (e->warm_manifest) {
        gchar *base = g_path_get_basename(e->warm_manifest);
        if (g_str_has_suffix(base, ".engine.json")) base[strlen(base) - strlen(".engine.json")] = 0;
        const gchar *id = g_getenv("SIGNAGE_ENGINE_WARMUP_ID") && *g_getenv("SIGNAGE_ENGINE_WARMUP_ID") ? g_getenv("SIGNAGE_ENGINE_WARMUP_ID") : base;
        warm = asset_get(e, id, e->warm_manifest, TRUE);
        g_free(base);
        if (warm->state != ASSET_READY) {
            g_warning("Engine warm-up asset unusable (%s: %s); starting cold", warm->code, warm->error);
            asset_unref(warm); warm = NULL;
        } else if (!e->compat_key) e->compat_key = g_strdup(warm->compat_key);
    }
    if (warm) {
        e->visible = FALSE;
        apply_visibility(e);
        g_mutex_lock(&e->lock);
        /* The first IDR allocates the capture pool; a few more frames exercise the P-frame
         * path (and the kernel's lazily allocated per-buffer MV-col buffers) before any
         * timed playback. Hardware: those lazy allocations still total 9 small ones in the
         * first ~15 s of playback and none afterwards, whatever is held here. */
        e->warm_limit = (guint)MIN(MAX(env_u64("SIGNAGE_ENGINE_WARM_FRAMES", 12), 1), warm->n_aus);
        g_atomic_int_set(&e->sink_buffers, 0);
        Instance *i = start_instance_locked(e, warm, 0, 0, NULL, FALSE, 0);
        i->warm = TRUE;
        g_mutex_unlock(&e->lock);
        e->warm_pending = TRUE;
        g_timeout_add(10, warm_check, e);
        asset_unref(warm);
    }
    GstStateChangeReturn r = gst_element_set_state(e->pipeline, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE) {
        g_message("Engine pipeline did not start (broker/decoder unavailable); retrying");
        e->warm_pending = FALSE;
        destroy_pipeline(e);
        schedule_rebuild(e, 1000);
        return G_SOURCE_REMOVE;
    }
    e->connected = TRUE;
    if (warm) {
        kick(e); /* stays hidden until a page shows a video */
    } else {
        apply_state(e);
        emit_ready(e, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_rebuild(Engine *e, guint ms)
{
    if (e->rebuild_source) return;
    e->rebuilds++;
    g_free(e->state); e->state = g_strdup("connecting");
    e->rebuild_source = g_timeout_add(ms, start_engine, e);
}

static gboolean recover(gpointer data)
{
    Engine *e = data;
    e->recovering = FALSE;
    e->ready_sent = FALSE;
    e->warm_pending = FALSE;
    e->connected = FALSE;
    destroy_pipeline(e);
    emit_simple(e, "interrupted", NULL, "pipeline", "pipeline rebuilt after a fault; timeline cleared", 0);
    schedule_rebuild(e, 1000);
    return G_SOURCE_REMOVE;
}

static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data)
{
    (void)bus;
    Engine *e = data;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ELEMENT: {
        const GstStructure *s = gst_message_get_structure(message);
        guint64 pts;
        if (s && gst_structure_has_name(s, "admiral-presented") && gst_structure_get_uint64(s, "pts", &pts)) on_presented(e, pts);
        break;
    }
    case GST_MESSAGE_APPLICATION: {
        const GstStructure *s = gst_message_get_structure(message);
        if (!s || !gst_structure_has_name(s, "engine-note")) break;
        const gchar *type = gst_structure_get_string(s, "type"), *asset = gst_structure_get_string(s, "asset");
        guint64 play_id = 0; gst_structure_get_uint64(s, "play_id", &play_id);
        if (!g_strcmp0(type, "waiting")) emit_simple(e, "waiting", asset, "loading", "next asset not ready; continuing the current clip", play_id);
        else if (!g_strcmp0(type, "queued-failed")) emit_simple(e, "error", asset, "load-failed", "queued asset failed; continuing", play_id);
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *error = NULL; gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        g_warning("Engine pipeline error from %s: %s (%s)", GST_OBJECT_NAME(GST_MESSAGE_SRC(message)), error->message, debug ? debug : "");
        emit_simple(e, "error", NULL, "pipeline", error->message, 0);
        g_clear_error(&error); g_free(debug);
        /* Fault recovery only (not a per-clip path): rebuild with backoff, outside this
         * bus callback. */
        if (!e->recovering) { e->recovering = TRUE; g_idle_add(recover, e); }
        break;
    }
    case GST_MESSAGE_QOS: {
        gint64 jitter = 0; gdouble proportion = 1.0; gint quality = 0;
        gst_message_parse_qos_values(message, &jitter, &proportion, &quality);
        GstFormat format = GST_FORMAT_UNDEFINED; guint64 processed = 0, dropped = 0;
        gst_message_parse_qos_stats(message, &format, &processed, &dropped);
        e->qos_messages++;
        if (jitter > 5 * GST_MSECOND) e->qos_late_messages++;
        if (jitter > e->qos_max_jitter_ns) e->qos_max_jitter_ns = jitter;
        if (dropped != (guint64)-1 && GST_MESSAGE_SRC(message)) {
            if (!e->qos_dropped_by) e->qos_dropped_by = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            guint64 *slot = g_hash_table_lookup(e->qos_dropped_by, GST_OBJECT_NAME(GST_MESSAGE_SRC(message)));
            if (!slot) { slot = g_new0(guint64, 1); g_hash_table_insert(e->qos_dropped_by, g_strdup(GST_OBJECT_NAME(GST_MESSAGE_SRC(message))), slot); }
            if (dropped > *slot) { e->qos_dropped_total += dropped - *slot; *slot = dropped; }
            if (dropped) g_warning("Engine QoS: %s dropped=%" G_GUINT64_FORMAT " processed=%" G_GUINT64_FORMAT " jitter=%.1f ms",
                                   GST_OBJECT_NAME(GST_MESSAGE_SRC(message)), dropped, processed, jitter / 1e6);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        g_warning("Engine pipeline unexpectedly reached EOS");
        break;
    default:
        break;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ commands */
static void emit_ack(Engine *e, gint64 seq, const gchar *cmd, const gchar *client, gboolean ok, const gchar *code, const gchar *message, guint64 play_id)
{
    JsonBuilder *b = event_begin("ack");
    ADD_INT(b, "seq", seq);
    ADD_STR(b, "cmd", cmd);
    if (client) ADD_STR(b, "client", client);
    ADD_BOOL(b, "ok", ok);
    if (code) ADD_STR(b, "code", code);
    if (message) ADD_STR(b, "message", message);
    if (play_id) ADD_INT(b, "play_id", play_id);
    event_end(e, b);
}

static void emit_status(Engine *e, gint64 seq)
{
    JsonBuilder *b = event_begin("status");
    ADD_INT(b, "seq", seq);
    ADD_INT(b, "protocol", ENGINE_PROTOCOL);
    ADD_STR(b, "state", e->state);
    ADD_STR(b, "owner", e->owner);
    ADD_BOOL(b, "visible", e->visible);
    ADD_BOOL(b, "playing", e->playing_intent && e->visible);
    ADD_BOOL(b, "connected", e->connected);
    ADD_STR(b, "decoder", e->decoder_name);
    ADD_STR(b, "sink", e->test_sink ? "test-appsink" : "westerossink(engine)");
    ADD_INT(b, "alloc_queries", g_atomic_int_get(&e->alloc_queries));
    ADD_INT(b, "caps_events", g_atomic_int_get(&e->caps_events));
    ADD_INT(b, "presented_total", e->presented_total);
    ADD_INT(b, "rebuilds", e->rebuilds);
    {   /* what the late-frame policy actually is, and what it has done */
        GstElement *plane = e->pipeline ? gst_bin_get_by_name(GST_BIN(e->pipeline), "admiral-video-plane") : NULL;
        GstElement *policy = plane ? plane : e->sink;
        if (policy && GST_IS_BASE_SINK(policy)) {
            ADD_BOOL(b, "qos_enabled", gst_base_sink_is_qos_enabled(GST_BASE_SINK(policy)));
            ADD_INT(b, "max_lateness_ns", gst_base_sink_get_max_lateness(GST_BASE_SINK(policy)));
            ADD_BOOL(b, "sink_sync", gst_base_sink_get_sync(GST_BASE_SINK(policy)));
        }
        if (plane) gst_object_unref(plane);
        ADD_INT(b, "qos_messages", e->qos_messages);
        ADD_INT(b, "qos_late_messages", e->qos_late_messages);
        ADD_INT(b, "qos_dropped", e->qos_dropped_total);
        ADD_INT(b, "qos_max_jitter_ns", e->qos_max_jitter_ns);
        ADD_INT(b, "qos_events", g_atomic_int_get(&e->qos_events));
        ADD_INT(b, "qos_late_events", g_atomic_int_get(&e->qos_late_events));
        ADD_INT(b, "qos_event_max_jitter_ns", __atomic_load_n(&e->qos_event_max_jitter_ns, __ATOMIC_RELAXED));
    }
    if (e->have_rect) {
        json_builder_set_member_name(b, "rect"); json_builder_begin_array(b);
        for (int k = 0; k < 4; k++) json_builder_add_int_value(b, e->rect[k]);
        json_builder_end_array(b);
    }
    g_mutex_lock(&e->lock);
    if (e->cur) { ADD_STR(b, "feeding", e->cur->asset->id); ADD_BOOL(b, "idle", e->idle); }
    if (e->queued.active) { ADD_STR(b, "queued", e->queued.asset->id); ADD_STR(b, "queued_at", e->queued.at); }
    for (GList *l = e->fed.tail; l; l = l->prev) {
        Instance *i = l->data;
        if (i->started && !i->flushed && !i->warm) {
            ADD_STR(b, "presenting", i->asset->id); ADD_INT(b, "instance", i->id); ADD_INT(b, "play_id", i->play_id);
            ADD_INT(b, "presented", i->presented);
            break;
        }
    }
    g_mutex_unlock(&e->lock);
    json_builder_set_member_name(b, "cache"); json_builder_begin_object(b);
    ADD_INT(b, "budget", e->budget); ADD_INT(b, "used", e->used);
    json_builder_set_member_name(b, "assets"); json_builder_begin_array(b);
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, e->assets);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Asset *a = v;
        json_builder_begin_object(b);
        ADD_STR(b, "id", a->id); ADD_INT(b, "bytes", a->es_size); ADD_INT(b, "pins", a->pins);
        ADD_STR(b, "state", a->state == ASSET_READY ? "ready" : a->state == ASSET_LOADING ? "loading" : "failed");
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    event_end(e, b);
}

static void switch_now(Engine *e);
static gboolean load_done(gpointer data)
{
    Asset *a = data;
    Engine *e = &engine_singleton;
    gboolean cancelled = g_atomic_int_get(&a->cancelled), start_now = FALSE, flush_now = FALSE;
    g_mutex_lock(&e->lock);
    a->state = a->data && !cancelled ? ASSET_READY : ASSET_FAILED;
    if (e->queued.active && e->queued.asset == a) {
        if (a->state == ASSET_READY) {
            if (e->idle || !e->cur) start_now = TRUE;
            else if (!g_strcmp0(e->queued.at, "now")) flush_now = TRUE;
        } else pending_clear(&e->queued);
    }
    if (start_now) start_queued_locked(e);
    g_mutex_unlock(&e->lock);
    if (a->state == ASSET_FAILED) {
        if (a->in_table && g_hash_table_lookup(e->assets, a->id) == a) cache_drop(e, a);
        if (!cancelled) emit_simple(e, "error", a->id, a->code, a->error, 0);
    } else {
        JsonBuilder *b = event_begin("ready");
        ADD_STR(b, "asset", a->id); ADD_INT(b, "frames", a->n_aus); ADD_INT(b, "bytes", a->es_size);
        ADD_INT(b, "duration_ms", a->duration_ns / GST_MSECOND);
        event_end(e, b);
    }
    if (flush_now) switch_now(e);
    if (start_now || flush_now) kick(e);
    asset_unref(a);
    return G_SOURCE_REMOVE;
}

/* Flushing switch inside the one pipeline (same decoder and pool). Used for
 * play(at="now"), seek and stop. `next` may be NULL (stop: hold nothing). */
static gboolean flush_to(Engine *e, Instance *next, guint64 target_pts)
{
    g_mutex_lock(&e->lock);
    instance_free(e->seek_instance);
    e->seek_instance = next;
    e->seek_pending = TRUE;
    g_mutex_unlock(&e->lock);
    gboolean ok = gst_element_seek(e->pipeline, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                   GST_SEEK_TYPE_SET, (gint64)target_pts, GST_SEEK_TYPE_NONE, -1);
    if (!ok) {
        g_mutex_lock(&e->lock);
        if (e->seek_pending) { instance_free(e->seek_instance); e->seek_instance = NULL; e->seek_pending = FALSE; }
        g_mutex_unlock(&e->lock);
    }
    kick(e);
    return ok;
}

static Instance *new_flush_instance(Engine *e, Asset *a, guint first_au, guint64 play_id, const gchar *client, gboolean loop, guint loop_index)
{
    Instance *i = g_new0(Instance, 1);
    i->id = ++e->next_instance;
    i->play_id = play_id; i->asset = asset_ref(a); i->first_au = first_au;
    i->base = e->fed_end + a->duration_ns + GST_SECOND; /* unique, monotonic timeline */
    i->loop = loop; i->loop_index = loop_index; i->client = g_strdup(client);
    return i;
}

static void switch_now(Engine *e)
{
    g_mutex_lock(&e->lock);
    if (!e->queued.active || e->queued.asset->state != ASSET_READY) { g_mutex_unlock(&e->lock); return; }
    Pending q = e->queued;
    memset(&e->queued, 0, sizeof e->queued);
    Instance *i = new_flush_instance(e, q.asset, 0, q.play_id, q.client, q.loop, 0);
    q.asset->pins--; asset_unref(q.asset); g_free(q.at); g_free(q.client);
    guint64 target = i->base;
    g_mutex_unlock(&e->lock);
    if (!flush_to(e, i, target)) emit_simple(e, "error", NULL, "seek", "flush switch refused by the pipeline", 0);
}

static const gchar *member_string(JsonObject *o, const gchar *name)
{
    JsonNode *n = o ? json_object_get_member(o, name) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

static gint64 member_int(JsonObject *o, const gchar *name, gint64 fallback)
{
    JsonNode *n = o ? json_object_get_member(o, name) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return fallback;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64) return json_node_get_int(n);
    if (t == G_TYPE_DOUBLE) return (gint64)json_node_get_double(n);
    return fallback;
}

static gboolean member_bool(JsonObject *o, const gchar *name, gboolean fallback)
{
    JsonNode *n = o ? json_object_get_member(o, name) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN ? json_node_get_boolean(n) : fallback;
}

static void owner_gone(Engine *e, const gchar *why)
{
    g_free(e->owner); e->owner = NULL;
    e->visible = FALSE; e->playing_intent = FALSE;
    apply_visibility(e); apply_state(e);
    emit_simple(e, "owner-gone", NULL, why, "video hidden and paused; decoder retained", 0);
}

static void handle_command(Engine *e, const gchar *line)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        emit_ack(e, -1, "", NULL, FALSE, "invalid", "command must be a JSON object", 0);
        g_object_unref(parser); return;
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));
    const gchar *cmd = member_string(o, "cmd"), *client = member_string(o, "client");
    gint64 seq = member_int(o, "seq", -1);
    static guint64 play_counter;
    guint64 play_id = (guint64)MAX(0, member_int(o, "play_id", 0));
    gboolean needs_owner = cmd && (!strcmp(cmd, "play") || !strcmp(cmd, "pause") || !strcmp(cmd, "resume") || !strcmp(cmd, "seek") ||
                                   !strcmp(cmd, "show") || !strcmp(cmd, "hide") || !strcmp(cmd, "rect") || !strcmp(cmd, "stop"));
    if (!cmd) { emit_ack(e, seq, "", client, FALSE, "invalid", "missing cmd", 0); goto done; }
    if (needs_owner && e->owner && g_strcmp0(e->owner, client)) {
        emit_ack(e, seq, cmd, client, FALSE, "not-owner", "another page controls the video; send acquire first", 0); goto done;
    }
    if (needs_owner && !e->pipeline) { emit_ack(e, seq, cmd, client, FALSE, "not-ready", e->state, 0); goto done; }
    if (!strcmp(cmd, "hello") || !strcmp(cmd, "status")) {
        emit_ack(e, seq, cmd, client, TRUE, NULL, NULL, 0); emit_status(e, seq);
    } else if (!strcmp(cmd, "release")) {
        /* Explicit, documented exception: free the decoder (pipeline to NULL, broker generation
         * retires) so a legacy HTML <video> without a prepared variant may play. Non-gapless. */
        if (e->owner && g_strcmp0(e->owner, client)) { emit_ack(e, seq, cmd, client, FALSE, "not-owner", NULL, 0); goto done; }
        if (e->rebuild_source) { g_source_remove(e->rebuild_source); e->rebuild_source = 0; }
        e->warm_pending = FALSE; e->ready_sent = FALSE; e->connected = FALSE;
        e->visible = FALSE; e->playing_intent = FALSE;
        destroy_pipeline(e);
        e->released = TRUE;
        g_free(e->state); e->state = g_strdup("released");
        emit_ack(e, seq, cmd, client, TRUE, NULL, NULL, 0);
        emit_simple(e, "released", NULL, NULL, "decoder released; acquire restarts the engine with preflight", 0);
    } else if (!strcmp(cmd, "acquire")) {
        if (!client || !*client) { emit_ack(e, seq, cmd, client, FALSE, "invalid", "client required", 0); goto done; }
        if (e->released) {
            e->released = FALSE;
            g_free(e->state); e->state = g_strdup("starting");
            if (!e->rebuild_source) e->rebuild_source = g_idle_add(start_engine, e);
        }
        if (e->owner && g_strcmp0(e->owner, client)) {
            JsonBuilder *b = event_begin("preempted"); ADD_STR(b, "client", e->owner); ADD_STR(b, "by", client); event_end(e, b);
        }
        g_free(e->owner); e->owner = g_strdup(client);
        emit_ack(e, seq, cmd, client, TRUE, NULL, NULL, 0);
    } else if (!strcmp(cmd, "client-gone")) {
        gboolean was = e->owner && !g_strcmp0(e->owner, client);
        if (was) owner_gone(e, "client-gone");
        emit_ack(e, seq, cmd, client, TRUE, NULL, was ? "owner released" : "not the owner", 0);
    } else if (!strcmp(cmd, "prepare") || !strcmp(cmd, "play")) {
        JsonObject *asset_obj = json_object_has_member(o, "asset") ? json_object_get_object_member(o, "asset") : NULL;
        Asset *a = asset_get(e, member_string(asset_obj, "id"), member_string(asset_obj, "manifest"), FALSE);
        if (a->state == ASSET_FAILED) {
            emit_ack(e, seq, cmd, client, FALSE, a->code, a->error, 0);
            asset_unref(a); goto done;
        }
        if (!strcmp(cmd, "prepare")) {
            emit_ack(e, seq, cmd, client, TRUE, NULL, a->state == ASSET_READY ? "ready" : "loading", 0);
            if (a->state == ASSET_READY) {
                JsonBuilder *b = event_begin("ready"); ADD_STR(b, "asset", a->id); ADD_INT(b, "frames", a->n_aus);
                ADD_INT(b, "bytes", a->es_size); ADD_INT(b, "duration_ms", a->duration_ns / GST_MSECOND); event_end(e, b);
            }
            asset_unref(a); goto done;
        }
        if (!e->owner && client) e->owner = g_strdup(client);
        if (!play_id) play_id = ++play_counter + (1ULL << 40);
        const gchar *at = member_string(o, "at");
        if (!at || (strcmp(at, "boundary") && strcmp(at, "gop") && strcmp(at, "now"))) at = "boundary";
        gboolean loop = member_bool(o, "loop", FALSE), flush = FALSE;
        g_mutex_lock(&e->lock);
        if ((!e->cur || e->idle) && a->state == ASSET_READY) {
            pending_clear(&e->queued);
            start_instance_locked(e, a, 0, play_id, client, loop, 0);
        } else {
            if (e->queued.active) {
                JsonBuilder *b = event_begin("superseded"); ADD_STR(b, "asset", e->queued.asset->id);
                ADD_INT(b, "play_id", e->queued.play_id); event_end(e, b);
            }
            pending_clear(&e->queued);
            e->queued = (Pending){ .active = TRUE, .loop = loop, .asset = asset_ref(a), .at = g_strdup(at),
                                   .client = g_strdup(client), .play_id = play_id };
            a->pins++;
            flush = !strcmp(at, "now") && a->state == ASSET_READY && e->cur && !e->idle;
            if (a->state != ASSET_READY && (!e->cur || e->idle)) {
                /* nothing to continue: the first frame appears when loaded */
            }
        }
        g_mutex_unlock(&e->lock);
        e->playing_intent = TRUE;
        if (member_bool(o, "show", TRUE)) e->visible = TRUE;
        apply_visibility(e);
        apply_state(e);
        emit_ack(e, seq, cmd, client, TRUE, NULL, a->state == ASSET_READY ? NULL : "loading", play_id);
        if (a->state == ASSET_LOADING) emit_simple(e, "waiting", a->id, "loading", "asset loading; current presentation continues", play_id);
        if (flush) switch_now(e);
        kick(e);
        asset_unref(a);
    } else if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume")) {
        e->playing_intent = !strcmp(cmd, "resume");
        apply_state(e);
        emit_ack(e, seq, cmd, client, TRUE, NULL, NULL, 0);
    } else if (!strcmp(cmd, "show") || !strcmp(cmd, "hide")) {
        e->visible = !strcmp(cmd, "show");
        apply_visibility(e);
        apply_state(e);
        emit_ack(e, seq, cmd, client, TRUE, NULL, NULL, 0);
    } else if (!strcmp(cmd, "rect")) {
        gint64 x = member_int(o, "x", G_MININT32), y = member_int(o, "y", G_MININT32), w = member_int(o, "w", 0), h = member_int(o, "h", 0);
        if (x < -32768 || y < -32768 || x > 32768 || y > 32768 || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            emit_ack(e, seq, cmd, client, FALSE, "invalid", "rect needs integer x,y and positive w,h (physical UI pixels)", 0); goto done;
        }
        e->rect[0] = (gint)x; e->rect[1] = (gint)y; e->rect[2] = (gint)w; e->rect[3] = (gint)h; e->have_rect = TRUE;
        if (!e->test_sink && e->sink) {
            gchar *r = g_strdup_printf("%d,%d,%d,%d", e->rect[0], e->rect[1], e->rect[2], e->rect[3]);
            g_object_set(e->sink, "rectangle", r, NULL); g_free(r);
        }
        emit_ack(e, seq, cmd, client, TRUE, NULL, NULL, 0);
    } else if (!strcmp(cmd, "seek")) {
        gint64 position_ms = member_int(o, "position_ms", -1);
        g_mutex_lock(&e->lock);
        Instance *ref = NULL;
        for (GList *l = e->fed.tail; l && !ref; l = l->prev) {
            Instance *i = l->data;
            if (i->started && !i->flushed && !i->warm) ref = i;
        }
        if (!ref && e->cur && !e->cur->warm) ref = e->cur;
        Instance *next = NULL; guint64 target = 0;
        if (ref && position_ms >= 0) {
            Asset *a = ref->asset;
            guint au = (guint)MIN((guint64)a->n_aus - 1, gst_util_uint64_scale((guint64)position_ms * GST_MSECOND, a->fps_n, (guint64)a->fps_d * GST_SECOND));
            guint gop = au;
            while (gop > 0 && !a->aus[gop].key) gop--;
            next = new_flush_instance(e, a, gop, ref->play_id, ref->client, ref->loop, ref->loop_index);
            target = next->base + a->aus[au].pts;
        }
        g_mutex_unlock(&e->lock);
        if (!next) { emit_ack(e, seq, cmd, client, FALSE, "invalid", "nothing presented to seek, or bad position_ms", 0); goto done; }
        gboolean ok = flush_to(e, next, target);
        emit_ack(e, seq, cmd, client, ok, ok ? NULL : "seek", NULL, 0);
    } else if (!strcmp(cmd, "stop")) {
        g_mutex_lock(&e->lock); pending_clear(&e->queued); g_mutex_unlock(&e->lock);
        e->visible = FALSE; e->playing_intent = FALSE;
        apply_visibility(e);
        gboolean ok = flush_to(e, NULL, e->fed_end + GST_SECOND);
        apply_state(e);
        emit_ack(e, seq, cmd, client, ok, NULL, NULL, 0);
    } else if (!strcmp(cmd, "cancel")) {
        JsonObject *asset_obj = json_object_has_member(o, "asset") ? json_object_get_object_member(o, "asset") : NULL;
        const gchar *id = member_string(asset_obj, "id");
        gboolean found = FALSE;
        g_mutex_lock(&e->lock);
        if (id && e->queued.active && !g_strcmp0(e->queued.asset->id, id)) { pending_clear(&e->queued); found = TRUE; }
        g_mutex_unlock(&e->lock);
        Asset *a = id ? g_hash_table_lookup(e->assets, id) : NULL;
        if (a && a->pins == 0) {
            g_atomic_int_set(&a->cancelled, 1);
            cache_drop(e, a);
            found = TRUE;
        }
        emit_ack(e, seq, cmd, client, TRUE, NULL, found ? "cancelled" : "not queued", 0);
        if (found) emit_simple(e, "cancelled", id, NULL, NULL, 0);
    } else {
        emit_ack(e, seq, cmd, client, FALSE, "invalid", "unknown command", 0);
    }
done:
    g_object_unref(parser);
}

/* ------------------------------------------------------------------ control socket */
static void control_read(GObject *source, GAsyncResult *result, gpointer data);

static void control_close(Engine *e, Control *c)
{
    e->controls = g_list_remove(e->controls, c);
    g_io_stream_close(G_IO_STREAM(c->connection), NULL, NULL);
    g_object_unref(c->input); g_object_unref(c->connection); g_object_unref(c->cancel);
    g_free(c);
    if (!e->controls && e->owner) owner_gone(e, "controller-disconnected");
}

static void control_read(GObject *source, GAsyncResult *result, gpointer data)
{
    Control *c = data;
    Engine *e = &engine_singleton;
    gsize length = 0;
    GError *error = NULL;
    gchar *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &error);
    if (!line) {
        g_clear_error(&error);
        control_close(e, c);
        return;
    }
    if (length > MAX_LINE) emit_ack(e, -1, "", NULL, FALSE, "invalid", "line too long", 0);
    else if (length) handle_command(e, line);
    g_free(line);
    if (g_list_find(e->controls, c) && !g_cancellable_is_cancelled(c->cancel))
        g_data_input_stream_read_line_async(c->input, G_PRIORITY_DEFAULT, c->cancel, control_read, c);
    else if (g_list_find(e->controls, c)) control_close(e, c);
}

static gboolean control_incoming(GSocketService *service, GSocketConnection *connection, GObject *source, gpointer data)
{
    (void)service; (void)source;
    Engine *e = data;
    GSocket *socket = g_socket_connection_get_socket(connection);
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(g_socket_get_fd(socket), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    Control *c = g_new0(Control, 1);
    c->connection = g_object_ref(connection);
    c->input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_set_newline_type(c->input, G_DATA_STREAM_NEWLINE_TYPE_LF);
    c->cancel = g_cancellable_new();
    e->controls = g_list_append(e->controls, c);
    if (e->ready_sent) { e->only = c; emit_ready(e, e->warm_done); e->only = NULL; } /* late controllers learn readiness */
    g_data_input_stream_read_line_async(c->input, G_PRIORITY_DEFAULT, c->cancel, control_read, c);
    return TRUE;
}

static gboolean quit_signal(gpointer data) { g_main_loop_quit(((Engine *)data)->loop); return G_SOURCE_REMOVE; }

#ifndef ENGINE_NO_MAIN
int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    Engine *e = &engine_singleton;
    g_mutex_init(&e->lock);
    g_queue_init(&e->fed);
    e->socket_path = g_strdup(g_getenv("SIGNAGE_ENGINE_SOCKET") ? g_getenv("SIGNAGE_ENGINE_SOCKET") : "/run/admiral-signage/video/engine.sock");
    e->test_sink = !g_strcmp0(g_getenv("SIGNAGE_ENGINE_SINK"), "test");
    e->sync = env_u64("SIGNAGE_ENGINE_TEST_SYNC", 1) != 0 || !e->test_sink;
    e->decoder_name = g_strdup(g_getenv("SIGNAGE_ENGINE_DECODER") && *g_getenv("SIGNAGE_ENGINE_DECODER") ? g_getenv("SIGNAGE_ENGINE_DECODER") : "v4l2slh265dec");
    e->width = env_u64("SIGNAGE_ENGINE_WIDTH", 3840); e->height = env_u64("SIGNAGE_ENGINE_HEIGHT", 2160);
    e->fps_n = env_u64("SIGNAGE_ENGINE_FPS", 60); e->fps_d = 1;
    if (!e->test_sink && (e->width != 3840 || e->height != 2160 || e->fps_n != 60))
        g_warning("Engine profile %ux%u@%u differs from the native 3840x2160@60 signage profile", e->width, e->height, e->fps_n);
    e->budget = env_u64("SIGNAGE_ENGINE_CACHE_MB", 160) * 1024 * 1024;
    e->load_delay_ms = e->test_sink ? (guint)env_u64("SIGNAGE_ENGINE_TEST_LOAD_DELAY_MS", 0) : 0; /* TEST ONLY */
    const gchar *roots = g_getenv("SIGNAGE_ENGINE_ASSET_ROOTS");
    e->roots = g_strsplit(roots && *roots ? roots : "/opt/signage/media/engine:/data/engine", ":", -1);
    e->warm_manifest = g_strdup(g_getenv("SIGNAGE_ENGINE_WARMUP"));
    if (e->warm_manifest && !*e->warm_manifest) g_clear_pointer(&e->warm_manifest, g_free);
    e->compat_key = g_getenv("SIGNAGE_ENGINE_COMPAT_KEY") && *g_getenv("SIGNAGE_ENGINE_COMPAT_KEY") ? g_strdup(g_getenv("SIGNAGE_ENGINE_COMPAT_KEY")) : NULL;
    e->state = g_strdup("starting");
    e->idle = TRUE;
    e->visible = FALSE;
    const gchar *hash = g_getenv("SIGNAGE_ENGINE_TEST_HASH");
    if (e->test_sink && hash && *hash) e->hash_file = g_fopen(hash, "w");
    e->assets = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)asset_unref);
    e->loader = g_thread_pool_new(loader_thread, e, 1, FALSE, NULL);
    e->loop = g_main_loop_new(NULL, FALSE);
    g_unlink(e->socket_path);
    GSocketAddress *address = g_unix_socket_address_new(e->socket_path);
    e->service = g_socket_service_new();
    GError *error = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(e->service), address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        g_printerr("Engine cannot listen on %s: %s\n", e->socket_path, error->message);
        return 1;
    }
    g_object_unref(address);
    g_chmod(e->socket_path, 0600);
    g_signal_connect(e->service, "incoming", G_CALLBACK(control_incoming), e);
    g_socket_service_start(e->service);
    g_unix_signal_add(SIGTERM, quit_signal, e);
    g_unix_signal_add(SIGINT, quit_signal, e);
    g_message("Persistent video engine: decoder=%s sink=%s profile=%ux%u@%u cache=%" G_GUINT64_FORMAT " MiB socket=%s",
              e->decoder_name, e->test_sink ? "test-appsink" : "westerossink(engine)", e->width, e->height, e->fps_n,
              e->budget >> 20, e->socket_path);
    g_idle_add(start_engine, e);
    g_main_loop_run(e->loop);
    g_socket_service_stop(e->service);
    while (e->controls) control_close(e, e->controls->data);
    if (e->rebuild_source) g_source_remove(e->rebuild_source);
    destroy_pipeline(e);
    g_thread_pool_free(e->loader, TRUE, TRUE);
    g_hash_table_unref(e->assets);
    g_object_unref(e->service);
    g_unlink(e->socket_path);
    if (e->hash_file) fclose(e->hash_file);
    g_main_loop_unref(e->loop);
    g_strfreev(e->roots);
    g_free(e->socket_path); g_free(e->decoder_name); g_free(e->compat_key); g_free(e->warm_manifest);
    g_free(e->owner); g_free(e->state);
    return 0;
}
#endif
