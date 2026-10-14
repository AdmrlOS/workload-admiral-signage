"""Supervise the browser and diagnostics for Admiral's root-process contract."""
import hashlib
import json
import os
import platform
import signal
import subprocess
import sys
import time
from pathlib import Path
from urllib.request import urlopen
from diagnostics import APP_ROOT, RUNTIME, VIDEO_RUNTIME, snapshot

# Stateless V4L2 decoders cedrus exposes; SIGNAGE_DECODER takes a comma-separated subset.
HARDWARE_DECODERS = ("v4l2slh264dec", "v4l2slh265dec", "v4l2slvp9dec")
SOFTWARE_DECODER = "avdec_h264"
DISABLED_DECODERS = ("openh264dec", "v4l2h264dec")


def decoder_config(environ):
    """Return (SIGNAGE_DECODER, GST_PLUGIN_FEATURE_RANK) for the environment."""
    software = environ.get("SIGNAGE_ALLOW_SOFTWARE", "0") == "1"
    default = SOFTWARE_DECODER if software else ",".join(HARDWARE_DECODERS)
    decoder = environ.get("SIGNAGE_DECODER") or default
    listed = [name.strip() for name in decoder.split(",")]
    unknown = [name for name in listed if name not in HARDWARE_DECODERS + (SOFTWARE_DECODER,)]
    if unknown:
        raise ValueError(f"Unsupported SIGNAGE_DECODER entries: {', '.join(unknown)}")
    if SOFTWARE_DECODER in listed and not software:
        raise ValueError("Software decoder requires SIGNAGE_ALLOW_SOFTWARE=1")
    ranks = {name: 512 for name in listed}
    for name in HARDWARE_DECODERS:
        ranks.setdefault(name, 0)
    ranks.setdefault(SOFTWARE_DECODER, 256 if software else 0)
    for name in DISABLED_DECODERS:
        ranks.setdefault(name, 0)
    return decoder, ",".join(f"{name}:{rank}" for name, rank in ranks.items())


PAGES = {"": "", "displaylab": "", "menuboard": "menuboard/", "player": "player/", "studio": "studio/"}


# Native standby admission (browser/standby-renderer.h standby_capacity) fails closed when
# SIGNAGE_STANDBY_CONTEXTS / SIGNAGE_STANDBY_CMA_BYTES are unset: every native-stage is then
# answered "not-ready" and a mixed playlist can never hand off. Ship the board-measured bound so the
# managed OCI is admitted without per-board hand-set environment. Composition of the 4K60 bound
# (additional to the active context): 4K decoder DPB 86 MiB + 3 native UI scanout BOs 98 MiB +
# 36 MiB allocator reserve = 230686720 bytes; menuboard stage needs only the UI/copy pool (64 MiB).
# Live CmaFree is still checked per stage, and any refusal degrades to the serial path in player.js.
STANDBY_CONTEXTS = "2"
STANDBY_CMA_BYTES = "230686720"
STANDBY_CMA_BY_PATH = '{"/native-builtin/menuboard/index.html":67108864,"/player/":230686720}'


def standby_defaults(environ):
    """Return admission variables to add; explicit operator values always win.

    The persistent single-decoder engine (default) replaces the native second-decoder
    standby, so no standby admission is defaulted while it is enabled."""
    if environ.get("SIGNAGE_VIDEO_ENGINE", "1") == "1":
        return {}
    defaults = {"SIGNAGE_STANDBY_CONTEXTS": STANDBY_CONTEXTS,
                "SIGNAGE_STANDBY_CMA_BYTES": STANDBY_CMA_BYTES,
                "SIGNAGE_STANDBY_CMA_BY_PATH": STANDBY_CMA_BY_PATH}
    return {k: v for k, v in defaults.items() if k not in environ}


def engine_environment(environ, app_root=APP_ROOT):
    """Engine variables to add (explicit values win); empty when SIGNAGE_VIDEO_ENGINE=0."""
    if environ.get("SIGNAGE_VIDEO_ENGINE", "1") != "1":
        return {}
    defaults = {"SIGNAGE_VIDEO_ENGINE": "1",
                "SIGNAGE_ENGINE_SOCKET": "/run/admiral-signage/video/engine.sock",
                "SIGNAGE_ENGINE_ASSET_ROOTS": f"{app_root}/media/engine:{environ.get('SIGNAGE_DATA_DIR', '/data')}/engine",
                "SIGNAGE_ENGINE_CACHE_MB": "160"}
    warm = Path(app_root) / "media/engine/promo-a.engine.json"
    if warm.is_file():
        defaults["SIGNAGE_ENGINE_WARMUP"] = str(warm)
    return {k: v for k, v in defaults.items() if k not in environ}


def start_url(environ):
    """Browser start URL: SIGNAGE_URL wins, else SIGNAGE_PAGE picks a bundled page."""
    if environ.get("SIGNAGE_URL"):
        return environ["SIGNAGE_URL"]
    page = environ.get("SIGNAGE_PAGE", "").strip().lower()
    if page not in PAGES:
        raise ValueError(f"SIGNAGE_PAGE must be one of: {', '.join(k for k in PAGES if k)}")
    return f"http://127.0.0.1:{environ.get('SIGNAGE_HTTP_PORT', '8080')}/{PAGES[page]}"


