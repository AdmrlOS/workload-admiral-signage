# Native 4K scheduling verification

The browser remains the sole DRM atomic committer. One pending flip is allowed.
An unfinished UI-copy fence is polled without blocking; the queued UI stays owned
while newer video commits against the completed primary framebuffer. WPE source
buffers are retained until copy completion, including replaced queued copies.
The sink drains RELEASED independently every 5 ms, guarded by the same mutex as
render/stop and serial validation. Child-owned duplicate descriptors survive
parent closure. Stop wakes/joins the receiver before parser/FB teardown. EOF or
hide timeout is not release proof: unacknowledged buffers are intentionally
retained until process exit (an error path, not normal pool recycling).

## Local checks

```
python3 -m unittest discover -s tests
node --test tests/*.js
```

With a development image containing compiler/headers (the existing Mac
`admiral-signage:dev` image works), no board/DRM access is needed:

```
docker run --rm --entrypoint sh -v "$PWD:/src:ro" admiral-signage:dev -c '
 cp -r /src/browser /src/native /src/tests /tmp/
 make -C /tmp/native && make -C /tmp/browser
 cc -Wall -Wextra -Werror -o /tmp/release-test /tmp/tests/native-release-lifecycle.c $(pkg-config --cflags --libs gstreamer-video-1.0 gstreamer-allocators-1.0 libdrm)
 GST_DEBUG=0 /tmp/release-test
'
```

The real sink lifecycle test performs 100 start/stop iterations with stale and
fragmented releases, no render activity, parent descriptor closure, and a release
racing hide/stop. The Mac parser/receiver harness additionally runs ASan/UBSan.

## Parent/kernel-owner board test (coordinate exclusive ownership first)

Do not run these while another agent owns the board. Do not change managed
configuration or the existing sigrun wrapper. Copy/build the branch in the
agreed development rootfs and use the parent-approved workload stop/start route.

Run the existing agreed development start command with:

```
SIGNAGE_DRM_MODE=3840x2160@60 SIGNAGE_UI_SIZE=native SIGNAGE_UI_SCALE=1 \
SIGNAGE_VIDEO_STATS=1 SIGNAGE_PAGE=player \
SIGNAGE_URL='http://127.0.0.1:8080/player/?debug=1' \
SIGROOT=/admrl/sigdev sh /admrl/sigrun.sh sh /sigstart.sh
```

Verify the start script does not override those env values. Select only the
restored 4K60 asset in the development library; preserve the managed playlist.
Capture full loops and mixed content transitions, along with:

```
SIGROOT=/admrl/sigdev sh /admrl/sigrun.sh ffprobe -v error -select_streams v:0 \
 -show_entries stream=width,height,r_frame_rate,nb_frames -of compact /opt/signage/media/demo-4k-hevc.mp4
cat /sys/kernel/debug/dri/0/state
cat /run/admrl/logs/default-workload.log
```

Paths/DRM card must be verified by the owner; development logs may be elsewhere.
Read the development namespace's browser-status.json and video/video-status.json
at two timestamps. `video_presented_total` counts new framebuffer transitions in
flip callbacks, excludes same-FB rectangle/expose commits; `decoded_frames`,
`sink_input_frames`, `basesink_rendered`, `basesink_dropped` and
`decoder_qos_dropped` are separate stages. Decoder QoS counts are supplied by the
decoder's GST_MESSAGE_QOS statistics, not inferred from currentTime. Counters are
per browser/sink lifetime, so do not subtract across sink recreation. Presented
counts still require camera confirmation; they are not proof of visible 60 fps.

Timings now use message arrival, accepted submission and flip callback; the
presented-hold->release metric includes the time a retired frame was actually
current, not a mislabeled submission interval. Callback delivery is not the
kernel's exact vblank timestamp. Static UI can correctly report zero UI fps.

Startup gates health readiness for up to 120 s; exhaustion exits nonzero rather
than launching an error-page browser. Navigation failures suppress the stock
error page and retry at 0.5/1/1.5/2 s, then exit after the fifth failure.

## Rendering and scanout boundaries

No resolution, device scale, animation cadence, blur appearance or design was
changed. Redundant diagnostic-page text writes are suppressed (10,000 unchanged
updates: before 10,000 writes, after 0). Decoder metadata strings/caps are now
serialized only when changed. These are operation-count reductions, not a
claimed on-board frame-rate gain. Whole-frame animations/blur require visual
comparison plus a controlled WPE benchmark before changing them; not completed.
Poster generation remains synchronous to preserve library correctness pending a
separate transactional background-worker implementation.

Broker message ABI and linear NV12 format contract are unchanged. AFBC direct UI
scanout needs actual ARGB alpha-preserving plane IN_FORMATS/modifier and atomic
TEST_ONLY acceptance; do not infer support from GPU OPP changes. Kernel ownership
and hardware performance acceptance remain with the parent/kernel agent.
