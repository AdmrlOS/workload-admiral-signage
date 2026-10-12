# Native 4K player EOS integration (offline)

The player now owns one rVFC chain per arm generation. Poll/timeupdate never schedule callbacks. Readiness callbacks are cancelled when media-clock progress resolves readiness; callbacks/timers are invalidated before seek, hold, removal and rearm. The previous readiness `onplaying` closure is removed after resolution, so restart cannot create additional readiness chains.

Full-length video entries wait for `ended`/EOS, not duration minus 50 ms or a wall-clock timer at exact duration. Explicit shorter playlist durations remain intentional scheduled cuts. A full clip has a duration+5-second recovery watchdog, logged as `watchdog` (NOT successful EOS). Same-URL playback seeks/replays in place without a second video element. Stale ended events must agree with the live element's ended/non-seeking state.

## Measurement contract

Use `/player/?debug=1`. `end transition` records reason, generation, media time, callback count, last rVFC presentedFrames (null if never delivered), and browser quality created/dropped counts. rVFC counters are browser-reported, not physical scanout proof; currentTime is readiness only. Native corrected observer independently supplies decoded_frames, sink_input_frames, decoder_qos_dropped, basesink_rendered/dropped. BaseSink rendered is delivery to render, not completed KMS scanout. Correlate browser atomic presentation evidence with native counters and numbered-frame correctness; never infer 600 scanouts from media time or EOS alone. Quality logs run once a second; no per-frame DOM metric updates.

## Latency: no unmeasured offset shipped

No timestamp offset, QoS disabling, lateness relaxation or frame-rate/resolution change is included. Existing `SIGNAGE_VIDEO_MAX_LATENESS_MS` remains an experiment knob; `SIGNAGE_VIDEO_QOS=0` is not an acceptance fix. The browser-committed sink inherits BaseSink processing-deadline (20 ms) and render-delay (0). It hands frames asynchronously to the browser, whose next atomic pageflip introduces a distinct submission-to-visible delay. Gst processing-deadline is the processing budget; render-delay describes that physical render-to-visible delay; pipeline clock latency must account for upstream decode/queue timing and sink latency negotiation. Neither is an arbitrary PTS offset.

Needed hardware gate: capture pipeline LATENCY queries/messages and actual configured sink processing-deadline/render-delay/latency, buffer PTS versus running time at sink arrival, broker submission and pageflip completion under simultaneous native UI/video. Verify the non-live file pipeline's actual chosen clock latency rather than assuming processing-deadline increases buffering. If sink arrivals are already late, fix/advertise upstream latency and provide bounded buffering with the negotiated clock budget; if arrivals are on time but flips late, advertise measured render-delay and correct broker scheduling. Set any processing-deadline from measured worst-case work/queue bounds, re-negotiate latency, keep QoS and finite max-lateness, and prove all 600 frames. Offline tests cannot select a justified numerical latency or certify T527 4K60/corruption-free playback.

## Reproduce offline

```sh
cd ~/Dev/admrl/workload-admiral-signage
node --test tests/test_*.js
python3 -m unittest discover -s tests
docker buildx build --platform linux/arm64 --load -t admiral-signage:player-eos .
docker run --rm --platform linux/arm64 --entrypoint sh -v "$PWD:/source:ro" admiral-signage:player-eos -c '
set -eu
flags="$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 json-glib-1.0 libdrm)"
cc -Wall -Wextra -Werror -fsanitize=address,undefined /source/tests/native-release-lifecycle.c -o /tmp/lifecycle $flags
ASAN_OPTIONS=detect_leaks=0 /tmp/lifecycle
cc -Wall -Wextra -Werror -fsanitize=address,undefined /source/tests/native-pipeline-observer.c /source/native/gst-admiral-videoplane.c -o /tmp/observer $flags
ASAN_OPTIONS=detect_leaks=0 /tmp/observer'
```

The Docker build includes the existing uncommitted direct-linear FDO and observer changes; the player fix does not alter those drafts. For integration into a kernelagent-owned rootfs, update only `www/player/player.js` at its existing `/opt/signage/www/player/player.js` location and use the separately built corrected native plugin/direct-linear FDO artifacts already agreed with the board owner. No board deployment/restart is performed here. Keep framebuffer and CSS viewport 3840x2160, UI scale 1, 60 Hz and original 600-frame 4K60 media. This document is not hardware acceptance.
