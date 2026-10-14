# Persistent video engine: offline evidence and coordinated hardware gates

Offline results prove pipeline/timeline/pixel correctness with a software decoder and a
TEST ONLY hashing sink. They do **not** prove scanout, TCON underflow behaviour or
camera-visible smoothness on the board. Hardware acceptance is RED until the gates below
pass with the exact published artifact.

## Offline (reproducible)

```sh
node --test tests/test_*.js                                   # component, player engine path, legacy player
TMPDIR=$HOME/.hermes/cache/scratch python3 -m unittest discover -s tests
SIGNAGE_SLOW=1 tests/run-native-tests.sh                      # ARM64 Debian toolchain, ASAN+UBSAN:
#   native-engine-sink (actual plane sink, engine mode), native-engine-broker (actual broker
#   admission), tests.test_video_engine (real engine + h265parse + avdec_h265),
#   tests.test_engine_media (4K promo-a -> promo-b -> promo-a, 1800/1800 frames pixel-exact),
#   tests.test_engine_relay. Ends with "ALL NATIVE TESTS PASSED" and exit 0.
```

ASAN runs with `detect_leaks=1` (LeakSanitizer verified functional in the container with a
deliberate leak). The engine exits cleanly on SIGTERM and every engine test asserts exit 0
and no ASAN/UBSAN report in the engine log.

## Hardware gates (parent agent, exclusive board handoff)

Stop the managed workload through Admiral MCP only; run the image rootfs as in
`admiral-workload-images` step 5. Record image digest, kernel release, plane IDs from the
browser log (never hard-code them) and the TEST_ONLY result lines.

1. **Startup preflight**: log shows `Engine preflight complete: decoder pool allocated (1
   allocation queries)` before the player's first `engine:play`; no visible warm frame
   (camera), `video-status.json` role engine.
2. **Exclusivity**: during the loop `Refused HTML video sink` appears only if a page used a
   plain `<video>`; `/proc/<webkit pid>/fd` never opens the decoder; exactly one
   `v4l2slh265dec` instance for the whole run (GST_DEBUG `v4l2codecs:4`).
3. **Exact presentation**: `SIGNAGE_VIDEO_TRACE_PRESENTED=1` ordered PTS for promo-a ->
   promo-b -> promo-a: 600/600 per clip, steps 16666666/16666667 ns only, no repeat at the
   handoff (outgoing last frame held exactly one interval, incoming IDR on the next vblank).
   Engine events `ended.presented == frames` for every instance.
4. **CMA/pool stability**: `cma-census` before/after 30 min of dynamic switching (boundary,
   gop, now, seek, pause, hide/show, rect changes, page handoffs): constant decoder pool,
   engine `status.alloc_queries == 1`, `caps_events == 1`, no growth of held framebuffers.
5. **TCON underflow, same clips as v12**: `tcon-uf-probe` + 30 fps YUYV camera through the
   relay (`handoff/CHECKPOINT-3.md`), same promo-a/promo-b/menuboard-pip playlist as the v12
   runs, 150 s windows, at least 8 handoffs. Compare UF counts and pink-band frames with v12
   (9–17 UF / 150 s) and v9. Do not claim the horizontal artifacts fixed unless this
   controlled same-clip camera+UF test shows zero UF-correlated glitches.
6. **Lifecycle on device**: kill the engine (expect bounded restart, page receives
   `engine-disconnected`/`engine-ready`); navigate away from a page with an engine component
   (expect hide+pause within ~250 ms, decoder retained); legacy H.264 upload without a variant
   (expect explicit `release`, legacy `<video>` plays, engine re-acquires afterwards).

Commands for the parent are listed in the branch report; no board access, restart, deploy,
configuration or library edit was done while producing this branch.

## Hardware result (OrangePi 4A, kernel build-id 7ecdd32e, slot B; dev root of the exact image, private netns)

Run by `tests/`-external harness (engine socket + PRESENTED trace + TCON underflow probe + 30 fps camera; scripts are
in the evidence directory). v9 live config was stopped through Admiral for the run and restored afterwards.

| gate | result |
|---|---|
| preflight | `Engine preflight complete ... 12 warm frame(s)` before any playback; needed two fixes (below) |
| exclusivity / contexts | 1 `/dev/video2` fd, constant inode, at every 0.5 s sample over every run; `alloc_queries` 2 / `caps_events` 1 constant |
| exact presentation | 9000/9000 (150 s user playlist), 7200, 4614 (player-driven), 3000, 2400 ... frames: PRESENTED PTS == manifest, steps 16666666/16666667 ns only, handoff gap exactly one interval, no QoS drops |
| restart soak | 30 supported engine restarts, each + one full clip: every one identical (33 allocations = 24 preflight + 9 first-clip lazy MV buffers, CmaFree 49176 kB constant, 0 `cma_alloc_fail`, ctx 1, 600/600) |
| CMA | after the first ~15 s (9 lazy 80-page cedrus MV buffers, once per engine process) `cma_alloc_success` delta is 0 per loop |
| TCON underflow, full-screen 1:1 video | 0 UF windows (28138 samples/150 s, 12406/60 s) + 0 camera candidates (4500 frames) |
| **downscaled video (any rect < full screen: PiP, zoom)** | **RED**: ~60 UF windows/s on a blank page at 2560x1440, 1920x1080, 1280x720; 0 at 3840x2160. Independent of HTML/iframe/UI. 4K60 NV12 scaled down by the display engine underflows the TCON every frame |
| HTML with video (menuboard-pip) | iframe path: UI + video 37 commits/s (not 60) and the UF above; top-level native-builtin: 60/60 but the same UF (scaling) |
| UI-only pages | menuboard scene A 39 commits/s, welcome 19 commits/s (same as v9), 0 UF |

Fixes found only on hardware (all in this branch): DRAIN query is a no-op in GstVideoDecoder 1.26 (replaced by a
still-frame event sent only when the appsrc queue is empty); hide of an already-disabled video plane was rejected
(EINVAL), so a restarted engine was refused forever on a static page; the player acquired once and fell back to a
second legacy decoder when the page loaded before the engine; the rectangle was remembered before it was
acknowledged; the opaque html/body/#standby/iframe covered the video plane (camera saw "Ready when you are" while the
ledger counted 60 fps).

Not claimed: v12 horizontal-band absence (v12 does not reproduce on this kernel either, so a clean run is not proof),
scaled-video playback, 1-hour soak with the exact published artifact.

## Status: accepted vs open

Hardware-observed on d015342 plus these fixes built on the board (not the published image): single-engine
dynamic 4K50/60 full-screen playback with exact PTS, one decoder context, no CMA growth after the first clip.
**Not yet hardware-accepted:** the exact image built from the commit carrying these fixes (it must be loaded and the
single-engine gates re-run before any acceptance claim). **Open:** scaled/PiP video (display-engine underflow, kernel
side), iframe+video commit rate (~37/s), welcome-page UI (19 commits/s), later native embed integration of UI iframes.
Assets must be native 4K at the playlist rate with a common header contract (no 4K50 asset presented as 60).
No full-screen restriction is part of this branch.
