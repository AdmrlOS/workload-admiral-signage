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
} SignageStandby;

static inline bool signage_standby_prepare(SignageStandby *s,
    uint64_t active, uint64_t next, unsigned contexts,
    uint64_t cma_available, uint64_t cma_required)
{
    if (s->phase != SIGNAGE_STANDBY_EMPTY || !active || !next || next <= active ||
        next <= s->high_generation || contexts < 2 || !cma_required ||
        cma_available < cma_required) return false;
    s->active = active;
    s->standby = next;
    s->high_generation = next;
    s->endpoint_presented = false;
    s->first_serial = s->first_fb = s->final_serial = s->final_fb = 0;
    s->phase = SIGNAGE_STANDBY_PREPARING;
    return true;
}

/* test_only must be the actual KMS TEST_ONLY result, not media-clock/rAF. */
static inline bool signage_standby_ready(SignageStandby *s, uint64_t generation,
    uint32_t serial, uint32_t fb, bool test_only)
{
    if (s->phase != SIGNAGE_STANDBY_PREPARING || generation != s->standby ||
        !serial || !fb || !test_only) return false;
    s->first_serial = serial;
    s->first_fb = fb;
    s->phase = SIGNAGE_STANDBY_READY;
    return true;
}

/* Only the exact outgoing pageflip endpoint is admitted. */
static inline bool signage_standby_endpoint(SignageStandby *s,
    uint64_t generation, uint32_t serial, uint32_t fb,
    uint64_t presented_at, uint64_t interval)
{
    if ((s->phase != SIGNAGE_STANDBY_PREPARING && s->phase != SIGNAGE_STANDBY_READY) ||
        generation != s->active || !serial || !fb || !interval ||
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
    if (s->phase != SIGNAGE_STANDBY_READY || outgoing != s->active ||
        incoming != s->standby || !s->endpoint_presented ||
        final_serial != s->final_serial || final_fb != s->final_fb ||
        !ui_generation || !ui_ready ||
        target_at < s->final_presented_at + s->final_interval) return false;
    s->ui_generation = ui_generation;
    s->phase = SIGNAGE_STANDBY_COMMITTING;
    return true;
}

/* ACTIVATED is legal only after the matching atomic pageflip completes. */
static inline bool signage_standby_flipped(SignageStandby *s,
    uint64_t generation, uint32_t serial, uint32_t fb)
{
    if (s->phase != SIGNAGE_STANDBY_COMMITTING || generation != s->standby ||
        serial != s->first_serial || fb != s->first_fb) return false;
    s->phase = SIGNAGE_STANDBY_RETIRING;
    return true;
}

/* An in-flight activation cannot be recalled. Keep both contexts occupied. */
static inline bool signage_standby_cancel(SignageStandby *s, uint64_t generation)
{
    if ((s->phase != SIGNAGE_STANDBY_PREPARING && s->phase != SIGNAGE_STANDBY_READY) ||
        generation != s->standby) return false;
    s->phase = SIGNAGE_STANDBY_EMPTY;
    s->standby = 0;
    return true;
}

/* Caller proves no current/pending/FIFO/retained framebuffer from outgoing. */
static inline bool signage_standby_retired(SignageStandby *s, uint64_t generation)
{
    if (s->phase != SIGNAGE_STANDBY_RETIRING || generation != s->active) return false;
    s->active = s->standby;
    s->standby = 0;
    s->phase = SIGNAGE_STANDBY_EMPTY;
    return true;
}
