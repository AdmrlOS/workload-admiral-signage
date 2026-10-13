# Bounded preloading / handoff candidate (offline, NOT gapless acceptance)

Native follow-up: `STANDBY_PROTOCOL.md` now documents the integrated v2 broker,
PAUSED sink, native WPE export/JS bridge and player/menu lifecycle. The remaining
blocker descriptions below record the earlier cache-only baseline; they are not
the current v2 code state. Hardware native4K60/all-frame acceptance remains RED.

## Shipped production path

`app/ram_cache.py` stores compressed files in anonymous mmap RAM. Defaults:
`SIGNAGE_RAM_CACHE_MB=256`, `SIGNAGE_RAM_RESERVE_MB=1024`. Zero budget disables
admission. Linux MemAvailable and cgroup-v2 remaining bytes both constrain
admission; missing memory telemetry fails closed. The reserve is ordinary-memory
headroom, **not** proof of sufficient contiguous CMA/decoder contexts. The cache
accounts in-flight allocation, reads in 64 KiB chunks, checks pressure/cancellation
between chunks, and discards incomplete/changed files. No playback cache write,
fsync, disk-backed tempfile, unlimited full-library cache, or full decoded video
copy. Anonymous pages can still be swapped by the OS: this is not mlock.

`POST /api/preload` accepts a latest active/next URL window (maximum six URLs
including posters), rejects cross-origin/opaque-package callers, resolves only
local media/content within allowed roots, and warms package dependencies including
CSS, nested CSS dependencies, fonts and images without executing the package.
One background loader plus one replaceable pending window prevents thread/job
accumulation. Playlist pins and HTTP reader leases prohibit LRU eviction; assets
that cannot fit fall back to disk. Warm media HTTP ranges are served directly
from leased RAM, preserving existing 206/416/HEAD behavior. `X-Signage-Cache`
reports ram/disk (HTML bridge injection makes a temporary HTML copy). Browser
HTTP cache, decoded posters (existing eight-image cap), WebKit page heap and
native decoder/CMA memory are additional consumers, not included in the compressed
budget. Budget/reserve are safety policies, not a hard process/cgroup limit.

The player publishes the current/next window and removes advisory unbounded
prefetch links. Static HTML opts in with the exact declaration:

```html
<meta name="signage-preload" content="static">
```

The server annotates the playlist; one hidden prepared iframe is allowed. Its
response has `media-src 'none'; frame-src 'none'; object-src 'none'` to prevent
a dynamically created second hardware sink/nested media document. Bridge readiness
waits for load, fonts.ready, image.decode and browser paints. A package containing
media/embedded document elements rejects staging. Edits/teardown cancel hidden
pages, their handshake listeners and generation. An active plus one prepared
page is a count bound, not a byte bound on arbitrary JavaScript heap; only trusted
static packages should opt in. Broken dependencies time out safely.

A matching staged page is awaited **before** releasing outgoing video. Visibility
switch and old element removal share an rAF task. No new navigation at activation.
The full clip continues to EOS; no duration-minus-50ms cut. Same-URL video still
restarts in place with one decoder. Native 3840x2160, CSS scale 1, original 60fps,
QoS, AFBC, browser/plugin ownership protocol and native diagnostic drafts are
unchanged. Ordinary mixed HTML is warmed as bytes only, retains its existing
alpha/holepunch path, and is not prerendered unsafely.

## Critical remaining gap: distinct-video and mixed-HTML transitions

**This does not implement or claim uninterrupted gapless video motion.** Current
`native/drm-broker.c:on_incoming` explicitly replaces/hides the old client when a
new sink connects (lines 364–371). A hidden second `<video>` would steal the
connection/generation and violate the outgoing zero-copy retirement contract.
Browser media-clock progress is not PRESENTED proof; rVFC can exist but never
fire on holepunch. Cached compressed data alone cannot eliminate demux, decoder
initialization/preroll, WebKit navigation or atomic flip latency. Current different
video fallback tears down then starts and uses a frozen poster: explicitly
**non-gapless**. Same-codec equality alone does not authorize decoder reuse.

Required native integration, not safely guessed/shipped in this patch:

1. Negotiate versioned STAGE capability on the single browser-owned DRM broker;
   do not replace the active client on standby registration. Reserve exactly one
   standby decoder context only after measured CMA/context admission. Decode
   standby to PAUSED, bounded DPB plus one presentable first frame, no SHOW or
   second DRM committer. Refuse when capacity is unavailable; never claim gapless.
2. Distinguish stream generation + SHOW serial + FB identity for active/standby;
   keep independent PRESENTED/CANCELLED credit and RELEASED ownership. Preserve
   old/current/pending/FIFO refs until real scanout retirement, never EOF/timeout.
3. At EOS wait for the exact last outgoing presentation endpoint ACK **and its
   display interval**, not network completion/EOS receipt alone. Current sink
   waits for outstanding presentation messages, but CANCELLED also clears
   credit; an empty credit table is not successful final-frame proof. Exact ordered
   PTS validation must still reject missing/dropped endpoints.
4. Schedule standby's first SHOW and associated native UI/alpha change together
   through the existing atomic committer at the next target vblank. Only after
   flip-complete acknowledge activation to WebKit/page JS, release old resources,
   advance playlist clock and permit lookahead of the following entry. Timeouts
   retain old surface but are failures/stalls, not successful motion handoffs.
5. For compatible concatenated streams, a native continuous timeline/decoder
   reuse path may avoid two contexts, but must handle codec configuration, DPB,
   timestamp rebasing, seeking and changed HTML layout explicitly. Existing JS
   same-URL seek is an optimization, not certified gapless.

The rAF static-page switch here is browser-prepared, not native-vblank-ACK
synchronization. CSS visibility of hidden iframe rendering and actual fonts/image
readiness still need exact WebKit hardware verification. Do not clear gapless
acceptance from offline VM tests.

## Verification and integration

Run from canonical repo:

```sh
node --test tests/test_*.js
TMPDIR=/Users/danielward/.hermes/cache/scratch python3 -m unittest discover -s tests
```

Tests cover cold/warm compressed cache, RAM ranges, oversized files, pinned LRU,
reader leases, memory pressure during load, missing/changed sources, cancellation,
HTML package font dependencies, staging CSP, origin controls, prepared-page reuse,
not-ready next page retaining old video, playlist edits/stale readiness, mixed-HTML
no hidden sink, same-URL restart, and existing complete 600-frame EOS loops. These
are real HTTP/cache and simulated DOM tests, not physical 600-scanout proof.

Integrate `app/ram_cache.py`, `app/server.py`, `www/player/player.js` and
`www/player/bridge.js` via the normal image (Dockerfile already copies app/www).
No kernel/plugin/browser native changes required for this cache candidate. Before
release measure ordinary RAM plus CMA over long playlist/edit/cancellation loops,
static/mixed alpha scene readiness, concurrent native 4K60 UI and video, exact
ordered full PTS per EOS, and camera-scored handoff motion/black/duplicate frames.
Correlate cache admission/read activity with the rare pre-SHOW decoder/QoS drops.
Persistent Btrfs compressed writes were measured harmful in read-delivery/RESULT;
this cache adds none, but concurrent disk reads/CPU can still disturb scheduling.
No board access/restart/deployment or interference with diagnostic matrix occurred.
