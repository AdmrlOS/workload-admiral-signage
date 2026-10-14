#!/bin/sh
# Compile and run the native engine/sink/broker tests against the real GStreamer/GLib
# sources with ASAN+UBSAN inside the ARM64 Debian toolchain stage of this Dockerfile,
# then run the engine integration tests (real engine binary, real decoder) in the same
# container. Usage: tests/run-native-tests.sh [pattern]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${SIGNAGE_TEST_IMAGE:-admiral-signage-toolchain:local}
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker buildx build --platform linux/arm64 --target toolchain --load -t "$IMAGE" "$ROOT"
fi
exec docker run --rm --platform linux/arm64 -e SIGNAGE_TEST_PATTERN="${1:-}" -e SIGNAGE_SLOW="${SIGNAGE_SLOW:-}" \
    -e GST_DEBUG=1 -e G_MESSAGES_DEBUG= -v "$ROOT:/src:ro" "$IMAGE" bash -euo pipefail -c '
cp -a /src /work && cd /work
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
CFLAGS="-O1 -g -std=gnu11 -Wall -Wextra -Werror"
PKGS="gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 gstreamer-app-1.0 gio-unix-2.0 json-glib-1.0 libdrm"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export GST_REGISTRY=/tmp/gst-registry.bin
mkdir -p /tmp/bin
for test in tests/native-engine-sink.c tests/native-engine-broker.c tests/native-engine-core.c tests/native-engine-realtime.c; do
    [ -f "$test" ] || continue
    case "$test" in *"${SIGNAGE_TEST_PATTERN}"*) ;; *) continue ;; esac
    name=$(basename "$test" .c)
    gcc $CFLAGS $SAN -o /tmp/bin/$name "$test" $(pkg-config --cflags --libs $PKGS)
    echo "== $name"; /tmp/bin/$name
done
make -C native >/dev/null
gcc $CFLAGS $SAN -o /tmp/bin/signage-video-engine native/signage-video-engine.c $(pkg-config --cflags --libs $PKGS)
export SIGNAGE_ENGINE_BIN=/tmp/bin/signage-video-engine GST_PLUGIN_PATH=/work/native
case "engine" in *"${SIGNAGE_TEST_PATTERN}"*)
    python3 -m unittest -v tests.test_video_engine tests.test_engine_media tests.test_engine_relay ;;
esac
echo "ALL NATIVE TESTS PASSED"
'
