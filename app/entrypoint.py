"""Supervise the browser and diagnostics for Admiral's root-process contract."""
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from urllib.request import urlopen
from diagnostics import APP_ROOT, RUNTIME, snapshot


def configure():
    RUNTIME.mkdir(parents=True, exist_ok=True, mode=0o700)
    RUNTIME.chmod(0o700)
    data = Path(os.environ.get("SIGNAGE_DATA_DIR", "/var/lib/admiral-signage"))
    for name in ("logs", "media", "www"):
        (data / name).mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("GST_DEBUG_DUMP_DOT_DIR", str(data / "logs"))
    # v4l2codecs discovers factories from devices when the plugin loads. Don't
    # reuse a registry produced before a BSP update exposed the stateless nodes.
    os.environ.setdefault("GST_REGISTRY", str(RUNTIME / "gstreamer-registry.bin"))
    Path(os.environ["GST_REGISTRY"]).unlink(missing_ok=True)
    for name in ("video-status.json", "drm-status.json", "browser-status.json"):
        (RUNTIME / name).unlink(missing_ok=True)
    mode = os.environ.get("SIGNAGE_VIDEO_MODE", "holepunch")
    if mode not in ("holepunch", "composited"):
        raise ValueError("SIGNAGE_VIDEO_MODE must be holepunch or composited")
    if mode == "holepunch":
        os.environ.setdefault("WEBKIT_GST_HOLE_PUNCH_QUIRK", "westeros")
        os.environ.setdefault("WEBKIT_GST_QUIRKS", "westeros")
        os.environ.setdefault("WEBKIT_GST_USE_PLAYBIN3", "1")
    else:
        os.environ.pop("WEBKIT_GST_HOLE_PUNCH_QUIRK", None)
        os.environ.pop("WEBKIT_GST_QUIRKS", None)
    software = os.environ.get("SIGNAGE_ALLOW_SOFTWARE", "0") == "1"
    decoder = os.environ.get("SIGNAGE_DECODER", "avdec_h264" if software else "v4l2slh264dec")
    os.environ.setdefault("SIGNAGE_DECODER", decoder)
    ranks = "v4l2slh264dec:512,avdec_h264:0,openh264dec:0,v4l2h264dec:0"
    if software:
        ranks = "v4l2slh264dec:512,avdec_h264:256,openh264dec:0,v4l2h264dec:0"
    if decoder == "avdec_h264":
        if not software:
            raise ValueError("Software decoder requires SIGNAGE_ALLOW_SOFTWARE=1")
        ranks = "v4l2slh264dec:0,avdec_h264:512,openh264dec:0,v4l2h264dec:0"
    os.environ.setdefault("GST_PLUGIN_FEATURE_RANK", ranks)
    os.environ.setdefault("SIGNAGE_URL", f"http://127.0.0.1:{os.environ.get('SIGNAGE_HTTP_PORT', '8080')}/")
    return data


def main():
    args = sys.argv[1:] or ["run"]
    operation = args.pop(0)
    if operation == "shell":
        os.execvp("/bin/bash", ["/bin/bash", *args])
    if operation not in ("run", "doctor", "smoke", "server"):
        os.execvp(operation, [operation, *args])
    data = configure()
    if operation in ("doctor", "smoke", "server"):
        module = {"doctor": "diagnostics.py", "smoke": "smoke.py", "server": "server.py"}[operation]
        os.execv(sys.executable, [sys.executable, str(APP_ROOT / "app" / module), *args])
    from input_metadata import prepare
    (RUNTIME / "input-status.json").write_text(json.dumps(prepare(), indent=2))
    print("Starting Admiral signage; device detection does not attest acceleration.", flush=True)
    (data / "logs" / "startup.json").write_text(json.dumps(snapshot(), indent=2))
    processes = []
    stopping = False

    def shutdown(signum=None, frame=None):
        nonlocal stopping
        stopping = True
        for process in processes:
            if process.poll() is None:
                process.terminate()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    processes.append(subprocess.Popen([sys.executable, str(APP_ROOT / "app/server.py")]))
    for _ in range(100):
        if stopping or processes[0].poll() is not None:
            break
        try:
            with urlopen(f"http://127.0.0.1:{os.environ.get('SIGNAGE_HTTP_PORT', '8080')}/healthz", timeout=0.2):
                break
        except OSError:
            time.sleep(0.1)
    if not stopping and processes[0].poll() is None:
        processes.append(subprocess.Popen(["dbus-run-session", "--", "/usr/local/bin/signage-browser",
                                           os.environ["SIGNAGE_URL"], *args]))
    code = 0
    try:
        while not stopping:
            for process in processes:
                if process.poll() is not None:
                    print(f"Required service exited ({process.returncode}); Admiral should restart workload.", flush=True)
                    code = process.returncode or 1
                    shutdown()
                    break
            time.sleep(0.2)
    finally:
        shutdown()
        deadline = time.monotonic() + 5
        for process in processes:
            try:
                process.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    return code if code >= 0 else 128 - code


if __name__ == "__main__":
    sys.exit(main())
