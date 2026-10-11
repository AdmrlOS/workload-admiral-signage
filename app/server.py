"""Local diagnostics and static/media server, with byte ranges for HTML video."""
import json
import mimetypes
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit

import content

from diagnostics import APP_ROOT, snapshot
from entrypoint import start_url

DATA_ROOT = Path(os.environ.get("SIGNAGE_DATA_DIR", "/data"))
PORT = int(os.environ.get("SIGNAGE_HTTP_PORT", "8080"))
STATUS = {"runtime": {}, "video": {}, "holepunch": {"verified": False}}


def configuration():
    return {"video_url": os.environ.get("SIGNAGE_VIDEO_URL", f"http://127.0.0.1:{PORT}/media/test-h264.mp4"),
            "video_mode": os.environ.get("SIGNAGE_VIDEO_MODE", "holepunch"),
            "browser_url": start_url(os.environ),
            "player_url": f"http://127.0.0.1:{PORT}/player/",
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

    def json_response(self, value, head=False, status=200):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def mutation(self):
        # Browser requests must originate from the studio, not an uploaded HTML package.
        origin = self.headers.get('Origin')
        if (self.headers.get('X-Signage-Request') != 'studio'
                or self.headers.get('Sec-Fetch-Site') == 'cross-site'
                or (origin and (urlsplit(origin).scheme not in ('http', 'https')
                               or urlsplit(origin).netloc != self.headers.get('Host')))):
            raise content.ContentError('Use the Admiral studio to change content.', 403)
        if self.headers.get('Transfer-Encoding'):
            raise content.ContentError('A Content-Length is required.', 411)
        try:
            length = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            raise content.ContentError('Invalid Content-Length.') from None
        path = urlsplit(self.path).path
        if self.command == 'POST' and path == '/api/assets':
            name = parse_qs(urlsplit(self.path).query).get('name', [''])[0]
            return content.upload(DATA_ROOT, APP_ROOT, name, self.rfile, length)
        if self.command == 'PUT' and path == '/api/playlist':
            if not 0 < length <= 256 * 1024:
                raise content.ContentError('Playlist is too large or empty.', 413)
            try:
                body = json.loads(self.rfile.read(length))
            except (ValueError, UnicodeError):
                raise content.ContentError('Invalid playlist JSON.') from None
            return content.update(DATA_ROOT, APP_ROOT, body)
        if self.command == 'DELETE' and path.startswith('/api/assets/'):
            return content.delete(DATA_ROOT, APP_ROOT, path.rsplit('/', 1)[-1])
        raise content.ContentError('Endpoint not found.', 404)

    def do_POST(self):
        self.connection.settimeout(120)
        try:
            self.json_response(self.mutation())
        except content.ContentError as exc:
            self.json_response({'error': str(exc)}, status=exc.status)
        except (OSError, ValueError):
            self.json_response({'error': 'Content could not be saved. Check the data volume and try again.'}, status=500)
        finally:
            self.close_connection = True

    do_PUT = do_POST
    do_DELETE = do_POST

    def do_GET(self, head=False):
        path = unquote(urlsplit(self.path).path)
        if path in ('/studio', '/player'):
            self.send_response(302)
            self.send_header('Location', path + '/')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path in ('/api/library', '/api/playlist'):
            try:
                state = content.read(DATA_ROOT, APP_ROOT)
                return self.json_response(content.playlist(state) if path == '/api/playlist' else state, head)
            except (OSError, ValueError, content.ContentError):
                return self.json_response({'error': 'The content library could not be opened. Check the data volume.'}, head, status=500)
        if path == "/healthz":
            return self.json_response({"ok": True}, head)
        if path == "/api/config":
            return self.json_response(configuration(), head)
        if path == "/api/status":
            return self.json_response(STATUS, head)
        if path.startswith('/content/'):
            name = path.removeprefix('/content/')
            roots = [DATA_ROOT / 'content']
        elif path.startswith("/media/"):
            name = path.removeprefix("/media/")
            roots = [DATA_ROOT / "media", APP_ROOT / "media"]
        else:
            name = path.lstrip("/")
            if not name or name.endswith("/"):
                name += "index.html"
            roots = [DATA_ROOT / "www", APP_ROOT / "www"]
        file = next((found for root in roots if (found := safe_file(root, name))), None)
        if file is None:
            return self.send_error(404)
        # Inject the playback handshake into packages without modifying their files.
        # Opaque-origin sandboxing prevents packages from accessing the studio API.
        package_html = path.startswith('/content/') and file.suffix.lower() in ('.html', '.htm')
        html_body = None
        if package_html:
            html_body = file.read_bytes()
            bridge = b'<script src="/player/bridge.js"></script>'
            match = re.search(br'<head(?:\s[^>]*)?>', html_body, re.IGNORECASE)
            at = match.end() if match else 0
            html_body = html_body[:at] + bridge + html_body[at:]
        size = len(html_body) if html_body is not None else file.stat().st_size
        start, end = 0, size - 1
        ranged = "Range" in self.headers and html_body is None
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
        self.send_header("Accept-Ranges", "none" if package_html else "bytes")
        self.send_header('X-Content-Type-Options', 'nosniff')
        if path.startswith('/content/'):
            self.send_header('Content-Security-Policy', "sandbox allow-scripts; connect-src 'none'; form-action 'none'; frame-ancestors 'self'")
        self.send_header("Content-Length", str(max(0, end - start + 1)))
        self.send_header("Cache-Control", "private, max-age=3600" if path.startswith(("/media/", "/content/")) and not package_html else "no-store")
        if ranged:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if not head:
            try:
                if html_body is not None:
                    self.wfile.write(html_body)
                    return
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
    content.read(DATA_ROOT, APP_ROOT)
    threading.Thread(target=refresh_status, daemon=True).start()
    host = os.environ.get("SIGNAGE_HTTP_HOST", "127.0.0.1")
    server = ThreadingHTTPServer((host, PORT), Handler)
    server.daemon_threads = True
    print(f"Admiral Signage listening on {host}:{PORT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
