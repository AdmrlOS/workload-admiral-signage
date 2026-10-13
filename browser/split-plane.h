/* SPDX-License-Identifier: MIT
 * Private KMS adapter, included after Browser and property helpers. */
#ifndef SIGNAGE_SPLIT_PLANE_H
#define SIGNAGE_SPLIT_PLANE_H

static const char *const split_property_names[] = {
    "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
    "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "zpos",
    "pixel blend mode", "alpha", "IN_FENCE_FD"
};

static gboolean plane_modifier(int fd, uint32_t plane, uint32_t format, uint64_t modifier)
{
    uint64_t blob_id = 0;
    if (!property_id(fd, plane, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id)) return FALSE;
    drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(fd, blob_id);
    gboolean found = FALSE;
    if (blob && blob->length >= sizeof(struct drm_format_modifier_blob)) {
        const struct drm_format_modifier_blob *b = blob->data;
        uint64_t fend = (uint64_t)b->formats_offset + (uint64_t)b->count_formats * sizeof(uint32_t);
        uint64_t mend = (uint64_t)b->modifiers_offset + (uint64_t)b->count_modifiers * sizeof(struct drm_format_modifier);
        if (fend <= blob->length && mend <= blob->length) {
            const uint32_t *formats = (const void *)((const char *)b + b->formats_offset);
            const struct drm_format_modifier *mods = (const void *)((const char *)b + b->modifiers_offset);
            for (uint32_t i = 0; i < b->count_modifiers; i++) {
                if (mods[i].modifier != modifier) continue;
                for (uint32_t j = 0; j < 64; j++) {
                    uint64_t index = (uint64_t)mods[i].offset + j;
                    if ((mods[i].formats & (1ULL << j)) && index < b->count_formats && formats[index] == format)
                        found = TRUE;
                }
            }
        }
    }
    if (blob) drmModeFreePropertyBlob(blob);
    return found;
}

static gboolean split_property_value(int fd, uint32_t plane, const char *name,
                                     const char *enum_name, uint64_t target,
                                     gboolean maximum, uint64_t *value)
{
    uint64_t current = 0;
    uint32_t id = property_id(fd, plane, DRM_MODE_OBJECT_PLANE, name, &current);
    drmModePropertyRes *p = id ? drmModeGetProperty(fd, id) : NULL;
    gboolean valid = FALSE;
    if (!p) return FALSE;
    if (enum_name) {
        for (int i = 0; i < p->count_enums; i++)
            if (!strcmp(p->enums[i].name, enum_name)) { target = p->enums[i].value; valid = TRUE; }
    } else if ((p->flags & DRM_MODE_PROP_RANGE) && p->count_values >= 2) {
        if (maximum) target = p->values[1];
        valid = target >= p->values[0] && target <= p->values[1];
    }
    if ((p->flags & DRM_MODE_PROP_IMMUTABLE) && current != target) valid = FALSE;
    if (valid) *value = target;
    drmModeFreeProperty(p);
    return valid;
}

