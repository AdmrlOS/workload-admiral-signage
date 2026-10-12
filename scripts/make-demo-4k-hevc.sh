#!/bin/sh
# Reproduce media/demo-4k-hevc.mp4: a 10 s, 3840x2160, 50 fps HEVC Main (L5.1)
# clip for the built-in "4K HEVC demo" library asset (not in the default loop).
#
# Source: "Test Jellyfin 4K HEVC 8bit 40M.mp4" by Gnattu, from the Jellyfin test
# video repository (https://repo.jellyfin.org/test-videos/), licensed CC BY-SA 4.0.
# The clip is trimmed to 8-18 s and re-encoded; audio is dropped (the board has
# no HDMI audio path).
#
# usage: scripts/make-demo-4k-hevc.sh [SOURCE.mp4]
# With no argument the source is downloaded into the scratch directory.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/media/demo-4k-hevc.mp4"
src="${1:-}"
if [ -z "$src" ]; then
  src="${TMPDIR:-/tmp}/Test Jellyfin 4K HEVC 8bit 40M.mp4"
  if [ ! -s "$src" ]; then
    curl -fL --retry 3 -o "$src" \
      "https://repo.jellyfin.org/test-videos/SDR/HEVC%208bit/Test%20Jellyfin%204K%20HEVC%208bit%2040M.mp4"
  fi
fi
ffmpeg -hide_banner -y -ss 8 -i "$src" -t 10 -an -vf fps=50 \
  -c:v libx265 -preset medium -profile:v main -pix_fmt yuv420p \
  -x265-params crf=22:keyint=100:min-keyint=100:vbv-maxrate=25000:vbv-bufsize=50000:level-idc=5.1 \
  -tag:v hvc1 -movflags +faststart "$out"
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,nb_frames -of compact "$out"
