#!/bin/sh
# Produce the menuboard demo clips served as /media/promo-a.mp4 and /media/promo-b.mp4.
#
# The sources are 1280x720, 24 fps, H.264 High, yuv420p with an AAC track. The
# board has no HDMI audio, so the audio is dropped; the video stream is remuxed
# (no re-encode) and the moov atom moved to the front for progressive playback.
#
# usage: scripts/make-menuboard-media.sh SOURCE_A.mp4 SOURCE_B.mp4
set -eu
[ $# -eq 2 ] || { echo "usage: $0 SOURCE_A.mp4 SOURCE_B.mp4" >&2; exit 2; }
out="$(dirname "$0")/../media"
mkdir -p "$out"
ffmpeg -hide_banner -y -i "$1" -an -c:v copy -movflags +faststart "$out/promo-a.mp4"
ffmpeg -hide_banner -y -i "$2" -an -c:v copy -movflags +faststart "$out/promo-b.mp4"
