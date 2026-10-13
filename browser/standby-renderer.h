/* SPDX-License-Identifier: MIT
 * Native renderer reservation. Included in the shell after export/copy helpers.
 * UI and video are tested and activated by the same existing DRM committer.
 */
static gboolean initialize_webkit(Browser *browser);

/* Pooled navigation: the only WebKit call allowed on the main loop while the
 * media clock runs. Timed against PLAYBACK_LOAD_BUDGET_US and recorded. */
static void pooled_load_uri(Browser *view)
{
    gint64 started=g_get_monotonic_time();
    webkit_web_view_load_uri(view->view,view->uri);
    gint64 elapsed=g_get_monotonic_time()-started;
    playback_guard.loads_total++;
    if (!playback_guard.running) return;
    playback_guard.loads_in_playback++;
    if (elapsed>playback_guard.max_load_us) playback_guard.max_load_us=elapsed;
    if (elapsed>PLAYBACK_LOAD_BUDGET_US) {
        playback_guard.loads_over_budget++;
        g_warning("Playback guard: pooled load_uri took %" G_GINT64_FORMAT " us (budget %d)",elapsed,PLAYBACK_LOAD_BUDGET_US);
    }
}

/* A staged view that never became visible owns no scanout state and keeps its
 * WebProcess: return it to the pool instead of destroying it (a destroyed pool
 * member forces a later in-playback WebView creation). */
static void recycle_stage_view(Browser *browser, Browser *view);
static void retain_or_destroy_staged(Browser *browser, Browser *view)
{
    if (!view) return;
    if (view->current || view->pending || view->copy_retired || view->retry_source ||
        view->fence_source || view->load_source || browser->spare_view) { destroy_stage_view(view); return; }
    playback_guard.cancel_recycled++;
    recycle_stage_view(browser,view);
}

static void recycle_stage_view(Browser *browser, Browser *view)
{
    if (!view) return;
    if (browser->spare_view) { destroy_stage_view(view); return; }
    /* Called only after exact UI/video scanout retirement on the owning main
     * context. Retain one WebView/backend, release its decoder via page JS. */
    view->current=view->pending=NULL;
    release_frame(view,view->queued); view->queued=NULL;
    webkit_web_view_evaluate_javascript(view->view,
        "document.getAnimations().forEach(a=>a.pause());document.querySelectorAll('video,audio').forEach(v=>{v.pause();v.removeAttribute('src');v.load()})",-1,NULL,NULL,NULL,NULL,NULL);
    wpe_view_backend_remove_activity_state(view->backend,wpe_view_activity_state_visible | wpe_view_activity_state_focused);
    browser->spare_view=view;
}


