/* SPDX-License-Identifier: MIT
 * Pure ownership gate for the single DRM committer. Times use monotonic units.
 * No successful timeout, cancellation or credit return implies presentation.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    SIGNAGE_STANDBY_EMPTY, SIGNAGE_STANDBY_PREPARING,
    SIGNAGE_STANDBY_READY, SIGNAGE_STANDBY_COMMITTING,
    SIGNAGE_STANDBY_RETIRING
} SignageStandbyPhase;

typedef struct {
    SignageStandbyPhase phase;
    uint64_t active, standby, high_generation, ui_generation;
    uint32_t first_serial, first_fb, final_serial, final_fb;
    uint64_t final_presented_at, final_interval;
    bool endpoint_presented;
    /* UI-only preparation: standby is a token with no broker slot, producer
     * or decoder. active may be 0 (boot / no outgoing video). */
    bool ui_only;
} SignageStandby;

static inline bool signage_standby_prepare(SignageStandby *s,
    uint64_t active, uint64_t next, unsigned contexts,
    uint64_t cma_available, uint64_t cma_required)
{
    /* active == 0: no outgoing video stream (UI-only or boot outgoing). */
    if (s->phase != SIGNAGE_STANDBY_EMPTY || !next || next <= active ||
        next <= s->high_generation || contexts < 2 || !cma_required ||
        cma_available < cma_required) return false;
    s->active = active;
    s->standby = next;
    s->high_generation = next;
    s->endpoint_presented = false;
    s->ui_only = false;
    s->first_serial = s->first_fb = s->final_serial = s->final_fb = 0;
    s->phase = SIGNAGE_STANDBY_PREPARING;
    return true;
}

/* UI-only incoming page: reserves no decoder/CMA video context. The token is
 * unique and monotonic with video generations but never owns a broker slot. */
static inline bool signage_standby_prepare_ui(SignageStandby *s, uint64_t active, uint64_t token)
{
    if (s->phase != SIGNAGE_STANDBY_EMPTY || !token || token <= active ||
        token <= s->high_generation) return false;
    s->active = active;
    s->standby = token;
    s->high_generation = token;
    s->endpoint_presented = false;
    s->ui_only = true;
    s->first_serial = s->first_fb = s->final_serial = s->final_fb = 0;
    s->phase = SIGNAGE_STANDBY_PREPARING;
    return true;
}

/* test_only must be the actual KMS TEST_ONLY result, not media-clock/rAF. */
static inline bool signage_standby_ready(SignageStandby *s, uint64_t generation,
    uint32_t serial, uint32_t fb, bool test_only)
{
    if (s->phase != SIGNAGE_STANDBY_PREPARING || s->ui_only || generation != s->standby ||
        !serial || !fb || !test_only) return false;
    s->first_serial = serial;
    s->first_fb = fb;
    s->phase = SIGNAGE_STANDBY_READY;
    return true;
}

/* ui_fb is the exported, fence-complete incoming UI framebuffer; test_only is
 * the actual KMS TEST_ONLY of that UI with the video plane disabled. */
static inline bool signage_standby_ready_ui(SignageStandby *s, uint64_t token,
    uint32_t ui_fb, bool test_only)
{
    if (s->phase != SIGNAGE_STANDBY_PREPARING || !s->ui_only || token != s->standby ||
        !ui_fb || !test_only) return false;
    s->first_fb = ui_fb;
    s->phase = SIGNAGE_STANDBY_READY;
    return true;
}

/* Only the exact outgoing pageflip endpoint is admitted. */
static inline bool signage_standby_endpoint(SignageStandby *s,
    uint64_t generation, uint32_t serial, uint32_t fb,
    uint64_t presented_at, uint64_t interval)
{
    if ((s->phase != SIGNAGE_STANDBY_PREPARING && s->phase != SIGNAGE_STANDBY_READY) ||
        !s->active || generation != s->active || !serial || !fb || !interval ||
        presented_at > UINT64_MAX - interval) return false;
    s->final_serial = serial;
    s->final_fb = fb;
    s->final_presented_at = presented_at;
    s->final_interval = interval;
    s->endpoint_presented = true;
    return true;
}