def writable_dir(*candidates):
    for candidate in candidates:
        try:
            candidate.mkdir(parents=True, exist_ok=True)
            probe = candidate / f".write-test-{os.getpid()}"
            probe.touch()
            probe.unlink()
            return candidate
        except OSError:
            continue
    raise OSError("No writable cache directory")


def configure_caches(data):
    """Keep fontconfig and the GStreamer registry in a writable, persistent place.

    An unwritable HOME makes every GStreamer process rescan all plugins (~14 s)
    and fontconfig warn about a missing cache.
    """
    cache = writable_dir(data / "cache", Path("/tmp/admiral-signage-cache"))
    os.environ.setdefault("XDG_CACHE_HOME", str(cache))
    if "GST_REGISTRY" not in os.environ:
        # v4l2codecs discovers factories from devices when the plugin loads, so a
        # registry is only valid for the kernel and V4L2/media nodes that built it.
        nodes = sorted(str(path) for pattern in ("video*", "media*") for path in Path("/dev").glob(pattern))
        key = hashlib.sha1("\n".join([platform.release(), *nodes]).encode()).hexdigest()[:12]
        registry = cache / "gstreamer-1.0" / f"registry-{key}.bin"
        registry.parent.mkdir(parents=True, exist_ok=True)
        for stale in registry.parent.glob("registry-*.bin*"):
            if not stale.name.startswith(registry.name):
                stale.unlink(missing_ok=True)
        os.environ["GST_REGISTRY"] = str(registry)
    return cache


def configure():
    RUNTIME.mkdir(parents=True, exist_ok=True, mode=0o700)
    RUNTIME.chmod(0o700)
    data = Path(os.environ.get("SIGNAGE_DATA_DIR", "/data"))
    for name in ("logs", "media", "www"):
        (data / name).mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("GST_DEBUG_DUMP_DOT_DIR", str(data / "logs"))
    configure_caches(data)
    VIDEO_RUNTIME.mkdir(parents=True, exist_ok=True, mode=0o700)
    VIDEO_RUNTIME.chmod(0o700)
    for name in ("drm-status.json", "browser-status.json"):
        (RUNTIME / name).unlink(missing_ok=True)
    (VIDEO_RUNTIME / "video-status.json").unlink(missing_ok=True)
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
    decoder, ranks = decoder_config(os.environ)
    os.environ.setdefault("SIGNAGE_DECODER", decoder)
    os.environ.setdefault("GST_PLUGIN_FEATURE_RANK", ranks)
    os.environ["SIGNAGE_URL"] = start_url(os.environ)
    os.environ.update(standby_defaults(os.environ))
    os.environ.update(engine_environment(os.environ))
    return data


def wait_for_server(url, alive, timeout=120):
    """Never launch a browser on an unready server; supervision retries the workload."""
    deadline = time.monotonic() + timeout
    while alive() and time.monotonic() < deadline:
        try:
            with urlopen(url, timeout=min(1, max(0.01, deadline - time.monotonic()))) as response:
                if response.status == 200:
                    return True
        except OSError:
            pass
        time.sleep(min(0.1, max(0, deadline - time.monotonic())))
    return False


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
    engine = None
    engine_starts = []

    def shutdown(signum=None, frame=None):
        nonlocal stopping
        stopping = True
        for process in processes + ([engine] if engine is not None else []):
            if process.poll() is None:
                process.terminate()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    processes.append(subprocess.Popen([sys.executable, str(APP_ROOT / "app/server.py")]))
    ready = wait_for_server(
        f"http://127.0.0.1:{os.environ.get('SIGNAGE_HTTP_PORT', '8080')}/healthz",
        lambda: not stopping and processes[0].poll() is None)
    code = 0
    if ready and not stopping and processes[0].poll() is None:
        processes.append(subprocess.Popen(["dbus-run-session", "--", "/usr/local/bin/signage-browser",
                                           os.environ["SIGNAGE_URL"], *args]))
    elif not stopping:
        print("HTTP server readiness failed; refusing browser launch. Admiral should retry workload.", flush=True)
        code = 1
        shutdown()
    def start_engine():
        # One persistent decoder for the whole presentation; it waits for the browser's
        # broker itself. A crashed engine is restarted (bounded) without the browser.
        nonlocal engine
        engine_starts.append(time.monotonic())
        engine = subprocess.Popen(["/usr/local/bin/signage-video-engine"])

    if code == 0 and os.environ.get("SIGNAGE_VIDEO_ENGINE") == "1" and os.path.exists("/usr/local/bin/signage-video-engine"):
        start_engine()
    try:
        while not stopping:
            if engine is not None and engine.poll() is not None and not stopping:
                recent = [t for t in engine_starts if time.monotonic() - t < 60]
                print(f"Video engine exited ({engine.returncode}); restarts in the last minute: {len(recent)}", flush=True)
                if len(recent) >= 5:
                    code = engine.returncode or 1
                    shutdown()
                    break
                time.sleep(1)
                start_engine()
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
        for process in processes + ([engine] if engine is not None else []):
            try:
                process.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    return code if code >= 0 else 128 - code


if __name__ == "__main__":
    sys.exit(main())