static void native_notice(Browser *browser, const char *event, guint64 generation)
{
    char *script = g_strdup_printf("window.dispatchEvent(new CustomEvent('signage-native',{detail:{event:'%s',generation:'%" G_GUINT64_FORMAT "',request:%" G_GUINT64_FORMAT "}}))", event, generation, browser->stage_request);
    webkit_web_view_evaluate_javascript(browser->view, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
}

static gboolean generation_busy(Browser *browser, guint64 generation)
{
    VideoPlane *v=&browser->video;
    if ((v->current.fb && v->current.generation==generation) ||
        (v->pending_valid && v->pending.fb && v->pending.generation==generation) ||
        (v->dirty && v->next.fb && v->next.generation==generation) ||
        (v->standby.fb && v->standby.generation==generation)) return TRUE;
    for (GList *it=v->waiting.head;it;it=it->next)
        if (((VideoState *)it->data)->generation==generation) return TRUE;
    return FALSE;
}

static gboolean test_standby(Browser *browser, Frame *ui, const VideoState *next)
{
    VideoPlane *video=&browser->video;
    drmModeAtomicReq *request=drmModeAtomicAlloc();
    if (!request) return FALSE;
    gboolean valid=add_ui_state(browser,request,ui);
    ADD(video->plane,video->fb,next->fb);
    ADD(video->plane,video->crtc,next->fb ? browser->crtc : 0);
    ADD(video->plane,video->src_x,next->src_x); ADD(video->plane,video->src_y,next->src_y);
    ADD(video->plane,video->src_w,next->src_w); ADD(video->plane,video->src_h,next->src_h);
    ADD(video->plane,video->crtc_x,(uint64_t)(int64_t)next->crtc_x);
    ADD(video->plane,video->crtc_y,(uint64_t)(int64_t)next->crtc_y);
    ADD(video->plane,video->crtc_w,next->crtc_w); ADD(video->plane,video->crtc_h,next->crtc_h);
    valid=valid && drmModeAtomicCommit(browser->fd,request,DRM_MODE_ATOMIC_TEST_ONLY,NULL)==0;
    drmModeAtomicFree(request); return valid;
}

static void standby_progress(Browser *browser)
{
    VideoPlane *v=&browser->video;
    SignageStandby *g=&v->gate;
    Browser *stage=browser->staged_view;
    if (stage && g->phase==SIGNAGE_STANDBY_PREPARING && browser->stage_deadline &&
        g_get_monotonic_time()>browser->stage_deadline) {
        browser->stage_deadline=0;
        native_notice(browser,"not-ready",g->standby);
        return; /* Never turn a readiness timeout into TEST_ONLY/presentation. */
    }
    if (browser->cancelled_generation && g->phase==SIGNAGE_STANDBY_EMPTY) {
        guint64 cancelled=browser->cancelled_generation;
        browser->cancelled_generation=0;
        retain_or_destroy_staged(browser,stage); browser->staged_view=NULL; stage=NULL;
        if (!signage_drm_broker_video_connected(cancelled)) signage_drm_broker_video_retired(cancelled);
    }
    if (browser->dead_generation && browser->dead_generation != signage_drm_broker_video_generation() &&
        !generation_busy(browser,browser->dead_generation)) {
        signage_drm_broker_video_retired(browser->dead_generation); browser->dead_generation=0;
    }
    if (g->phase==SIGNAGE_STANDBY_RETIRING && !generation_busy(browser,g->active) &&
        signage_drm_broker_video_tx_idle(g->active)) {
        guint64 outgoing=g->active;
        signage_standby_retired(g,outgoing);
        /* Do not close the outgoing socket here: RELEASED may still be partially
         * written. Its owner tears down after activated, and EOF frees its slot. */
        if (!signage_drm_broker_video_connected(outgoing)) signage_drm_broker_video_retired(outgoing);
        if (browser->retiring_view) {
            recycle_stage_view(browser, browser->retiring_view); browser->retiring_view=NULL;
        }
    }
    Browser *retiring=browser->retiring_view;
    if (retiring && g->phase==SIGNAGE_STANDBY_EMPTY &&
        (!browser->current || browser->current->owner!=retiring) &&
        (!browser->pending || browser->pending->owner!=retiring) &&
        (!browser->queued || browser->queued->owner!=retiring) && !v->current.fb && !v->pending_valid) {
        destroy_stage_view(retiring); browser->retiring_view=NULL;
    }
    if (!stage || !stage->queued || !browser->stage_ui_ready || !frame_ready(stage->queued)) return;
    if (g->phase==SIGNAGE_STANDBY_PREPARING && v->standby.fb &&
        test_standby(browser,stage->queued,&v->standby) &&
        signage_standby_ready(g,v->standby.generation,v->standby.serial,v->standby.fb,TRUE)) {
        browser->stage_deadline=0;
        browser->stage_ui_generation++;
        signage_drm_broker_video_ack(g->standby,SIGNAGE_VIDEO_PREPARED,v->standby.fb,v->standby.serial);
        native_notice(browser,"prepared",g->standby);
    }
    if (!browser->activation_requested || browser->commit_pending || v->dirty ||
        !g_queue_is_empty(&v->waiting) || g->phase!=SIGNAGE_STANDBY_READY) return;
    if (!test_standby(browser,stage->queued,&v->standby)) return;
    if (!signage_standby_activate(g,g->active,g->final_serial,g->final_fb,g->standby,
        g_get_monotonic_time(),browser->stage_ui_generation,TRUE)) return;
    if (browser->queued) release_frame(browser,browser->queued);
    browser->queued=stage->queued; stage->queued=NULL;
    v->next=v->standby; v->standby=(VideoState){0}; v->dirty=TRUE;
    browser->activation_requested=FALSE;
    submit_commit(browser);
}

static gboolean standby_tick(gpointer data)
{
    Browser *browser=data;
    standby_progress(browser);
    return G_SOURCE_CONTINUE;
}

static void destroy_stage_view(Browser *view)
{
    if (!view) return;
    gint64 teardown_started=g_get_monotonic_time();
    g_message("Native retirement begin mono=%" G_GINT64_FORMAT,teardown_started);
    /* Only called after scanout references owned by this view are gone. */
    if (view->retry_source) g_source_remove(view->retry_source);
    if (view->fence_source) g_source_remove(view->fence_source);
    if (view->load_source) g_source_remove(view->load_source);
    if (view->view) { webkit_web_view_stop_loading(view->view); g_object_unref(view->view); }
    release_frame(view,view->queued); view->queued=NULL;
    if (view->copy_retired) {
        SavedContext saved;
        if (!copy_context_enter(view,&saved)) return; /* fail safe retain */
        glFinish(); copy_context_leave(view,&saved);
        for (GList *it=view->copy_retired;it;it=it->next) release_frame(view,it->data);
        g_list_free(view->copy_retired); view->copy_retired=NULL;
    }
    if (view->fence_source) { g_source_remove(view->fence_source); view->fence_source=0; }
    destroy_copy(view);
    if (view->exportable) wpe_view_backend_exportable_fdo_destroy(view->exportable);
    if (view->handles) g_hash_table_unref(view->handles);
    g_free((char *)view->uri); g_free(view->failure); g_free(view);
    g_message("Native retirement end mono=%" G_GINT64_FORMAT " elapsed_us=%" G_GINT64_FORMAT,g_get_monotonic_time(),g_get_monotonic_time()-teardown_started);
}

static gboolean standby_capacity(const char *uri,guint64 *available,guint64 *required,unsigned *contexts)
{
    /* Hardware certification supplies the measured *additional* decoder DPB,
     * UI scanout/copy pool and allocator headroom bound. Unknown fails closed. */
    const char *c=g_getenv("SIGNAGE_STANDBY_CONTEXTS");
    const char *r=g_getenv("SIGNAGE_STANDBY_CMA_BYTES");
    if (!c || !r) return FALSE;
    char *end=NULL;
    guint64 count=g_ascii_strtoull(c,&end,10);
    if (!*c || *c=='-' || *end || count<2 || count>G_MAXUINT) return FALSE;
    *contexts=count;
    *required=g_ascii_strtoull(r,&end,10);
    if (!*r || *r=='-' || *end || !*required) return FALSE;
    const char *bounds=g_getenv("SIGNAGE_STANDBY_CMA_BY_PATH");
    if (bounds) {
        JsonParser *parser=json_parser_new();
        GUri *target=g_uri_parse(uri,G_URI_FLAGS_NONE,NULL);
        gboolean valid=target && json_parser_load_from_data(parser,bounds,-1,NULL);
        JsonNode *node=valid ? json_parser_get_root(parser):NULL;
        JsonObject *obj=node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node):NULL;
        const char *path=target ? g_uri_get_path(target):NULL;
        JsonNode *bound=obj && path && json_object_has_member(obj,path) ? json_object_get_member(obj,path):NULL;
        valid=bound && JSON_NODE_HOLDS_VALUE(bound) && json_node_get_value_type(bound)==G_TYPE_INT64;
        gint64 bytes=valid ? json_node_get_int(bound):0;
        if (target) g_uri_unref(target);
        g_object_unref(parser);
        if (bytes<=0) return FALSE;
        *required=(guint64)bytes;
    }
    char *mem=NULL;
    if (!g_file_get_contents("/proc/meminfo",&mem,NULL,NULL)) return FALSE;
    const char *free=strstr(mem,"CmaFree:"); unsigned long long kib=0;
    gboolean ok=free && sscanf(free,"CmaFree: %llu kB",&kib)==1 && kib<=G_MAXUINT64/1024;
    *available=ok ? kib*1024:0; g_free(mem);
    return ok && *available>=*required;
}

