/* SPDX-License-Identifier: MIT
 * Caller-serialized diagnostic ring: no allocation or I/O per frame.
 * Dump only after streaming stops; overwrites are explicit, never acceptance.
 */
#ifndef SIGNAGE_VIDEO_LEDGER_H
#define SIGNAGE_VIDEO_LEDGER_H
#include <stdint.h>
#include <stddef.h>
#define VIDEO_LEDGER_CAPACITY 8192u
typedef struct {
    uint64_t mono_us, pts, value;
    uint32_t stage, serial, fb;
} VideoLedgerRecord;
typedef struct {
    uint64_t total;
    VideoLedgerRecord records[VIDEO_LEDGER_CAPACITY];
} VideoLedger;
static inline void video_ledger_record(VideoLedger *l, uint64_t mono_us,
    uint64_t pts, uint64_t value, uint32_t stage, uint32_t serial, uint32_t fb)
{
    l->records[l->total++ % VIDEO_LEDGER_CAPACITY] =
        (VideoLedgerRecord){mono_us, pts, value, stage, serial, fb};
}
static inline const VideoLedgerRecord *video_ledger_at(const VideoLedger *l, uint64_t i)
{
    uint64_t count = l->total < VIDEO_LEDGER_CAPACITY ? l->total : VIDEO_LEDGER_CAPACITY;
    if (i >= count) return NULL;
    return &l->records[(l->total - count + i) % VIDEO_LEDGER_CAPACITY];
}
#endif
