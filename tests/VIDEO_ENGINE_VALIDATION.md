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
