"""Local diagnostics and static/media server, with byte ranges for HTML video."""
import json
import mimetypes
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

from diagnostics import APP_ROOT, snapshot

DATA_ROOT = Path(os.environ.get("SIGNAGE_DATA_DIR", "/var/lib/admiral-signage"))
PORT = int(os.environ.get("SIGNAGE_HTTP_PORT", "8080"))
STATUS = {"runtime": {}, "video": {}, "holepunch": {"verified": False}}


def configuration():
    return {"video_url": os.environ.get("SIGNAGE_VIDEO_URL", f"http://127.0.0.1:{PORT}/media/test-h264.mp4"),
            "video_mode": os.environ.get("SIGNAGE_VIDEO_MODE", "holepunch"),
            "browser_url": os.environ.get("SIGNAGE_URL", f"http://127.0.0.1:{PORT}/"),
            "quirk": os.environ.get("WEBKIT_GST_HOLE_PUNCH_QUIRK", ""),
            "allow_software": os.environ.get("SIGNAGE_ALLOW_SOFTWARE", "0") == "1"}


def refresh_status():
    global STATUS
    while True:
        try:
            STATUS = snapshot()
            STATUS["updated_at"] = time.time()
        except Exception as exc:
            STATUS = {"error": str(exc)}
        time.sleep(3)


def safe_file(root, relative):
    root = root.resolve()
    candidate = (root / relative).resolve()
    if not candidate.is_relative_to(root) or not candidate.is_file():
        return None
    return candidate


def parse_range(value, size):
    """Return inclusive range, rejecting multipart/unsatisfiable ranges."""
    match = re.fullmatch(r"bytes=(\d*)-(\d*)", value or "")
    if not match or not any(match.groups()) or size <= 0:
        raise ValueError("Invalid range")
    first, last = match.groups()
    if first:
        start, end = int(first), min(int(last), size - 1) if last else size - 1
    else:
        suffix = int(last)
        if suffix <= 0:
            raise ValueError("Empty suffix")
        start, end = max(0, size - suffix), size - 1
    if start >= size or end < start:
        raise ValueError("Unsatisfiable range")
    return start, end


class Handler(BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.do_GET(head=True)

    def json_response(self, value, head=False):
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def do_GET(self, head=False):
        path = unquote(urlsplit(self.path).path)
        if path == "/healthz":
            return self.json_response({"ok": True}, head)
        if path == "/api/config":
            return self.json_response(configuration(), head)
        if path == "/api/status":
            return self.json_response(STATUS, head)
        if path.startswith("/media/"):
            name = path.removeprefix("/media/")
            roots = [DATA_ROOT / "media", APP_ROOT / "media"]
        else:
            name = path.lstrip("/") or "index.html"
            roots = [DATA_ROOT / "www", APP_ROOT / "www"]
        file = next((found for root in roots if (found := safe_file(root, name))), None)
        if file is None:
            return self.send_error(404)
        size = file.stat().st_size
        start, end = 0, size - 1
        ranged = "Range" in self.headers
        if ranged:
            try:
                start, end = parse_range(self.headers["Range"], size)
            except ValueError:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        self.send_response(206 if ranged else 200)
        self.send_header("Content-Type", mimetypes.guess_type(file.name)[0] or "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(max(0, end - start + 1)))
        self.send_header("Cache-Control", "no-store" if file.suffix != ".mp4" else "private, max-age=60")
        if ranged:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if not head:
            try:
                with file.open("rb") as stream:
                    stream.seek(start)
                    remaining = end - start + 1
                    while remaining > 0:
                        chunk = stream.read(min(65536, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def log_message(self, format, *args):
        if os.environ.get("SIGNAGE_HTTP_LOG", "0") == "1":
            super().log_message(format, *args)


def main():
    threading.Thread(target=refresh_status, daemon=True).start()
    host = os.environ.get("SIGNAGE_HTTP_HOST", "127.0.0.1")
    server = ThreadingHTTPServer((host, PORT), Handler)
    server.daemon_threads = True
    print(f"Diagnostics listening on {host}:{PORT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
