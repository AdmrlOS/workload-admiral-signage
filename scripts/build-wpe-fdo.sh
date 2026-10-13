#!/bin/sh
# Build a pinned WPEBackend-fdo with a compositor-side RGB modifier policy.
# No EGL interposition: advertise only actual driver-reported linear RGB pairs.
set -eu
src=${1:-/opt/signage/src/wpebackend-fdo}
archive=${SIGNAGE_FDO_ARCHIVE:-/tmp/wpebackend-fdo-1.16.0.tar.xz}
if [ ! -f "$archive" ]; then
    curl -fL https://wpewebkit.org/releases/wpebackend-fdo-1.16.0.tar.xz -o "$archive"
fi
printf '%s  %s\n' beddf321232d5bd08106c179dbc600f8ce88eb3620b4a59a6329063b78f64635 "$archive" | sha256sum -c -
mkdir -p "$src"
tar xf "$archive" -C "$src" --strip-components=1
cp "$(dirname "$0")/../browser/fdo-modifier-policy.h" "$src/src/"
python3 - "$src/src/ws-egl.cpp" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
s = s.replace('#include "ws-egl.h"', '#include "ws-egl.h"\n#include "fdo-modifier-policy.h"\n#include <cstdlib>')
old = '            callback(formats[i], modifiers[j]);'
assert s.count(old) == 1, 'Pinned source modifier advertisement changed'
s = s.replace(old, '            if (signage_fdo_advertise_modifier(std::getenv("SIGNAGE_UI_MODIFIER"), formats[i], modifiers[j]))\n                callback(formats[i], modifiers[j]);')
p.write_text(s)
PY
meson setup "$src/build" "$src" --prefix=/usr/local --libdir=lib/aarch64-linux-gnu -Dbuild_docs=false --buildtype=release
meson compile -C "$src/build"
meson install -C "$src/build"
/sbin/ldconfig
