# Native standby candidate — offline implementation, hardware acceptance RED

## Integrated path

Protocol v2 is one matching image build; do not mix old browser/plugin binaries.
The broker owns two bounded connection slots (active and explicitly reserved
standby/retiring), independent RX parsers, serial high-water marks and bounded
partial-write ACK queues. Every message carries its connection generation.
A second ordinary connection cannot replace active. Handshake reads are
nonblocking and timeout-bounded; a stalled connecting peer cannot block KMS.

The native `signage` WebKit message bridge accepts player reservations, creates
one separate native-size WPE renderer and retains its exported physical UI frame.
It does not use a hidden incoming video in the outgoing DOM. The sink binds the
reserved generation through the media URI, prerolls PAUSED to one retained first
DMA-BUF and never displays it. Actual UI+video geometry uses the existing
single committer's `DRM_MODE_ATOMIC_TEST_ONLY`. Fonts/images readiness requests a
subsequent export; the export fence must complete. rAF/media progress is not
physical presentation proof.

EOS sends ENDPOINT only after successful final PRESENTED, no cancellations and
no outstanding credits. The browser checks the exact final (generation, serial,
FB), retains the full source frame interval (rounded UP to microseconds), and
activates only after that interval with no pending/FIFO old video. One atomic
commit switches prepared physical UI and retained first video together.
ACTIVATED is routed only after the exact matching pageflip; player teardown and
incoming play are after that ACK. RELEASED remains independent. Cancelling or
hiding standby cannot hide active; a retiring peer cannot retake the new plane.
Slots and decoder buffers are not released based on a timeout/EOF alone.

Distinct videos use `/player/?native_surface=...` in the native staged renderer.
Mixed menu uses its explicit `native_stage` lifecycle: retains artwork/layout,
prepares scene C (or explicit B/C) without running the curtain/readiness wait,
loads a PAUSED token-bound sink and fonts/images before ui-ready. Playback starts
only on native activation. Ordinary menu loop/preview is unchanged. Arbitrary
HTML without this lifecycle cannot report ready and fails/stalls explicitly;
this does not certify all third-party HTML packages. Static media-free HTML keeps
its existing browser staging path, which is not a physical-vblank guarantee.

## UI-only native lifecycle (startup, HTML->HTML, video->HTML, HTML->video)

Built-in media-free pages (welcome, menuboard scene A) declare an explicit
UI-only lifecycle: `native-stage` carries `ui_only:true`, the gate takes a
token from the broker generation counter that owns no slot (no producer,
decoder or CMA video context can bind to it), and the page sends
`{type:'ui-ready',ui_only:true}` after fonts, decoded images and paused
animations. PREPARED then still requires a later fence-complete export of the
staged 3840x2160/scale-1 view and an actual KMS TEST_ONLY of that UI with the
video plane disabled. No PREPARED/ACTIVATED broker ACK is sent (there is no
producer) and no video frame is fabricated.

Activation commits the incoming UI in one atomic request; if outgoing video is
on screen its HIDE is in that same request. At startup or from a UI-only page
there is no outgoing endpoint and none is required; activation is refused if
any untracked video is on screen. After an outgoing video EOS the player sends
`endpoint:true` and the commit waits for the exact final frame interval.
`activated` is routed only after the pageflip of that exact UI frame; then the
outgoing stream is deactivated (its later SHOWs are CANCELLED, never queued)
and retired only after exact RELEASED/EOF. A UI-only outgoing view is recycled
into the one-view pool once its UI is off screen. HTML->video with no outgoing
stream reserves a slot without an active one; that slot becomes active only at
its own matching flip. Activation is bounded (2 s) and reports `failed`, which
the player turns into cancel + explicit serial fallback, never a parked
transition. Arbitrary third-party HTML gets no UI-only promise.

## Admission and explicit failure

Admission fails closed when `SIGNAGE_STANDBY_CONTEXTS` (at least 2) and
`SIGNAGE_STANDBY_CMA_BYTES` (additional decoder DPB + UI scanout/copy pool +
allocator reserve, bytes) are absent. The entrypoint now supplies the 4K60 bound
measured on the T527 board (86 MiB DPB + 98 MiB UI BOs + 36 MiB reserve) plus a
per-path bound, as defaults an operator can override. Before this, the managed OCI
had neither variable, so every `native-stage` was answered `not-ready`.

The player never parks on a refusal: a not-ready/failed/cancelled stage, a lost
readiness notice (20 s bound) or a failed activation degrades to the explicit
serial path (non-gapless), and refusals back off 1 s doubling to 30 s per entry. Admission
reads live CmaFree, keeps native 3840x2160 / scale 1 / 60Hz and never reduces
resolution/rate or disables QoS. Compressed RAM cache is not CMA admission.
No arbitrary guessed board memory threshold is shipped.

A missing reservation, failed capacity, failed TEST_ONLY, unready UI or absent
outgoing endpoint yields next-not-ready/failed, never fabricated gapless success.
A 15-second preparation timeout reports not-ready and retains outgoing scanout.
An accepted atomic commit cannot be cancelled mid-flight.

## Offline evidence

- Real production broker/receiver socketpair code compiled with ASAN/UBSAN:
  generation-directed ACKs, partial reads/writes, EINTR/EAGAIN, TX floods,
  stale ACK, replayed serial, isolated HIDE/cancel and bounded reservations.
- Production native activation functions execute against instrumented KMS calls:
  failed TEST_ONLY, incomplete outgoing interval, UI fence, pending commit,
  matched/mismatched flip and retirement. This is ordering/ownership evidence,
  **not** an actual hardware KMS TEST_ONLY or presentation measurement.
- Actual Gst sink lifecycle: 100 repeated start/stop/refcount/receiver iterations
  on ARM64 with ASAN/UBSAN, plus receiver and gate tests on ARM64.
- ARM64 browser/plugin build uses -Wall -Wextra -Werror. Browser --version and
  gst-inspect load the installed adapter. OCI entrypoint/arguments/HTTP/SIGTERM
  startup check runs on the updated local ARM64 image.
- Player tests reject stale generation activation and keep outgoing until ACK.
  Existing 600-callback DOM tests remain simulated, not 600 physical frames.

Buffered diagnostic `video-ledger.h` is preserved and integrated. Set
`SIGNAGE_VIDEO_LEDGER_DIR` to volatile `/run/...` to enable; per-frame record is a
bounded memory ring, dumps at EOS/stop, includes generation and explicit total /
capacity. Compare complete ordered demux PTS against stages, not empty credits,
unique summary counts, or rotating logs. No cache/persistent-write policy changed.

## Hardware gates still required

Exact installed WebKit must confirm PAUSED preroll creation, playbin URI token
binding, post-readiness physical UI export/alpha, real two-context CMA occupancy,
matching atomic pageflip scheduling and frame-clock rebasing after activation.
Exercise repeated mixed/video loops, native-reset/edits/seeks, stalled sockets,
active/standby disconnection and rejected activation. Native 4K60 menu delivery
and all ordered 600 frames remain separate hardware acceptance gates.

The preserved film review found no unambiguous half-green fault in 2700 frames,
not absence of intermittent faults. Its coherent branded hold is a real readiness
boundary, not proof of successful motion. Managed auto already selected AFBC;
this patch adds no forced allocator trick. The 598 unique / 599 PRESENTED EOS
summary is a deficit, despite resultok. Menu UI ~39fps and ~37.1fps without PiP
remain unresolved performance evidence; this patch does not claim to fix them.
No board, kernel, deployment, modeset or managed-workload restart was performed.