static inline bool signage_standby_activate(SignageStandby *s,
    uint64_t outgoing, uint32_t final_serial, uint32_t final_fb,
    uint64_t incoming, uint64_t target_at, uint64_t ui_generation, bool ui_ready)
{
    if (s->phase != SIGNAGE_STANDBY_READY || s->ui_only || outgoing != s->active ||
        incoming != s->standby || !ui_generation || !ui_ready) return false;
    /* No outgoing video stream: nothing to complete; never fabricate one. */
    if (!s->active) {
        if (final_serial || final_fb) return false;
    } else if (!s->endpoint_presented ||
        final_serial != s->final_serial || final_fb != s->final_fb ||
        target_at < s->final_presented_at + s->final_interval) return false;
    s->ui_generation = ui_generation;
    s->phase = SIGNAGE_STANDBY_COMMITTING;
    return true;
}

/* UI-only activation. need_endpoint: outgoing video ended at EOS, so its exact
 * final frame must have been presented for its full interval first. Otherwise
 * (timed HTML outgoing, boot) outgoing video is hidden in the same commit. */
static inline bool signage_standby_activate_ui(SignageStandby *s, uint64_t outgoing,
    uint64_t token, bool need_endpoint, uint64_t target_at, uint64_t ui_generation, bool ui_ready)
{
    if (s->phase != SIGNAGE_STANDBY_READY || !s->ui_only || outgoing != s->active ||
        token != s->standby || !ui_generation || !ui_ready) return false;
    if (need_endpoint && s->active && (!s->endpoint_presented ||
        target_at < s->final_presented_at + s->final_interval)) return false;
    s->ui_generation = ui_generation;
    s->phase = SIGNAGE_STANDBY_COMMITTING;
    return true;
}

/* ACTIVATED is legal only after the matching atomic pageflip completes. */
static inline bool signage_standby_flipped(SignageStandby *s,
    uint64_t generation, uint32_t serial, uint32_t fb)
{
    if (s->phase != SIGNAGE_STANDBY_COMMITTING || s->ui_only || generation != s->standby ||
        serial != s->first_serial || fb != s->first_fb) return false;
    /* No outgoing video stream (UI-only outgoing): incoming is active now. */
    if (!s->active) { s->active = s->standby; s->standby = 0; s->phase = SIGNAGE_STANDBY_EMPTY; }
    else s->phase = SIGNAGE_STANDBY_RETIRING;
    return true;
}

/* UI-only: the exact prepared UI fb completed its pageflip (with the video
 * plane disabled in that same commit). With no outgoing video there is
 * nothing to retire; otherwise outgoing waits for exact retirement. */
static inline bool signage_standby_flipped_ui(SignageStandby *s, uint64_t token, uint32_t ui_fb)
{
    if (s->phase != SIGNAGE_STANDBY_COMMITTING || !s->ui_only || token != s->standby ||
        !ui_fb || ui_fb != s->first_fb) return false;
    if (!s->active) { s->standby = 0; s->ui_only = false; s->phase = SIGNAGE_STANDBY_EMPTY; }
    else s->phase = SIGNAGE_STANDBY_RETIRING;
    return true;
}

/* A rejected activation commit returns to READY; never to presentation. */
static inline bool signage_standby_activation_failed(SignageStandby *s, uint64_t incoming)
{
    if (s->phase != SIGNAGE_STANDBY_COMMITTING || incoming != s->standby) return false;
    s->phase = SIGNAGE_STANDBY_READY;
    return true;
}

/* An in-flight activation cannot be recalled. Keep both contexts occupied. */
static inline bool signage_standby_cancel(SignageStandby *s, uint64_t generation)
{
    if ((s->phase != SIGNAGE_STANDBY_PREPARING && s->phase != SIGNAGE_STANDBY_READY) ||
        generation != s->standby) return false;
    s->phase = SIGNAGE_STANDBY_EMPTY;
    s->standby = 0;
    s->ui_only = false;
    return true;
}

/* Caller proves no current/pending/FIFO/retained framebuffer from outgoing. */
static inline bool signage_standby_retired(SignageStandby *s, uint64_t generation)
{
    if (s->phase != SIGNAGE_STANDBY_RETIRING || !generation || generation != s->active) return false;
    /* A UI-only incoming owns no video stream: none becomes active. */
    s->active = s->ui_only ? 0 : s->standby;
    s->standby = 0;
    s->ui_only = false;
    s->phase = SIGNAGE_STANDBY_EMPTY;
    return true;
}
