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
its faster AFBC buffers. The browser status file reports `scanout_path`
(`direct` or `gpu-copy`). Mesa's Panfrost path is intended to provide
OpenGL ES on the T527's Mali-G57. OpenGL ES 3.1 is the target; this workload does
not depend on Vulkan. The browser's WebGL renderer and the native EGL/DRM logs
must agree before calling the rendering path accelerated.

In the default `holepunch` mode, WebKit manages the real HTML `<video>` element,
playback and its window rectangle. The entrypoint enables WebKit's existing
Westeros holepunch quirk. Our `westerossink` compatibility adapter implements the
properties that this WebKit path expects and delegates video scanout to
GStreamer's `kmssink`. This is an owned compatibility adapter, not the Westeros
compositor. No external compositor is involved.

The web view does not render at the display mode. `SIGNAGE_UI_SIZE` sets the view
and framebuffer size; by default a mode larger than 1920×1080 renders at the
largest size within 1920×1080 that keeps the mode's aspect ratio (3840×2160 →
1920×1080) and the primary plane's scaler stretches it over the whole CRTC
(`SRC_W/H` = UI size, `CRTC_W/H` = mode size). Before committing to scaling the
browser runs an atomic `TEST_ONLY` commit of the scaled configuration; if the
kernel rejects it, it logs the failure and falls back to UI size = mode size. The
status JSON reports `width`/`height` (mode) and `ui_width`/`ui_height`.

WebKit reports the video rectangle in web-view (UI) coordinates, while the NV12
overlay is positioned in CRTC coordinates. The broker passes the UI and mode sizes
to the adapter, which scales the rectangle by mode/UI per axis, rounding outward
to even pixels (NV12 chroma is subsampled), before setting `kmssink`'s
`render-rectangle`. With equal sizes the rectangle passes through unchanged.
The adapter also constrains decoder output to linear `NV12` before `kmssink`;
without it a stateless decoder can negotiate the tiled `NV12_32L32`, which the
display shows as garbage.

The browser brokers its DRM file descriptor to the adapter so UI and video
share the same DRM ownership. The broker requires an ARGB primary plane with
premultiplied alpha blending and an NV12 overlay that can sit below it. It
configures and checks `zpos` and blend properties, then the video sink puts the
decoded video on that lower plane. WebKit makes the matching browser rectangle
transparent. This keeps video outside the browser's GPU texture-composition
path while allowing page content around it.

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
  -v signage-data:/var/lib/admiral-signage \
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
- **Custom Volume:** mounts `signage-data` at `/var/lib/admiral-signage` to retain
  media, page overrides and logs across image updates.

The sample uses `:latest` intentionally for experimentation. For a repeatable
test or rollout, replace it with the workflow's unique commit tag or published
`@sha256:` digest. Admiral treats version/commit tags as immutable and can reuse
a cached tag; publish a new tag for every change.

The JSON schema uses `env` as a map, `volumes` as a map whose values have a
`destination`, and `ports` as objects with `port` and `protocol`. Host networking
is expressed as `options.namespaces: ["network"]`. Private networking is
sufficient for the default browser and local HTTP server.

The HTTP server binds to loopback by default. To inspect it from another machine,
set `SIGNAGE_HTTP_HOST=0.0.0.0` and add
`{"port":8080,"protocol":"tcp"}` to `ports` in bridge mode, or opt into host
networking. The development endpoints do not authenticate requests, so enable
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
| `SIGNAGE_VIDEO_URL` | Local `/media/test-h264.mp4` URL | HTML video source on the information screen. |
| `SIGNAGE_VIDEO_MODE` | `holepunch` | `holepunch` or `composited` comparison. |
| `SIGNAGE_DECODER` | `v4l2slh264dec,v4l2slh265dec,v4l2slvp9dec` | Comma-separated allowed decoders; any one of the stateless H.264/HEVC/VP9 decoders, or `avdec_h264` with software opt-in (then the default is `avdec_h264`). |
| `SIGNAGE_ALLOW_SOFTWARE` | `0` | Set `1` to permit software decoding during experiments. |
| `SIGNAGE_DRM_DEVICE` | Auto-detect | Display DRM card path, independent of GPU render-node numbering. |
| `SIGNAGE_RENDER_DEVICE` | Display GBM/kmsro | Optional GPU render node (for example `/dev/dri/renderD128`) for EGL; frames are imported into the separate display card, or GPU-copied into buffers allocated from it. |
| `SIGNAGE_DRM_MODE` | Auto-select | Display mode override as `WIDTHxHEIGHT@REFRESH`. |
| `SIGNAGE_UI_SIZE` | Mode size, or largest ≤1920×1080 of the mode's aspect ratio | Web view size as `WIDTHxHEIGHT`, no larger than the mode; the primary plane scales it to the mode. |
| `SIGNAGE_DRM_CONNECTOR` | Auto-select | Display connector ID or name. |
| `SIGNAGE_DRM_PRIMARY_PLANE` | Auto-select | Browser primary-plane ID. |
| `SIGNAGE_VIDEO_PLANE_ID` | Auto-select | Compatible NV12 video-overlay ID. |
| `SIGNAGE_INPUT` | Enabled | Set `0` to disable native keyboard/pointer input and metadata preparation. |
| `SIGNAGE_INPUT_SEAT` | `seat0` | Input seat for initial event-device classification and libinput. |
| `SIGNAGE_KEYBOARD_LAYOUT` | System/US | XKB keyboard layout. |
| `SIGNAGE_HTTP_HOST` / `SIGNAGE_HTTP_PORT` | `127.0.0.1` / `8080` | Local information/diagnostic server. |
| `SIGNAGE_DATA_DIR` | `/var/lib/admiral-signage` | Durable media, page overrides and diagnostic logs. |
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
/var/lib/admiral-signage/www/index.html   overrides the local page
/var/lib/admiral-signage/www/app.js       overrides the page script
/var/lib/admiral-signage/www/style.css    overrides page styles
/var/lib/admiral-signage/media/clip.mp4   served as /media/clip.mp4
/var/lib/admiral-signage/logs/startup.json
```

Set `SIGNAGE_VIDEO_URL=http://127.0.0.1:8080/media/clip.mp4` for a replacement
clip. Missing override files fall back to the bundled assets. Refresh the page
after edits. Native live evidence is in `/run/admiral-signage/drm-status.json`,
`browser-status.json` and `video/video-status.json`; `/api/status` exposes the current snapshot and
`/api/config` exposes page configuration. `/healthz` checks only the HTTP server.

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
