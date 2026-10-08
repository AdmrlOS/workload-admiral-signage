# syntax=docker/dockerfile:1.7
# Development image: the toolchain and patched sources are intentionally retained.
ARG DEBIAN_IMAGE=debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
FROM ${DEBIAN_IMAGE} AS toolchain
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential meson ninja-build pkg-config curl ca-certificates xz-utils patch git \
    python3 dbus dbus-x11 tini udev kmod procps strace gdb \
    libwpewebkit-2.0-dev libwpebackend-fdo-1.0-dev libwpe-1.0-dev \
    libdrm-dev libgbm-dev libegl-dev libgles-dev libepoxy-dev libinput-dev libudev-dev \
    libwayland-dev wayland-protocols libxkbcommon-dev libmanette-0.2-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libjson-glib-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
    gstreamer1.0-gl gstreamer1.0-alsa libgl1-mesa-dri libegl-mesa0 libgbm1 \
    mesa-utils drm-info libdrm-tests v4l-utils ffmpeg graphviz fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

FROM toolchain AS workload
# Keep PID 1 independent of the distribution's ELF loader and /usr layout.
# Execute it in an empty root to prove it has no shared-library dependencies.
RUN install -m 0755 /usr/bin/tini-static /signage-init && \
    mkdir -p /tmp/init-check && cp /signage-init /tmp/init-check/signage-init && \
    chroot /tmp/init-check /signage-init --version && rm -rf /tmp/init-check
ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="Admiral Signage Development" \
      org.opencontainers.image.description="WPE WebKit, Panfrost WebGL and experimental stateless V4L2 KMS holepunch development workload" \
      org.opencontainers.image.source="https://github.com/admrlos/workload-admiral-signage" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}"
COPY native/ /opt/signage/src/native/
COPY browser/ /opt/signage/src/browser/
RUN make -C /opt/signage/src/native && make -C /opt/signage/src/native install && \
    make -C /opt/signage/src/browser && make -C /opt/signage/src/browser install && ldconfig
COPY app/ /opt/signage/app/
COPY www/ /opt/signage/www/
COPY bin/ /usr/local/bin/
RUN chmod +x /usr/local/bin/signage-* && \
    mkdir -p /opt/signage/media /var/lib/admiral-signage && \
    ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 \
      -t 12 -an -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
      -preset veryfast -crf 23 -g 60 -movflags +faststart /opt/signage/media/test-h264.mp4 && \
    dpkg-query -W > /opt/signage/package-versions.txt
ENV PYTHONUNBUFFERED=1 \
    GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 \
    XDG_RUNTIME_DIR=/run/admiral-signage \
    SIGNAGE_DATA_DIR=/var/lib/admiral-signage \
    SIGNAGE_HTTP_HOST=127.0.0.1 \
    SIGNAGE_HTTP_PORT=8080 \
    SIGNAGE_VIDEO_MODE=holepunch \
    SIGNAGE_ALLOW_SOFTWARE=0 \
    G_MESSAGES_DEBUG=all \
    GST_DEBUG_NO_COLOR=1 \
    GST_DEBUG="2,v4l2codecs:6,v4l2slh264dec:6,kmssink:6,admiral*:6,webkit*:4" \
    LIBGL_DEBUG=verbose \
    EGL_LOG_LEVEL=debug
EXPOSE 8080
STOPSIGNAL SIGTERM
ENTRYPOINT ["/signage-init", "-g", "--", "/usr/local/bin/signage-entrypoint"]
CMD ["run"]
