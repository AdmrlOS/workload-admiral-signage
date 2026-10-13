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
from ram_cache import RamCache

from diagnostics import APP_ROOT, snapshot
from entrypoint import start_url

DATA_ROOT = Path(os.environ.get("SIGNAGE_DATA_DIR", "/data"))
PORT = int(os.environ.get("SIGNAGE_HTTP_PORT", "8080"))
STATUS = {"runtime": {}, "video": {}, "holepunch": {"verified": False}}
# Conservative compressed-byte budget, separate from decoder/CMA and native UI reserves.
RAM_CACHE = RamCache(int(os.environ.get('SIGNAGE_RAM_CACHE_MB', '256')) * 1024 * 1024,
                     int(os.environ.get('SIGNAGE_RAM_RESERVE_MB', '1024')) * 1024 * 1024)
PRELOADER = None
PRELOAD_ACTIVE = False
PRELOAD_LOCK = threading.Lock()
PRELOAD_PENDING = None


def resolve_asset(url):
    parsed = urlsplit(url)
    if parsed.scheme or parsed.netloc:
        return None
    path = unquote(parsed.path)
    if path.startswith('/content/'):
        roots, name = [DATA_ROOT / 'content'], path[9:]
    elif path.startswith('/media/'):
        roots, name = [DATA_ROOT / 'media', APP_ROOT / 'media'], path[7:]
    else:
        return None
    return next((found for root in roots if (found := safe_file(root, name))), None)


def preload_window(urls):
    """Latest two-item window only, one loader; edits cancel old in-flight loads."""
    global PRELOADER, PRELOAD_PENDING, PRELOAD_ACTIVE
    files = []
    for url in urls:
        file = resolve_asset(url)
        if file is None:
            continue
        files.append(file)
        if urlsplit(url).path.startswith('/content/') and file.suffix.lower() in ('.html', '.htm'):
            # Package dependencies include CSS/fonts/images/nested CSS URLs without parsing HTML.
            package = DATA_ROOT / 'content' / unquote(urlsplit(url).path).split('/')[2]
            for dependency in package.rglob('*'):
                if len(files) >= 3000:
                    break
                if dependency.is_file() and not dependency.is_symlink() and dependency.resolve().is_relative_to(package.resolve()):
                    files.append(dependency.resolve())
    files = list(dict.fromkeys(files))
    with PRELOAD_LOCK:
        epoch = RAM_CACHE.set_window(files)
        PRELOAD_PENDING = (RAM_CACHE, files, epoch)
        if not PRELOAD_ACTIVE:
            PRELOAD_ACTIVE = True
            PRELOADER = threading.Thread(target=preload_worker, daemon=True)
            PRELOADER.start()
    return RAM_CACHE.stats()


def preload_worker():
    global PRELOAD_PENDING, PRELOAD_ACTIVE
    while True:
        with PRELOAD_LOCK:
            if PRELOAD_PENDING is None:
                PRELOAD_ACTIVE = False
                return
            cache, files, epoch = PRELOAD_PENDING
            PRELOAD_PENDING = None
        for file in files:
            if epoch != cache.epoch:
                break
            cache.warm(file, epoch)



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
            if urlsplit(self.path).path == '/api/preload':
                origin = self.headers.get('Origin')
                if (self.command != 'POST' or self.headers.get('X-Signage-Request') != 'player'
                        or self.headers.get('Sec-Fetch-Site') == 'cross-site'
                        or (origin and urlsplit(origin).netloc != self.headers.get('Host'))):
                    raise content.ContentError('Player origin required.', 403)
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 8192 or self.headers.get('Transfer-Encoding'):
                    raise content.ContentError('Invalid preload request.')
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise content.ContentError('Invalid preload request.')
                urls = body.get('urls')
                if not isinstance(urls, list) or len(urls) > 6 or not all(isinstance(url, str) for url in urls):
                    raise content.ContentError('At most active/next assets and posters may be pinned.')
                return self.json_response(preload_window(urls))
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
                if path == '/api/playlist':
                    state = content.playlist(state)
                    for entry in state['entries']:
                        asset = entry['asset']
                        file = resolve_asset(asset['url']) if asset['kind'] == 'html' else None
                        if file and file.stat().st_size <= 1024 * 1024:
                            with file.open('rb') as stream:
                                markup = stream.read(1024 * 1024)
                            asset['preload_static'] = bool(re.search(br'<meta\s+name=[\"\']signage-preload[\"\']\s+content=[\"\']static[\"\']\s*/?>', markup, re.I))
                return self.json_response(state, head)
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
            with RAM_CACHE.acquire(file) as cached:
                html_body = cached[:] if cached is not None else file.read_bytes()
            staging = parse_qs(urlsplit(self.path).query).get('signage_stage') == ['1']
            bridge = (b'<script>window.__signage_stage=true;</script>' if staging else b'') + b'<script src="/player/bridge.js"></script>'
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
        with RAM_CACHE.acquire(file) as cached:
            return self.file_response(file, html_body, cached, head, path, package_html, size, start, end, ranged)

    def file_response(self, file, html_body, cached, head, path, package_html, size, start, end, ranged):
        self.send_response(206 if ranged else 200)
        self.send_header('X-Signage-Cache', 'ram' if cached is not None else 'disk')
        self.send_header("Content-Type", mimetypes.guess_type(file.name)[0] or "application/octet-stream")
        self.send_header("Accept-Ranges", "none" if package_html else "bytes")
        self.send_header('X-Content-Type-Options', 'nosniff')
        if path.startswith('/content/'):
            staging = parse_qs(urlsplit(self.path).query).get('signage_stage') == ['1']
            self.send_header('Content-Security-Policy', "sandbox allow-scripts; connect-src 'none'; form-action 'none'; frame-ancestors 'self'" + ("; media-src 'none'; frame-src 'none'; object-src 'none'" if staging else ''))
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
                if cached is not None:
                    offset = start
                    while offset <= end:
                        chunk = cached[offset:min(offset + 65536, end + 1)]
                        self.wfile.write(chunk)
                        offset += len(chunk)
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
