# Offline protocol safety candidate — NOT hardware acceptance

## Contract
- SHOW serials are nonzero and strictly increasing per connection. UINT32_MAX is legal, but the sink errors before wrap; recreate the pipeline/connection.
- PRESENTED is an actual flip of the exact SHOW. CANCELLED is a terminal non-presentation outcome (malformed, rejected, HIDE-cancelled); it returns presentation credit but NEVER releases decoder storage.
- RELEASED remains independent and requires no current/pending/next/FIFO reference to that framebuffer. A malformed re-SHOW updates its retirement identity without retiring an in-flight framebuffer.
- Browser enforces two outstanding SHOW credits, excluding already-cancelled pending commits. Old connection generations cannot produce PRESENTED/CANCELLED for a replacement connection.
- Broker TX holds up to 64 records, retains byte offsets across short writes/EINTR/EAGAIN and drains on writable events. Overflow/error disconnects and follows ordinary hide; no partially discarded record is reused.
- Flush hides old work, clears presentation identities/credits, retains retirement references and never resets serials.
- Stop with unretired buffers terminates the WebKit process through g_error, instead of orphaning held/framebuffer tables or falsely unrefing on EOF. Browser WebKit termination recovery recreates the view; this error path must be verified on hardware before release.
- Ordered trace is opt-in SIGNAGE_VIDEO_TRACE_PRESENTED. Unique PTS diagnostics are capped at 4096 and absent without trace. Unique counts are NOT acceptance.

## Offline checks
Python discovery includes C socketpair tests compiled with ASAN/UBSAN, partial send (7-byte maximum), injected EINTR, real EAGAIN, TX flooding, same-FB malformed re-SHOW, HIDE with pending/FIFO states, serial max/zero, connection generation cancellation, receiver teardown loops and process-fatal recovery.
`tests/native-release-lifecycle.c` executes the actual Gst sink 100 times, verifying buffer refcount and independent retirement, compiled with -Wall -Wextra -Werror and ASAN/UBSAN in ARM64 Docker.
Latency query tests execute extracted production C: nonlive 83.333ms, finite/infinite maxima, live processing counted once, sync off and pre-preroll. No QoS disable, lower resolution or framerate change.

## Integration for board owner
1. Use matching browser/plugin binaries from the ARM64 candidate image; do not mix protocol versions. No kernel changes included.
2. Retain native 3840x2160 UI, CSS scale1, native 4K60 video and QoS enabled, plus the independently reviewed kernel candidate.
3. Enable SIGNAGE_VIDEO_TRACE_PRESENTED=1 only for acceptance; independently demux the exact input into JSON integer nanosecond PTS in original order, including actual nonzero initial offset.
4. Run `python3 tests/check_presented_order.py expected-pts.json workload.log`. It accepts only initial preroll reshow, not interior duplicate/reorder/offset/missing PTS. Compare complete EOS loops; do not substitute unique counts.
5. Exercise seek/flush with two credits, malformed/rejected states, disconnect/replacement during flip, TX backpressure and stop with FIFO; confirm process recovery hides/retires old scanout before new decode storage.
6. Repeat exact-image ordered EOS/restart/thermal/camera acceptance. Prior hardware trial remains RED (89/90 EOS; cycle25 missing12); this offline artifact does not clear it.

No board access, restart, deployment, kernel or screenshot changes performed by this worker.
