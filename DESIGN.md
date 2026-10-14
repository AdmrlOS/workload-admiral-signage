# Persistent single-decoder video engine (design)

Status: implementation on branch `native-video-engine`. Offline tests only; hardware
acceptance (camera, TCON underflow, 600/600 ordered PTS on the board) is still RED until the
parent runs the coordinated hardware gates in `tests/VIDEO_ENGINE_VALIDATION.md`.

## Problem

* v9 (`846410a`) works live: one `<video>` per clip, teardown + new pipeline per clip (non-gapless).
* v10 (`2a0bb8b`) added a second, hidden standby decoder/WebView; without explicit admission
  defaults every stage was refused and playback froze.
* v12 (`5d90b53`) admitted standby by default: two decoders + hidden views run concurrently.
  Concurrent memory bursts at activation/stage time coincide with TCON FIFO underflow
  (display fetch, decoded pixels proven bit-exact, `CHECKPOINT-3.md`).

A second decoder, a second WebView and per-clip pipeline construction are therefore exactly the
bursts to remove. The decoder must be created once and fed continuously.

## Architecture

```
page JS ──(SignageVideo component / data-signage-engine adapter)──▶ player (postMessage proxy for iframes)
   │  HTTP POST /api/video/command, SSE /api/video/events            (app/video_engine.py relay)
   ▼
signage-video-engine (one process, one pipeline for the life of the workload)
   appsrc(HEVC Annex-B AUs, continuous PTS) ! h265parse ! v4l2slh265dec ! westerossink client-role=engine
                                                                         │ existing broker protocol
                                                                         ▼
signage-browser: single atomic DRM committer (UI plane + NV12 video plane), PRESENTED/RELEASED
```

* **One engine owns the V4L2 decoder.** It connects to the existing broker as an ordinary
  browser-committed client with role `engine` (handshake token `UINT64_MAX`). The broker
  already admits only one active client; with an engine lease it logs and refuses HTML
  `<video>` holepunch sinks explicitly (the page receives a media error) instead of a
  silent second decoder. When the engine is not running (feature off), static HTML and the
  legacy `<video>` lifecycle are unchanged (v9 behaviour).
* **No pipeline per clip, no EOS between clips.** Prepared assets are HEVC Main 8-bit
  3840x2160 CFR 60 elementary streams with closed GOPs, IDR at every GOP start, zero
  reorder (`sps_max_num_reorder_pics=0`) and one SPS shared by all compatible assets
  (`compat_key`). The feeder pushes access units into the same appsrc with continuous
  timestamps: the outgoing last frame keeps its full 1/60 s interval and the incoming IDR
  carries the next PTS. The decoder never sees a resolution/format change, so its buffer
  pool (CMA) is negotiated once at startup preflight.
* **Dynamic selection, not a fixed sequence.** Commands (`prepare`, `play`, `pause`,
  `resume`, `seek`, `show`, `hide`, `rect`, `cancel`, `release`) are asynchronous; the engine
  decides at GOP granularity what to feed next:
  * `play(at="boundary")` queues after the current clip (gapless);
  * `play(at="gop")` switches at the next not-yet-fed IDR of the current clip (gapless,
    latency ≤ GOP + feed window);
  * `play(at="now")` is an explicit flush switch (one pipeline, same decoder; may repeat the
    last frame for the flush latency, reported as `cut:"flush"`).
  * Same clip loop = the same asset fed again (no seek/restart, no END).
  * Next asset not ready at the boundary: the current clip continues (loops) and the engine
    emits `waiting`; it switches at the first boundary after `ready`. Never freezes silently.
* **Presentation acknowledgements are real.** In hardware mode the plane sink posts
  `admiral-presented` (pts, serial) only when the browser reports PRESENTED for that SHOW
  after the matching flip. `started` (first frame of an instance), `ended` and position are
  derived from those. CANCELLED is never counted as presented. Test mode (`appsink`) labels
  its acknowledgements `source:"test-appsink"`; they are not scanout proof.
* **Bounded memory.** Encoded assets are read into an LRU RAM cache bounded by
  `SIGNAGE_ENGINE_CACHE_MB` (default 160); current and queued assets are pinned; a prepare
  that cannot fit fails with `over-budget`, never evicts pinned data. appsrc is bounded
  (`SIGNAGE_ENGINE_FEED_MS`, default 500 ms). Decoded frames stay in the decoder's single pool;
  the sink keeps buffers until RELEASED (existing retirement contract).
* **Startup preflight.** Engine start: connect broker, build pipeline, preroll the warm-up
  asset's first GOP invisibly (`visible=false`, no SHOW), so decoder pool, broker generation
  and plane admission exist before any timed playback; then emits `engine-ready` with pool
  allocation count and cache budget. The player starts timed playback only after it.
* **Ownership/lifecycle.** One controlling client (page instance) at a time. `acquire`
  preempts the previous owner (`preempted` event). Owner SSE disconnect (pagehide, crash,
  navigation) → `client-gone` → engine hides and pauses, keeps the decoder. Every command
  carries a client generation; late completions of cancelled prepares are discarded.
* **Rect / layout.** The component sends its physical rectangle (CSS px × DPR, iframe
  offsets added by the player). Engine sets `westerossink rectangle`; the adapter maps UI to
  mode pixels as for WebKit. Full screen, PiP and page handoffs only change the rectangle.
  The component's box is transparent, so page alpha around it is preserved.

## Asset preparation (`scripts/prepare-engine-asset.py`)

Local ffmpeg/ffprobe only. Original files are read-only inputs and are never overwritten
(output path must differ and must not exist unless `--force`, which only replaces outputs).
Outputs `NAME.hevc` + `NAME.engine.json` (`engine_protocol`, codec/profile/level, size, fps,
frame count, per-AU offset/size/pts/key, GOP list, `compat_key` = SHA-256 of the SPS RBSP,
PPS hash, ES SHA-256, source SHA-256, audio policy). Audio is never invented: a source with
audio requires an explicit `--audio drop`; the manifest records `audio:"none"|"dropped"`.
Validation parses the ES itself (NAL types, IDR per GOP, no CRA/RASL, SPS profile/bit
depth/chroma/size/reorder, VUI colour) and runs ffprobe (frame count, CFR). Incompatible
assets are rejected with a reason; the engine re-validates the manifest and refuses
mismatching `compat_key` rather than renegotiating.

## Compatibility

* Old player asset API (`/api/library`, `/api/playlist`, Studio uploads) unchanged. A video
  asset is engine-playable when a prepared variant exists whose `source_sha256` matches the
  *current* file (image builtins in `/opt/signage/media/engine/`, data variants in
  `/data/engine/`). Otherwise the player uses the legacy v9 `<video>` path after an
  explicit engine `release` (non-gapless, logged).
* Image-backed builtins: engine variants and the engine protocol version ship in the image;
  stale data copies whose hash differs are not silently treated as the builtin.
* Native standby (v10/v12 second decoder) is disabled whenever the engine is enabled.

## Out of scope / not claimed

* No kernel change. TCON underflow is a display-fetch bandwidth margin; removing the second
  decoder/WebView bursts may reduce it but is **not claimed fixed** until the controlled
  same-clip camera + UF probe test passes on hardware.
* Audio playback; multiple simultaneous independent videos; transforms/rotation.
