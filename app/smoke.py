"""Hardware-independent image checks; these never certify the target board."""
import json
import os
import subprocess
from pathlib import Path
from diagnostics import APP_ROOT, RUNTIME


def check(args, timeout=30):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Failed {args}:\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def main():
    version = check(["signage-browser", "--version"])
    print(version.strip())
    adapter = check(["gst-inspect-1.0", "westerossink"])
    for property_name in ("rectangle", "zorder", "res-usage"):
        assert property_name in adapter, property_name
    kms = check(["gst-inspect-1.0", "kmssink"])
    assert "fd " in kms and "skip-vsync" in kms
    library = next(Path("/usr/lib").glob("*/libWPEWebKit-2.0.so.1"))
    library_data = library.read_bytes()
    assert b"WEBKIT_GST_HOLE_PUNCH_QUIRK" in library_data and b"westeros" in library_data, "WPE quirk unavailable"
    clip = APP_ROOT / "media/test-h264.mp4"
    stream = json.loads(check(["ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,width,height,pix_fmt,profile", "-of", "json", str(clip)]))["streams"][0]
    assert stream["codec_name"] == "h264" and stream["width"] == 1280 and stream["height"] == 720
    assert stream["pix_fmt"] == "yuv420p"
    # Explicit software decode checks the bundled media, not hardware capability.
    check(["gst-launch-1.0", "-q", "filesrc", f"location={clip}", "!", "qtdemux", "!",
           "h264parse", "!", "avdec_h264", "!", "fakesink", "sync=false"])
    if not (RUNTIME / "drm.sock").exists():
        result = subprocess.run(["gst-launch-1.0", "-q", "videotestsrc", "num-buffers=1", "!",
            "video/x-raw,format=NV12", "!", "westerossink"], capture_output=True, text=True, timeout=15)
        assert result.returncode != 0, "Adapter must reject missing DRM broker"
        status = json.loads((RUNTIME / "video-status.json").read_text())
        assert status["hardware_decode"]["verified"] is False
        assert status["holepunch"]["verified"] is False
        assert status["error"], "Missing broker must produce diagnostic evidence"
    print("Image smoke checks passed. GPU, stateless decode and plane blending still require the T527 board.")


if __name__ == "__main__":
    main()
