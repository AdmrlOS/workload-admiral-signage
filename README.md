# Admiral signage development workload

An ARM64 OCI development image for testing GPU-rendered web content and stateless
V4L2 video on an Orange Pi 4A with the Allwinner T527. It boots an owned WPE
WebKit browser into a local information screen with animated WebGL content and
an embedded HTML video. The image retains its browser and video adapter sources,
compiler, debugger and hardware inspection tools for DE35 development on Admiral.

The target is Admiral's Linux 7.2 kernel and board support package. The image
supplies userspace; it does not supply the kernel, device tree, GPU firmware or
video/display kernel drivers. A successful image build cannot establish that
the connected board accelerates rendering, decodes video or blends its planes
correctly. Those are separate acceptance checks below.

## Signage Studio demo

Open **http://DEVICE-IP:8080/studio/** to upload content and manage the loop.
The image now boots **/player/** on the display. For an existing deployment, set `SIGNAGE_PAGE=player` and clear any `SIGNAGE_URL`
override, then apply/restart the workload. The studio reports the native display
page separately from its browser preview; standalone demos do not follow the schedule.

The original Display Lab at `/`
and SUBSTATION page at `/menuboard/` remain available.

- The initial published loop includes an Admiral welcome screen, the complete
  SUBSTATION main menu, a dedicated **menu + picture-in-picture video** slot, and
  both existing promo videos. The complete original animated demo remains in the
  library too (80 seconds when added, so its later PiP scene is reached).
- Add videos (MP4, WebM, MOV), images (PNG, JPEG, WebP, GIF, AVIF), a standalone
  HTML file, or a ZIP containing `index.html` and its relative assets. A single
  enclosing ZIP directory is supported. The limit is 512 MB per upload and
  512 MB / 1,500 files after extraction. MP4/H.264 is the recommended board format;
  accepted containers do not guarantee the hardware supports every codec.
- Add library content to the loop, move it up/down, set its duration, and optionally
  select start/end times. Times are entered in the operator browser's local timezone
  and saved with a UTC offset. Windows include the start and exclude the end.
  Outside its window, an item is skipped; an empty active loop shows an Admiral slate.
- **Publish loop** applies changes to players automatically (normally within two
  seconds). Each player loops independently. Reload discards unpublished edits.
  Videos use `0` duration to play to completion; images and HTML need a positive duration.
- This is a trusted-network demo with no login or multi-user permissions. HTML is
  sandboxed without same-origin access; packages must be self-contained (no API fetches,
  forms, local storage, or authenticated services). Same-origin checks protect write
  endpoints from uploaded packages and cross-site browser requests.

### Persistent storage on Admiral

Mount the Admiral persistent volume at **`/data`**, as in
`config/admiral.example.json`. The first run copies the bundled media and HTML
packages there and creates an initial schedule. Later restarts and image updates
keep the existing library and schedule. A one-time catalog upgrade adds the
explicit main-menu/PiP library items and corrects an untouched initial demo loop;
customized schedules and uploaded files are preserved.

```text
/data/library.json          Library, published loop, schedule and revision
/data/media/                Persistent copies of the bundled videos and stills
/data/content/              Bundled and uploaded HTML, images, videos and ZIP assets
/data/cache/                Runtime caches
/data/logs/                 Diagnostics
/data/www/                  Optional page overrides (existing development feature)
```

An existing volume mounted at the previous `/var/lib/admiral-signage` destination
should be remounted at `/data`; the image does not move or delete that volume.
`SIGNAGE_DATA_DIR` can override this location when needed.

### Continuous transitions

The player retains the outgoing surface until the incoming content reports ready.
Video uploads generate first/last-frame stills ahead of playback, and the player
warms the upcoming media cache without opening a second hardware decoder. It paints
an outgoing still before releasing the video plane, then waits for a presented
incoming video frame before revealing it. HTML packages receive a small lifecycle
bridge that waits for load/fonts/paint and freezes their timers, animations and media
before the next item takes over. Failed content is skipped; the last visual is kept
while trying the remaining loop, and failed loops retry.

This is a **no-blank-frame transition design**, not a claim of frame-accurate seamless
video concatenation. A still can briefly hold during a decoder handoff. Arbitrary HTML
with nested media frames, WebGL video textures, or custom renderers may need its own
lifecycle handling. Only one active video sink is supported by the native adapter.
Verify a full loop on the target T527 with both video-to-video and HTML transitions
before promising gapless playback to a customer; desktop tests cannot certify KMS
plane timing or decoder teardown on that board.

### Local presentation / server-only preview

```sh
docker run --rm --name admiral-signage-studio -p 8097:8080 \
  -v admiral-signage-demo:/data admiral-signage:studio server
```

Then open `http://localhost:8097/studio/`. This uses the same backend, uploaded
content and schedule as the board; browser video in this preview is desktop playback.

## Graphics and video paths

The stack uses Debian trixie packages: WPE WebKit 2.48 with the legacy libwpe
backend API, WPEBackend-fdo, Mesa 25 and GStreamer 1.26. Exact installed versions
are recorded in `/opt/signage/package-versions.txt`; Debian security updates can
change package versions between builds.

The browser shell belongs to this repository and has no Cog dependency. The
legacy libwpe API is intentional for this experiment: WPE's newer WPEPlatform
API [does not yet provide a video-plane equivalent](https://wpewebkit.org/blog/2026-10-06-wpe-platform.html).

`browser/signage-browser.c` owns the browser shell. WPEBackend-fdo exports browser
DMA-BUF frames and the shell presents them on an ARGB browser primary plane through
atomic DRM/KMS. It first tries direct (zero-copy) scanout: import the frame into
the display device and add it as a framebuffer. On boards whose display engine
has no IOMMU, such as the T527 today, the display can only scan out physically
contiguous buffers, while Panfrost renders into non-contiguous (and normally
AFBC-compressed) GPU memory, so that import fails. The shell then logs the reason
once and switches to a GPU copy: each frame is drawn by GLES, as an exact RGBA
copy without blending, into one of three contiguous linear ARGB8888 buffers
allocated from the display device, and that buffer is scanned out. Premultiplied
alpha is preserved, so holepunch video still shows through, and Panfrost may keep
its faster AFBC buffers. The main thread does not wait for the copy: with
`EGL_ANDROID_native_fence_sync` and the primary plane's `IN_FENCE_FD` the shell
flushes, passes the copy's fence fd with the atomic commit and the kernel waits
for it before the flip (about 0.7 ms of main-thread time per frame instead of
7.6 ms on the T527). Without either it falls back to an `EGL_KHR_fence_sync`
client wait or `glFinish`. WebKit's frame callback fires when a commit is
accepted, so the next frame renders while the previous one is copied and
flipped (`SIGNAGE_FRAME_CALLBACK=flip` restores firing at the flip). The
browser status file reports `scanout_path` (`direct` or `gpu-copy`) and
`copy_sync` (`in-fence`, `client-wait` or `finish`). Mesa's Panfrost path is intended to provide
OpenGL ES on the T527's Mali-G57. OpenGL ES 3.1 is the target; this workload does
not depend on Vulkan. The browser's WebGL renderer and the native EGL/DRM logs
must agree before calling the rendering path accelerated.

In the default `holepunch` mode, WebKit manages the real HTML `<video>` element,
playback and its window rectangle. The entrypoint enables WebKit's existing
Westeros holepunch quirk. Our `westerossink` compatibility adapter implements the
properties that this WebKit path expects and hands decoded frames to the
browser, which scans them out on the video plane. This is an owned compatibility
adapter, not the Westeros compositor. No external compositor is involved.

`SIGNAGE_UI_SIZE` selects the **physical framebuffer** size and defaults to the
selected display mode (`native`). With the default `SIGNAGE_UI_SCALE=1`, a
3840×2160 mode means both a **3840×2160 framebuffer and CSS viewport**
(`devicePixelRatio=1`). There is no default 1080p cap. To select 4K explicitly:

```sh
SIGNAGE_DRM_MODE=3840x2160
```

Leave `SIGNAGE_UI_SIZE` and `SIGNAGE_UI_SCALE` unset for native rendering.
An explicit lower size such as `SIGNAGE_UI_SIZE=1920x1080` is an **opt-in
performance choice**: it renders a smaller framebuffer that the primary plane
stretches over the display. `SIGNAGE_UI_SIZE=3840x2160` also works when the mode
is at least that large. Native 4K increases GPU/buffer cost; measure frame rate
and memory use on the target hardware.

`SIGNAGE_UI_SCALE` defaults to `1` and is a WPE device scale factor, **not page
zoom or a KMS upscale**. An explicit `SIGNAGE_UI_SCALE=2` with native 4K retains
the 3840×2160 framebuffer but uses a 1920×1080 CSS viewport and
`devicePixelRatio=2`; text and SVG still render at native resolution. Canvas pages
must size their own backing stores using DPR; low-resolution raster assets are
not made sharper.
Values must be finite and in libwpe's supported range 0.05–5, and must divide the
framebuffer dimensions into integer logical dimensions. Invalid settings fail startup.

Fixed-size HTML pages such as the menuboard declare their design size via
`signage-canvas`. `/player/bridge.js` fits them to the CSS viewport using body
`zoom` (2× for a 1920×1080 design in the default 3840×2160 viewport). This is
page layout/paint scaling inside the native framebuffer: text and SVG are
rasterized at the enlarged native resolution, not rendered into a 1080p
framebuffer and then upscaled by KMS. Raster images and fixed-resolution canvas
backing stores still retain their source-resolution limits.

Before committing to primary-plane scaling the browser runs an atomic
`TEST_ONLY` commit; if the kernel rejects it, it logs the failure and falls back
to framebuffer size = mode size, recalculating the logical viewport. The status
JSON reports `width`/`height` (mode), `ui_width`/`ui_height` (physical framebuffer),
`view_width`/`view_height` (logical viewport), and `ui_scale`.

WebKit's Westeros holepunch path reports the video rectangle after the compositor
transform, including device scale, in **physical UI framebuffer coordinates**.
The broker therefore continues to pass physical UI and mode sizes to the adapter,
which scales by mode/UI per axis, rounding outward to even pixels (NV12 chroma is
subsampled). Passing logical dimensions here would apply device scale twice.
Native libwpe pointer and axis positions also remain physical; WebKit divides
positions by device scale for DOM events, so input bounds must not become logical.
These contracts were checked against libwpe's public scale API, Cog's DRM backend,
and upstream WebKit 2.48's `ThreadedCompositor`,
`CoordinatedPlatformLayerBufferHolePunch`, and libwpe event factory. No board was
accessed for this change: the installed WebKit build still needs verification of
exported 3840×2160 buffers, the default 3840×2160 CSS viewport/DPR 1, pointer
alignment and inset video rectangle alignment (also at explicit scale 2), plus
frame rate and memory use.
The adapter also constrains decoder output to linear `NV12` before the sink;
without it a stateless decoder can negotiate the tiled `NV12_32L32`, which the
display shows as garbage.

The browser brokers its DRM file descriptor to the adapter so UI and video
share the same DRM ownership. The broker requires an ARGB primary plane with
premultiplied alpha blending and an NV12 overlay that can sit below it. It
configures and checks `zpos` and blend properties, then the video sink puts the
decoded video on that lower plane. WebKit makes the matching browser rectangle
transparent. This keeps video outside the browser's GPU texture-composition
path while allowing page content around it.

A CRTC accepts one commit per vblank. When `kmssink` updated the video plane
with its own (legacy, blocking `SETPLANE`) commit, the browser's commits and
the video's alternated vblanks: a 30 fps video left the UI at about 29 fps of
the 60 Hz mode, and the browser's commit ioctl blocked for up to 15 ms behind
the video's. The adapter therefore uses its own sink: it imports each decoded
DMA-BUF frame once as an NV12 framebuffer on the brokered descriptor, fits it
into the rectangle (aspect preserved, clipped to the CRTC) and sends the
framebuffer and plane coordinates over the broker connection, which stays
open. The browser adds the video plane to its next atomic commit, together with
a UI frame when one is ready, and tells the sink when a framebuffer is no
longer on screen so the decoder can reuse it. Measured on the T527 at
3840×2160@60 with the bundled 30 fps clip: UI 57.5 fps with video at 30 fps
(previously 28.7 fps), 57.5 fps without video (previously 55). Software
decoding (`avdec_h264`) and `SIGNAGE_VIDEO_SINK=kmssink` keep `kmssink`. The
browser logs commit statistics (`Commits: ... ioctl avg ..., submit->flip
avg ..., EBUSY ...`) next to each frame-rate line; `video-status.json` reports
`video_sink` (`browser-commit` or `kmssink`).

The current adapter supports one active video sink. Multiple simultaneous HTML
videos, CSS transforms, rotated output, HDR and arbitrary crop effects are
outside this first implementation. `SIGNAGE_VIDEO_MODE=composited` provides a
comparison using WebKit's ordinary video composition path.
The initial codec proof is H.264 through
[`v4l2slh264dec`](https://gstreamer.freedesktop.org/documentation/v4l2codecs/v4l2slh264dec.html);
HEVC (`v4l2slh265dec`) and VP9 (`v4l2slvp9dec`) are allowed too.

Decoding and the holepunch sink run inside WebKit's bubblewrap sandbox, which
stays enabled. WebKit itself device-binds only `/dev/video0-2` and
`/dev/media0`, and `webkit_web_context_add_path_to_sandbox()` refuses `/dev`
paths (its binds are `nodev` anyway). When a USB camera takes `media0`, the
decoder's media controller falls outside that list. The image therefore
diverts `/usr/bin/bwrap` to `/usr/bin/bwrap.real` and installs
`signage-bwrap` in its place: it device-binds every node under
`/sys/bus/media/devices` and `/sys/class/video4linux` at each web-process
spawn, then runs the real bwrap. The browser also binds
`/run/admiral-signage/video` (DRM broker socket, `video-status.json`) into the
sandbox. The browser log lists the exposed nodes (`WebKit sandbox: ...`).
The [KMS sink](https://gstreamer.freedesktop.org/documentation/kms/index.html)
accepts decoded DMA-BUF frames; format/modifier compatibility and zero-copy
scanout must be established separately on the board.

## Build and export

Build the image for the board, including from an ARM64 Mac with Docker Desktop:

```sh
docker buildx build --platform linux/arm64 --load \
  -t admiral-signage:dev .
```

Export an OCI image archive when a local image is not needed:

```sh
mkdir -p output
docker buildx build --platform linux/arm64 \
  -t admiral-signage:dev \
  --output type=oci,dest=output/admiral-signage-arm64.oci.tar .
```

The Dockerfile builds both native components and generates a reproducible test
pattern video: 12 seconds, 1280×720, 30 fps, H.264 baseline, no audio. Media
generation uses the build machine's software encoder; runtime hardware decode
is a separate operation. The bundled clip makes the initial screen independent
of Internet access.

On a dedicated Linux board with direct hardware access, the development Compose
configuration mounts the local page, service and native sources for iteration:

```sh
docker compose up --build
```

To use only the image and a persistent data volume, run:

```sh
docker run --rm --privileged --network host \
  -v signage-data:/data \
  admiral-signage:dev
```

Stop any other program that owns the same DRM display before this direct KMS
run. Docker Desktop's VM cannot expose the Mac's GPU or the T527 devices; it can
build and run the userspace smoke check, but board rendering requires the board.

```sh
docker run --rm admiral-signage:dev smoke
docker run --rm admiral-signage:dev doctor --quick
```

## Run on Admiral

Use the values in [config/admiral.example.json](config/admiral.example.json) when
creating a configuration. They follow Admiral's runtime
`api/container.Configuration` shape. Use the selected API endpoint's request
wrapper when creating or updating a configuration through the API.

- **Interactive Mode enabled:** gives the workload physical screen/input
  ownership and the TTY capability needed for graphical workloads.
- **Device Isolation disabled:** exposes host `/dev/dri`, `/dev/video*`,
  `/dev/media*`, `/dev/input` and `/dev/snd` through Admiral's normal device path.
- **Command empty:** uses the image's entrypoint and default `run` command.
- **Writable rootfs:** permits development builds inside the image.
- **Custom Volume:** mounts `signage-data` at `/data` to retain
  media, page overrides and logs across image updates.

The sample pins `:studio-20261010-live` for this presentation. For subsequent
rollouts, use a fresh version/commit tag or the published `@sha256:` digest. Admiral treats version/commit tags as immutable and can reuse
a cached tag; publish a new tag for every change.

The JSON schema uses `env` as a map, `volumes` as a map whose values have a
`destination`, and `ports` as objects with `port` and `protocol`. Host networking
is expressed as `options.namespaces: ["network"]`. Private networking is
sufficient for the default browser and local HTTP server.

The demo image binds HTTP to all interfaces by default. Its sample configuration adds
`{"port":8080,"protocol":"tcp"}` to `ports` in bridge mode. Set
`SIGNAGE_HTTP_HOST=127.0.0.1` for local-only access. The development endpoints do not authenticate requests, so enable
remote access only on your development network.

Admiral starts the process in `/`, mounts `/run` and `/tmp` as private tmpfs and
already exposes the hardware nodes. The entrypoint uses absolute paths, creates
its runtime directory on every start and supervises both required services.
Either required service exiting causes the root process to stop so Admiral can
restart it. Docker `HEALTHCHECK` instructions are not Admiral liveness probes.

The image starts `/signage-init`, a static copy of Debian's Tini, which forwards
signals to the workload process group and reaps orphaned children. The build
executes it in an otherwise empty root filesystem to verify it needs no dynamic
loader or shared libraries. CI also checks the complete entrypoint, HTTP startup
and SIGTERM shutdown with `/` as the working directory and fresh `/run` and `/tmp`.

If Admiral reports a startup executable as missing, record the exact error and
image digest. Linux can report `no such file or directory` for an existing
dynamic executable when its ELF loader is missing. The static init avoids that
ambiguity for PID 1; Python, WPE and the other programs still require an intact
image filesystem. Deploy the newly published commit tag or digest with Command
empty. An error still naming `/usr/bin/tini` means the old entrypoint or a command
override is being used. If the error instead names the child entrypoint, shell
or Python, inspect the extracted filesystem and any mounts covering its paths.

The workload does not hardcode the Admiral console's VT layout. Admiral owns
the display handoff; the browser uses DRM/KMS after that handoff. Ordinary
rootfs edits survive service restarts but are replaced on image/configuration
updates. Keep durable artifacts in the Custom Volume.

See Admiral's [launch contract](https://docs.admrl.co/core-concepts/configurations),
[hardware access](https://docs.admrl.co/core-concepts/hardware-access),
[platform boundary](https://docs.admrl.co/core-concepts/platform-boundary) and
[registry credentials](https://docs.admrl.co/core-concepts/registry-credentials).

## Development controls

| Variable | Default | Purpose |
| --- | --- | --- |
| `SIGNAGE_URL` | `http://127.0.0.1:8080/` | Browser startup URL. |
| `SIGNAGE_PAGE` | `player` in the image | `player` starts the published loop; `studio` opens the manager; `displaylab` and `menuboard` retain the original demos. Ignored when `SIGNAGE_URL` is set. |
| `SIGNAGE_VIDEO_URL` | Local `/media/test-h264.mp4` URL | HTML video source on the information screen. |
| `SIGNAGE_VIDEO_MODE` | `holepunch` | `holepunch` or `composited` comparison. |
| `SIGNAGE_DECODER` | `v4l2slh264dec,v4l2slh265dec,v4l2slvp9dec` | Comma-separated allowed decoders; any one of the stateless H.264/HEVC/VP9 decoders, or `avdec_h264` with software opt-in (then the default is `avdec_h264`). |
| `SIGNAGE_ALLOW_SOFTWARE` | `0` | Set `1` to permit software decoding during experiments. |
| `SIGNAGE_DRM_DEVICE` | Auto-detect | Display DRM card path, independent of GPU render-node numbering. |
| `SIGNAGE_RENDER_DEVICE` | Display GBM/kmsro | Optional GPU render node (for example `/dev/dri/renderD128`) for EGL; frames are imported into the separate display card, or GPU-copied into buffers allocated from it. |
| `SIGNAGE_DRM_MODE` | Auto-select | Display mode override as `WIDTHxHEIGHT@REFRESH`. |
| `SIGNAGE_UI_SIZE` | Native mode size | Physical framebuffer size: `native` uses the mode; `WIDTHxHEIGHT` must not exceed it. Smaller buffers are an opt-in performance choice, scaled to the mode by the primary plane. |
| `SIGNAGE_UI_SCALE` | `1` | WPE device scale, finite 0.05–5; framebuffer/scale must give integer logical dimensions. Default native 4K has a 3840×2160 CSS viewport; explicit scale `2` gives 1920×1080 CSS on the same 3840×2160 framebuffer. |
| `SIGNAGE_DRM_CONNECTOR` | Auto-select | Display connector ID or name. |
| `SIGNAGE_DRM_PRIMARY_PLANE` | Auto-select | Browser primary-plane ID. |
| `SIGNAGE_VIDEO_PLANE_ID` | Auto-select | Compatible NV12 video-overlay ID. |
| `SIGNAGE_VIDEO_SINK` | Browser commit | Set `kmssink` to let `kmssink` commit the video plane separately (comparison; halves the UI frame rate while video plays). |
| `SIGNAGE_FRAME_CALLBACK` | On commit | Set `flip` to release WebKit's frame callback only after the page flip. |
| `SIGNAGE_INPUT` | Enabled | Set `0` to disable native keyboard/pointer input and metadata preparation. |
| `SIGNAGE_INPUT_SEAT` | `seat0` | Input seat for initial event-device classification and libinput. |
| `SIGNAGE_KEYBOARD_LAYOUT` | System/US | XKB keyboard layout. |
| `SIGNAGE_HTTP_HOST` / `SIGNAGE_HTTP_PORT` | `0.0.0.0` / `8080` in the image | Local information/diagnostic server. |
| `SIGNAGE_DATA_DIR` | `/data` | Durable media, page overrides and diagnostic logs. |
| `WEBKIT_INSPECTOR_SERVER` | Unset | Opt-in WebKit remote inspector address, such as `127.0.0.1:9222`. |
| `GST_DEBUG` | Detailed V4L2/KMS/adapter categories | GStreamer logging; override to focus a capture. |
| `GST_DEBUG_DUMP_DOT_DIR` | Data directory `/logs` | Pipeline graphs produced by components that request a dump. |
| `G_MESSAGES_DEBUG` | `all` | GLib/WebKit/browser messages. |
| `LIBGL_DEBUG` / `EGL_LOG_LEVEL` | `verbose` / `debug` | Mesa and EGL diagnostics. |
| `XDG_CACHE_HOME` | `$SIGNAGE_DATA_DIR/cache` (else `/tmp/admiral-signage-cache`) | Fontconfig and other caches; must be writable. |
| `GST_REGISTRY` | `$XDG_CACHE_HOME/gstreamer-1.0/registry-<hash>.bin` | Plugin registry, keyed by kernel release and `/dev/video*`/`/dev/media*` nodes so a BSP change forces a rescan. |

The broker automatically chooses the available browser/video stacking levels.
A plane selection override is accepted only if its format, CRTC, alpha and
stacking requirements pass. If the BSP cannot provide this combination,
video playback fails with a diagnostic while the information screen stays available.

In holepunch mode, the entrypoint sets these WebKit controls unless explicitly
supplied:

```text
WEBKIT_GST_HOLE_PUNCH_QUIRK=westeros
WEBKIT_GST_QUIRKS=westeros
WEBKIT_GST_USE_PLAYBIN3=1
```

It ranks the allowed stateless decoders (H.264, HEVC, VP9) ahead of others and
disables software H.264 fallback by default. Explicit software experiments use both:

```text
SIGNAGE_ALLOW_SOFTWARE=1
SIGNAGE_DECODER=avdec_h264
```

The holepunch adapter also enforces the requested decoder against actual output
buffers, so changing ranks cannot silently enable a software fallback. In
`composited` mode the adapter is bypassed; verify the actual pipeline separately.

Native keyboard, pointer and scrolling events are forwarded to WPE through
libinput and XKB. Touchscreen forwarding is not implemented. On Admiral, the
entrypoint classifies initial `/sys/class/input/event*` devices using the
read-only `udevadm test-builtin input_id` command and creates only their metadata
in the workload's private `/run/udev/data`. It preserves any existing or mounted
udev database, including the Compose configuration's host database. This
preparation does not run udev rules, start a daemon, trigger host events or
change `/dev` permissions. Initial discovery is supported; restart the workload
after attaching a new input device to refresh private metadata. Check browser
logs for the discovered device count if input is unavailable. Private metadata
contains basic input classification and seat assignment; device-specific udev
rules, hardware database overrides and calibration are not synthesized.

Useful entrypoint operations:

```text
run                 Start the information server and browser (default).
doctor [--quick]    Print devices, package versions and native diagnostic status.
smoke               Check userspace installation without requiring board hardware.
server              Run the information server without the browser.
shell               Open a development shell.
<executable> ...    Run an explicit command instead of the normal workload.
```

Inside the workload, `signage-doctor`, `signage-smoke` and `signage-rebuild` are
also installed. Development tools include `gdb`, `strace`, `gst-inspect-1.0`,
`gst-launch-1.0`, `eglinfo`, `drm_info`, `modetest` and `v4l2-ctl`.

Edit `/opt/signage/src/browser` or `/opt/signage/src/native`, then run
`signage-rebuild` and restart the workload to use the rebuilt binaries. Commit
the same source changes in this repository and rebuild/publish the OCI image to
retain them through an update. This workload does not install kernel modules.

Files in the Custom Volume override bundled files individually:

```text
/data/www/index.html   overrides the local page
/data/www/app.js       overrides the page script
/data/www/style.css    overrides page styles
/data/media/clip.mp4   served as /media/clip.mp4
/data/logs/startup.json
```

Set `SIGNAGE_VIDEO_URL=http://127.0.0.1:8080/media/clip.mp4` for a replacement
clip. Missing override files fall back to the bundled assets. Refresh the page
after edits. Native live evidence is in `/run/admiral-signage/drm-status.json`,
`browser-status.json` and `video/video-status.json`; `/api/status` exposes the current snapshot and
`/api/config` exposes page configuration. `/healthz` checks only the HTTP server.

## Menuboard demo (SUBSTATION)

`www/menuboard/` is a second start page: a fake sandwich-shop menuboard ("SUBSTATION",
original CSS/SVG artwork, AUD prices) designed for exactly 1920×1080 CSS pixels. The Display
Lab page stays the default. Select the menuboard with either:

```text
SIGNAGE_PAGE=menuboard                          # entrypoint maps it to http://127.0.0.1:8080/menuboard/
SIGNAGE_URL=http://127.0.0.1:8080/menuboard/    # explicit URL (wins over SIGNAGE_PAGE)
```

The scene controller loops forever: **A** main menu (18 s) → **B** full-screen
`/media/promo-a.mp4` (until `ended`, 10 s) → **A** (18 s) → **C** meal deals with
`/media/promo-b.mp4` in the bottom-right 640×360 box (20 s) → repeat. Each hop is a
page-only green wipe. Scene C plays its clip once, then fades a page-drawn poster over the
box and releases the video so the rest of the scene holds a still.

Holepunch constraints honoured: one `<video>` exists at a time (created while the wipe covers
the screen, `src` removed and element deleted before the next scene); nothing transforms or
fades the video or its ancestors; the video box is axis-aligned; a video `error` or stall
skips to the next scene after 1.5 s. The first ~0.5 s of each clip plays behind the wipe.

QA parameters: `?scene=A|B|C&freeze=1[&t=3]` shows one scene statically (video seeked to `t`
and paused), `?adur=15..20`, `?cdur=`, `?srca=`/`?srcb=` override clips, `?log=1` mirrors
the transition log to the console (`window.__menuLog` always holds it).

The type is DejaVu Sans Condensed Bold, which comes from `fonts-dejavu-extra` (added to the
image; `fonts-dejavu-core` has no condensed face). Clips live in `media/` and are copied into
`/opt/signage/media`. Regenerate them from the source files (audio stripped, video remuxed,
faststart) with:

```sh
scripts/make-menuboard-media.sh SOURCE_A.mp4 SOURCE_B.mp4
# = ffmpeg -i SRC -an -c:v copy -movflags +faststart media/promo-{a,b}.mp4
```

Both clips must be H.264 (the allowed stateless decoders also take HEVC/VP9). Files in the Custom
Volume (`/data/media/promo-a.mp4`) override the bundled ones.

### 4K HEVC demo clip

`media/demo-4k-hevc.mp4` is a 10 s, 3840×2160, 50 fps HEVC Main (L5.1) clip with no audio. It backs
the built-in library asset "4K HEVC demo" (`/media/demo-4k-hevc.mp4`). It is not in the default loop;
add it from the library. It is 50 fps because that is what the T527 plays without drops: cedrus at
528 MHz decodes 4K HEVC at about 86 fps into its tiled format but only about 64 fps (about 55 fps
while the browser is presenting) into the linear NV12 the display planes accept, so a 4K60 clip
falls behind and GStreamer drops frames for QoS. Measured on the board: 4K50 and 4K30 play with no
dropped frames. Regenerate it with:

```sh
scripts/make-demo-4k-hevc.sh [SOURCE.mp4]   # with no argument, downloads the source into $TMPDIR
```

Attribution: derived from "Test Jellyfin 4K HEVC 8bit 40M.mp4" by Gnattu, from the Jellyfin test
videos (https://repo.jellyfin.org/test-videos/), licensed CC BY-SA 4.0. The clip is trimmed and
re-encoded. This file is distributed under CC BY-SA 4.0.

## Board acceptance runbook

Record the image digest, Admiral version, kernel release, package versions and
connected display mode for every result. Target-board acceptance has not been
performed in the local Mac development environment. Test the three paths
independently:

1. **GPU rendering:** inspect the animated WebGL panel and its reported renderer.
   Check native EGL logs for the Panfrost/Mali device, and reject `llvmpipe`,
   `softpipe` or another software renderer as GPU proof. Check the browser log for
   the scanout path (direct, or `GPU copy ready` after the logged direct-import
   failure) and that repeated primary-plane commits succeed at the selected mode.
2. **Stateless decode:** inspect `gst-inspect-1.0 v4l2slh264dec`, the media/video
   nodes and V4L2 controls. Then play the bundled H.264 clip inside the browser.
   A decoder factory being listed is only availability. Hardware decode becomes
   verified only when the adapter observes an actual decoded buffer attributable
   to `v4l2slh264dec` while playback is active. Record the pipeline, buffers and
   adapter evidence; software playback must stay labelled unverified.
3. **Holepunch and DE35 blending:** verify the adapter shares the browser's DRM
   descriptor and uses the selected NV12 overlay. On the physical panel, confirm
   video is visible inside the page's video rectangle, the surrounding UI and
   WebGL stay intact, and video placement tracks the HTML element. Inspect host
   DRM plane state to confirm video below an ARGB UI plane with the intended
   alpha blend. Pause/resume, seek, loop and restart to check plane lifecycle.

The screen deliberately keeps holepunch `verified=false`: configuring plane
properties and submitting buffers cannot automatically prove that the panel
shows correct blending. Save the visual observation with host plane-state
evidence as the acceptance result. Admiral does not expose `debugfs`/`tracefs`
inside the workload; DRM state under `/sys/kernel/debug/dri` and kernel tracing
must be collected in the platform/host debugging context.

If video fails, first distinguish missing stateless decoder controls from
DMA-BUF import/format failures and from DE35 plane stacking/blending. Use
`composited` mode to compare browser rendering, and explicit software decode
only when isolating the video presentation path. Neither comparison establishes
the target hardware-decode-plus-holepunch path.

## GHCR publishing

The GitHub Actions workflow builds ARM64 images for pull requests and publishes
to `ghcr.io/<repository-owner>/workload-admiral-signage` on pushes to `main` and
version tags. Authentication uses the repository's `GITHUB_TOKEN` with package
write permission. Commit tags and the reported digest identify test builds;
`latest` follows `main` for development.

Push the repository to GitHub to run the workflow. For a private GHCR package,
configure the package's pull credentials in Admiral. Choose package visibility
in GitHub as appropriate for the workload. No registry credentials are baked
into the image.