static void select_split_plane(Browser *browser)
{
    if (!signage_split_geometry(browser->ui_width, browser->ui_height,
                               browser->mode.hdisplay, browser->mode.vdisplay, &browser->split_geometry)) return;
    const uint64_t modifier = 0x0800000000000072ULL;
    if (!plane_modifier(browser->fd, browser->primary, DRM_FORMAT_ARGB8888, modifier)) return;
    uint64_t primary_z = 0, video_z = 0;
    if (!property_id(browser->fd, browser->primary, DRM_MODE_OBJECT_PLANE, "zpos", &primary_z)) return;
    if (browser->video.plane && !property_id(browser->fd, browser->video.plane, DRM_MODE_OBJECT_PLANE, "zpos", &video_z)) return;
    drmModePlaneRes *planes = drmModeGetPlaneResources(browser->fd);
    for (uint32_t i = 0; planes && i < planes->count_planes && !browser->split_plane; i++) {
        uint32_t id = planes->planes[i];
        if (id == browser->primary || id == browser->video.plane) continue;
        drmModePlane *p = drmModeGetPlane(browser->fd, id);
        uint64_t attached = 0, type = 0;
        gboolean ok = p && (p->possible_crtcs & (1u << browser->crtc_index)) &&
            supports_format(p, DRM_FORMAT_ARGB8888) && !supports_format(p, DRM_FORMAT_NV12) &&
            property_id(browser->fd, id, DRM_MODE_OBJECT_PLANE, "type", &type) && signage_split_plane_type(type) &&
            property_id(browser->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &attached) && !attached &&
            plane_modifier(browser->fd, id, DRM_FORMAT_ARGB8888, modifier);
        if (p) drmModeFreePlane(p);
        uint64_t z = 0, blend = 0, alpha = 0;
        uint64_t target_z = browser->video.plane ? video_z + 1 : primary_z;
        ok = ok && split_property_value(browser->fd, id, "zpos", NULL, target_z, FALSE, &z) &&
            (!browser->video.plane || z > video_z) &&
            split_property_value(browser->fd, id, "pixel blend mode", "Pre-multiplied", 0, FALSE, &blend);
        uint32_t props[14] = {0};
        for (unsigned j = 0; ok && j < G_N_ELEMENTS(props); j++) {
            props[j] = property_id(browser->fd, id, DRM_MODE_OBJECT_PLANE, split_property_names[j], NULL);
            if (j < 12 && !props[j]) ok = FALSE;
        }
        if (ok && props[12]) ok = split_property_value(browser->fd, id, "alpha", NULL, 0, TRUE, &alpha);
        if (ok) {
            browser->split_plane = id;
            memcpy(browser->split_props, props, sizeof props);
            browser->split_zpos = z; browser->split_blend = blend; browser->split_alpha = alpha;
            g_message("Native AFBC UI views selected on planes %u/%u; one shared FB, no scaling", browser->primary, id);
        }
    }
    if (planes) drmModeFreePlaneResources(planes);
}

static gboolean add_ui_state(Browser *browser, drmModeAtomicReq *request, const Frame *frame)
{
    gboolean split = frame->split;
    if (split && !browser->split_plane) return FALSE;
    uint64_t width = split ? browser->split_geometry.left : browser->ui_width;
    uint64_t output_width = split ? width : browser->mode.hdisplay;
    const uint32_t props[] = { browser->plane_fb, browser->plane_crtc,
        browser->plane_src_x, browser->plane_src_y, browser->plane_src_w, browser->plane_src_h,
        browser->plane_x, browser->plane_y, browser->plane_w, browser->plane_h };
    const uint64_t values[] = { frame->fb, browser->crtc, 0, 0, width << 16,
        (uint64_t)browser->ui_height << 16, 0, 0, output_width, browser->mode.vdisplay };
    for (unsigned i = 0; i < G_N_ELEMENTS(props); i++)
        if (drmModeAtomicAddProperty(request, browser->primary, props[i], values[i]) < 0) return FALSE;
    if (!browser->split_plane) return TRUE;
    const uint64_t right[] = { split ? frame->fb : 0, split ? browser->crtc : 0,
        width << 16, 0, (uint64_t)browser->split_geometry.right << 16,
        (uint64_t)browser->ui_height << 16, width, 0, browser->split_geometry.right,
        browser->mode.vdisplay, browser->split_zpos, browser->split_blend,
        browser->split_alpha, (uint64_t)(int64_t)frame->fence_fd };
    unsigned count = split ? G_N_ELEMENTS(right) : 2;
    for (unsigned i = 0; i < count; i++) {
        if (!browser->split_props[i]) continue;
        if (drmModeAtomicAddProperty(request, browser->split_plane, browser->split_props[i], right[i]) < 0) return FALSE;
    }
    return TRUE;
}
#endif
