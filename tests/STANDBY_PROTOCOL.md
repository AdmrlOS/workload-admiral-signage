# Standby protocol implementation scope (offline)

The browser remains the only DRM committer. A second connection must be explicitly reserved; it must never replace/hide the outgoing connection. Each socket owns its receive parser, bounded TX queue and monotonically allocated generation. Frame identities are (generation, serial, framebuffer), and retirement is independent of presentation credit.

Prepare is admission-gated on explicit decoder-context/CMA capacity, not the compressed RAM cache. One active and one standby is the maximum; a retiring outgoing socket continues occupying its slot until independent framebuffer retirement. Unknown CMA/context capacity must reject preparation rather than reduce 3840x2160, CSS scale 1, original 60fps or QoS.

A presentable first frame is retained, not displayed, and readiness requires an atomic TEST_ONLY against the real UI/video layout. Activation requires the exact outgoing final PRESENTED identity and its complete display interval, a ready UI fence, and one atomic UI/video commit. ACTIVATED is emitted only on pageflip completion; cancellation/EOF/timeouts never fake retirement or successful motion.

Implementation and test evidence will be listed here as completed. Until the entire browser/sink/player path and exact-image hardware gate pass, hidden second HTMLVideoElements remain disabled and this work must not be called gapless acceptance.

## Completed subset — NOT a working standby path

- `native/standby-protocol.h` is a dependency-free ownership gate, currently **not wired into the broker/browser/player**. It bounds a reservation to one, requires explicit context/CMA admission, rejects stale generations, validates the outgoing endpoint interval and matching first-frame flip, and holds both context slots until outgoing retirement. Its TEST_ONLY/readiness inputs are caller obligations; the helper itself neither imports nor validates a DRM framebuffer.
- Native sink EOS now rejects cancelled presentation credit, missing final PRESENTED serial and zero-frame EOS. Failure resets on start/flush-stop, without resetting serials or releasing held decoder buffers. This fixes a real false-success path but does not prove no upstream decoder/QoS drops.
- New C gate test executes under ASAN/UBSAN on macOS and ARM64 Docker. A regression test executes the extracted production EOS function and was RED before the fix. Existing receiver socketpair tests exercise the new outcome fields.

## Remaining implementation blockers / handoff

The requested real end-to-end preroll is **unfinished**. `drm-broker.c` still has one connection/global ACK routing and replaces outgoing; `gst-admiral-holepunch.c` still takes exclusive `video.lock` before connecting. No native JS message bridge correlates a video element, reservation, prepared UI export and pageflip ACK. The player must NOT create hidden videos until these are implemented together.

Next integration must replace broker globals with bounded connection objects (two maximum including retiring connections), route ACKs by generation, track FB ownership independent of current active socket, add versioned reservation metadata and actual KMS TEST_ONLY readiness, retain a paused first-frame DMA-BUF, and bind activation to a prepared physical UI export. Do not merely remove video.lock or allow the new client to inherit the old generation. No measured CMA/context admission exists in this offline environment; preparation must fail closed until explicitly configured/verified. No board operations occurred.
