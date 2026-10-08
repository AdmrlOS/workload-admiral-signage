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
DMA-BUF frames; the shell imports them and submits an ARGB browser primary plane
directly through atomic DRM/KMS. Mesa's Panfrost path is intended to provide
OpenGL ES on the T527's Mali-G57. OpenGL ES 3.1 is the target; this workload does
not depend on Vulkan. The browser's WebGL renderer and the native EGL/DRM logs
must agree before calling the rendering path accelerated.

In the default `holepunch` mode, WebKit manages the real HTML `<video>` element,
playback and its window rectangle. The entrypoint enables WebKit's existing
Westeros holepunch quirk. Our `westerossink` compatibility adapter implements the
properties that this WebKit path expects and delegates video scanout to
GStreamer's `kmssink`. This is an owned compatibility adapter, not the Westeros
compositor. No external compositor is involved.

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
[`v4l2slh264dec`](https://gstreamer.freedesktop.org/documentation/v4l2codecs/v4l2slh264dec.html).
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
| `SIGNAGE_DECODER` | `v4l2slh264dec` | Stateless H.264 decoder; `avdec_h264` requires software opt-in. |
| `SIGNAGE_ALLOW_SOFTWARE` | `0` | Set `1` to permit software decoding during experiments. |
| `SIGNAGE_DRM_DEVICE` | Auto-detect | Display DRM card path, independent of GPU render-node numbering. |
| `SIGNAGE_RENDER_DEVICE` | Display GBM/kmsro | Optional GPU render node (for example `/dev/dri/renderD128`) for EGL; DMA-BUF frames are imported into the separate display card. |
| `SIGNAGE_DRM_MODE` | Auto-select | Display mode override as `WIDTHxHEIGHT@REFRESH`. |
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
| `PAN_MESA_DEBUG` | Unset | Optional Panfrost modifier experiments; see below. |

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

It ranks stateless H.264 ahead of other decoders and disables software H.264
fallback by default. Explicit software experiments use both:

```text
SIGNAGE_ALLOW_SOFTWARE=1
SIGNAGE_DECODER=avdec_h264
```

The holepunch adapter also enforces the requested decoder against actual output
buffers, so changing ranks cannot silently enable a software fallback. In
`composited` mode the adapter is bypassed; verify the actual pipeline separately.

For DMA-BUF modifier bring-up, `PAN_MESA_DEBUG=noafbc,linear` is an optional
comparison: it disables AFBC and forces linear textures in Mesa's Panfrost
driver. The browser reports the actual modifier and DRM framebuffer import
failure when DE35 cannot scan out the exported frame. This debug setting is not
a default or a replacement for matching browser export and plane capabilities.
The flags are defined in [Mesa's Panfrost driver source](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-25.0.7/src/gallium/drivers/panfrost/pan_screen.c).

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
`browser-status.json` and `video-status.json`; `/api/status` exposes the current snapshot and
`/api/config` exposes page configuration. `/healthz` checks only the HTTP server.

## Board acceptance runbook

Record the image digest, Admiral version, kernel release, package versions and
connected display mode for every result. Target-board acceptance has not been
performed in the local Mac development environment. Test the three paths
independently:

1. **GPU rendering:** inspect the animated WebGL panel and its reported renderer.
   Check native EGL logs for the Panfrost/Mali device, and reject `llvmpipe`,
   `softpipe` or another software renderer as GPU proof. Ensure browser DMA-BUF
   imports and repeated primary-plane commits succeed at the selected mode.
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