static void native_command(WebKitUserContentManager *manager G_GNUC_UNUSED, JSCValue *value, gpointer data)
{
    Browser *sender=data, *browser=sender->root ? sender->root:sender;
    char *text=jsc_value_to_string(value);
    JsonParser *parser=json_parser_new();
    if (!json_parser_load_from_data(parser,text,-1,NULL)) { g_free(text); g_object_unref(parser); return; }
    g_free(text);
    JsonNode *node=json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(node)) { g_object_unref(parser); return; }
    JsonObject *obj=json_node_get_object(node);
    const char *command=json_object_has_member(obj,"type") ? json_object_get_string_member(obj,"type"):"";
    g_message("Native bridge: command=%s stage=%d phase=%d",command,sender->root != NULL,browser->video.gate.phase);
    SignageStandby *gate=&browser->video.gate;
    if (sender->root) {
        if (!strcmp(command,"ui-ready") && browser->staged_view==sender) {
            browser->stage_ui_ready=TRUE;
            if (sender->queued) { release_frame(sender,sender->queued); sender->queued=NULL; }
            webkit_web_view_evaluate_javascript(sender->view,
                "document.documentElement.style.outline='1px solid transparent';", -1, NULL, NULL, NULL, NULL, NULL);
            standby_progress(browser);
        } else if (!strcmp(command,"ended") && browser->visible_view==sender) {
            native_notice(browser,"ended",signage_drm_broker_video_generation());
        }
        g_object_unref(parser); return;
    }
    if (!strcmp(command,"native-warm-pool")) {
        if (!browser->spare_view && browser->staged_view && !browser->visible_view) {
            Browser *warm=g_new0(Browser,1); *warm=*browser;
            warm->root=browser; warm->view=NULL; warm->exportable=NULL; warm->backend=NULL;
            warm->current=warm->pending=warm->queued=NULL; warm->copy_retired=NULL;
            warm->retry_source=warm->fence_source=warm->load_source=0;
            warm->stats_source=warm->stage_tick=0; warm->failure=NULL; warm->render_fd=-1;
            warm->copy=(GpuCopy){.context=EGL_NO_CONTEXT};
            warm->handles=g_hash_table_ref(browser->handles); warm->uri=g_strdup("about:blank");
            browser->spare_view=warm;
            if (!initialize_webkit(warm)) { browser->spare_view=NULL; destroy_stage_view(warm); }
        }
    } else if (!strcmp(command,"native-playback")) {
        playback_guard.running=json_object_has_member(obj,"running") && json_object_get_boolean_member(obj,"running");
        playback_guard.strict=!g_strcmp0(g_getenv("SIGNAGE_PLAYBACK_WEBKIT"),"strict");
        g_message("Playback guard: media clock %s strict=%d pool_spare=%d",playback_guard.running ? "running":"stopped",
                  playback_guard.strict,browser->spare_view!=NULL);
        write_status(browser);
    } else if (!strcmp(command,"native-stage")) {
        browser->stage_request=json_object_has_member(obj,"request") ? json_object_get_int_member(obj,"request"):0;
        const char *uri=json_object_has_member(obj,"uri") ? json_object_get_string_member(obj,"uri"):NULL;
        guint64 available=0,required=0; unsigned contexts=0;
        GError *error=NULL; GUri *target=uri ? g_uri_parse(uri,G_URI_FLAGS_NONE,&error):NULL;
        gboolean local=target && !g_strcmp0(g_uri_get_scheme(target),"http") &&
            !g_strcmp0(g_uri_get_host(target),"127.0.0.1") && g_uri_get_port(target)==8080 && !g_uri_get_userinfo(target);
        guint64 generation=0;
        if (playback_guard.running && playback_guard.strict && !browser->spare_view &&
            !browser->staged_view && !browser->retiring_view && gate->phase==SIGNAGE_STANDBY_EMPTY) {
            /* Pool exhausted during playback: explicit failed-ready, never a
             * synchronous WebView creation on the presentation owner. */
            playback_guard.refused_in_playback++;
            g_warning("Playback guard: no pooled WebView; refusing in-playback creation");
            native_notice(browser,"not-ready",0);
            if (target) g_uri_unref(target);
            g_clear_error(&error);
            g_object_unref(parser); return;
        }
        if (local && !browser->staged_view && !browser->retiring_view && gate->phase==SIGNAGE_STANDBY_EMPTY &&
            browser->ui_width==3840 && browser->ui_height==2160 && browser->ui_scale==1 &&
            mode_refresh(&browser->mode)==60 && standby_capacity(uri,&available,&required,&contexts))
            generation=signage_drm_broker_video_reserve();
        if (!generation || !signage_standby_prepare(gate,signage_drm_broker_video_generation(),generation,contexts,available,required)) {
            if (generation) signage_drm_broker_video_retired(generation);
            native_notice(browser,"not-ready",0);
        } else {
            VideoState *endpoint=&browser->video.endpoint;
            if (endpoint->generation==gate->active && endpoint->fb)
                signage_standby_endpoint(gate,endpoint->generation,endpoint->serial,endpoint->fb,
                    endpoint->flipped_us,browser->video.endpoint_interval);
            Browser *stage=browser->spare_view; browser->spare_view=NULL;
            gboolean reused=stage!=NULL;
            if (!stage) {
            stage=g_new0(Browser,1);
            *stage=*browser;
            stage->retry_source=stage->fence_source=stage->load_source=0;
            stage->load_attempts=0; stage->stats_source=stage->stage_tick=0;
            stage->root=browser; stage->view=NULL; stage->exportable=NULL; stage->backend=NULL;
            stage->current=stage->pending=stage->queued=NULL; stage->copy_retired=NULL;
            stage->copy=(GpuCopy){.context=EGL_NO_CONTEXT}; stage->render_fd=-1;
            stage->handles=g_hash_table_ref(browser->handles); stage->failure=NULL;
            } else { g_free((char *)stage->uri); stage->last_visible_export=0; }
            stage->uri=g_strdup_printf("%s%csignage_standby=%" G_GUINT64_FORMAT,uri,strchr(uri,'?') ? '&':'?',generation);
            browser->staged_view=stage; browser->stage_ui_ready=FALSE; browser->activation_requested=FALSE;
            browser->stage_deadline=g_get_monotonic_time()+15*G_USEC_PER_SEC;
            gint64 initialize_started=g_get_monotonic_time();
            gboolean initialized=TRUE;
            if (reused) {
                wpe_view_backend_add_activity_state(stage->backend,wpe_view_activity_state_visible | wpe_view_activity_state_in_window);
                pooled_load_uri(stage);
            }
            else initialized=initialize_webkit(stage);
            if (g_getenv("SIGNAGE_TRANSITION_TRACE"))
                g_message("Transition initialize mono=%" G_GINT64_FORMAT " elapsed_us=%" G_GINT64_FORMAT,initialize_started,g_get_monotonic_time()-initialize_started);
            if (!initialized) {
                destroy_stage_view(stage); browser->staged_view=NULL;
                signage_standby_cancel(gate,generation); signage_drm_broker_video_retired(generation);
                native_notice(browser,"not-ready",generation);
            }
        }
        if (target) g_uri_unref(target);
        g_clear_error(&error);
    } else if (!strcmp(command,"native-activate")) {
        guint64 generation=json_object_has_member(obj,"generation") ? g_ascii_strtoull(json_object_get_string_member(obj,"generation"),NULL,10):0;
        if (generation==gate->standby && gate->phase==SIGNAGE_STANDBY_READY) {
            browser->activation_requested=TRUE; standby_progress(browser);
        } else native_notice(browser,"not-ready",generation);
    } else if (!strcmp(command,"native-reset")) {
        if (gate->phase==SIGNAGE_STANDBY_COMMITTING || gate->phase==SIGNAGE_STANDBY_RETIRING) {
            native_notice(browser,"not-ready",gate->standby);
        } else {
            if (browser->visible_view) {
                Browser *visible=browser->visible_view;
                webkit_web_view_evaluate_javascript(visible->view,
                    "document.querySelectorAll('video,audio').forEach(v=>{v.pause();v.removeAttribute('src');v.load()})",-1,NULL,NULL,NULL,NULL,NULL);
                browser->retiring_view=visible; browser->visible_view=NULL;
            }
            browser->orchestrator_hidden=FALSE;
        }
    } else if (!strcmp(command,"native-cancel")) {
        guint64 generation=gate->standby;
        if (signage_standby_cancel(gate,generation)) {
            uint32_t fb=browser->video.standby.fb,serial=browser->video.standby.serial;
            browser->video.standby=(VideoState){0};
            signage_drm_broker_video_ack(generation,SIGNAGE_VIDEO_CANCELLED,fb,serial);
            video_release_if_unused(browser,fb);
            retain_or_destroy_staged(browser,browser->staged_view); browser->staged_view=NULL;
            if (!signage_drm_broker_video_connected(generation)) signage_drm_broker_video_retired(generation);
            /* Peer EOF, not cancellation, retires the socket after ACK drain. */
            native_notice(browser,"cancelled",generation);
        }
    }
    g_object_unref(parser);
}

static void configure_native_bridge(Browser *browser)
{
    WebKitUserContentManager *manager=webkit_web_view_get_user_content_manager(browser->view);
    webkit_user_content_manager_register_script_message_handler(manager,"signage",NULL);
    g_signal_connect(manager,"script-message-received::signage",G_CALLBACK(native_command),browser);
    const char *source="window.signageNative={preflight:true,send:m=>window.webkit.messageHandlers.signage.postMessage(JSON.stringify(m))};";
    WebKitUserScript *script=webkit_user_script_new(source,WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,NULL,NULL);
    webkit_user_content_manager_add_script(manager,script); webkit_user_script_unref(script);
}
