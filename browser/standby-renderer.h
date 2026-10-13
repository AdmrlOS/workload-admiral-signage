/* SPDX-License-Identifier: MIT
 * Native renderer reservation. Included in the shell after export/copy helpers.
 * UI and video are tested and activated by the same existing DRM committer.
 */
static gboolean initialize_webkit(Browser *browser);

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
        destroy_stage_view(stage); browser->staged_view=NULL; stage=NULL;
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
            destroy_stage_view(browser->retiring_view); browser->retiring_view=NULL;
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
}

static gboolean standby_capacity(guint64 *available,guint64 *required,unsigned *contexts)
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
    if (!strcmp(command,"native-stage")) {
        browser->stage_request=json_object_has_member(obj,"request") ? json_object_get_int_member(obj,"request"):0;
        const char *uri=json_object_has_member(obj,"uri") ? json_object_get_string_member(obj,"uri"):NULL;
        guint64 available=0,required=0; unsigned contexts=0;
        GError *error=NULL; GUri *target=uri ? g_uri_parse(uri,G_URI_FLAGS_NONE,&error):NULL;
        gboolean local=target && !g_strcmp0(g_uri_get_scheme(target),"http") &&
            !g_strcmp0(g_uri_get_host(target),"127.0.0.1") && g_uri_get_port(target)==8080 && !g_uri_get_userinfo(target);
        guint64 generation=0;
        if (local && !browser->staged_view && !browser->retiring_view && gate->phase==SIGNAGE_STANDBY_EMPTY &&
            browser->ui_width==3840 && browser->ui_height==2160 && browser->ui_scale==1 &&
            mode_refresh(&browser->mode)==60 && standby_capacity(&available,&required,&contexts))
            generation=signage_drm_broker_video_reserve();
        if (!generation || !signage_standby_prepare(gate,signage_drm_broker_video_generation(),generation,contexts,available,required)) {
            if (generation) signage_drm_broker_video_retired(generation);
            native_notice(browser,"not-ready",0);
        } else {
            VideoState *endpoint=&browser->video.endpoint;
            if (endpoint->generation==gate->active && endpoint->fb)
                signage_standby_endpoint(gate,endpoint->generation,endpoint->serial,endpoint->fb,
                    endpoint->flipped_us,browser->video.endpoint_interval);
            Browser *stage=g_new0(Browser,1);
            *stage=*browser;
            stage->retry_source=stage->fence_source=stage->load_source=0;
            stage->load_attempts=0; stage->stats_source=stage->stage_tick=0;
            stage->root=browser; stage->view=NULL; stage->exportable=NULL; stage->backend=NULL;
            stage->current=stage->pending=stage->queued=NULL; stage->copy_retired=NULL;
            stage->copy=(GpuCopy){.context=EGL_NO_CONTEXT}; stage->render_fd=-1;
            stage->handles=g_hash_table_ref(browser->handles); stage->failure=NULL;
            stage->uri=g_strdup_printf("%s%csignage_standby=%" G_GUINT64_FORMAT,uri,strchr(uri,'?') ? '&':'?',generation);
            browser->staged_view=stage; browser->stage_ui_ready=FALSE; browser->activation_requested=FALSE;
            browser->stage_deadline=g_get_monotonic_time()+15*G_USEC_PER_SEC;
            if (!initialize_webkit(stage)) {
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
            destroy_stage_view(browser->staged_view); browser->staged_view=NULL;
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
    const char *source="window.signageNative={send:m=>window.webkit.messageHandlers.signage.postMessage(JSON.stringify(m))};";
    WebKitUserScript *script=webkit_user_script_new(source,WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,NULL,NULL);
    webkit_user_content_manager_add_script(manager,script); webkit_user_script_unref(script);
}
