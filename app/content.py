"""Small, persistent content library and scheduled loop. No external dependencies."""
import json
import math
import os
import shutil
import stat
import subprocess
import threading
import time
import uuid
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from urllib.parse import quote

LOCK = threading.RLock()
MAX_UPLOAD = 512 * 1024 * 1024
MAX_UNPACKED = 512 * 1024 * 1024
TYPES = {'.mp4': 'video', '.webm': 'video', '.mov': 'video', '.png': 'image',
         '.jpg': 'image', '.jpeg': 'image', '.webp': 'image', '.gif': 'image',
         '.avif': 'image', '.html': 'html', '.zip': 'html'}
DEMO_4K_ID, DEMO_4K_NAME = 'demo-4k-hevc', 'demo-4k-hevc.mp4'


class ContentError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def video_details(file, url):
    """Prepare stills independently of the board's single live decoder."""
    try:
        result = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
            '-show_entries', 'format=duration', '-of', 'json', str(file)],
            capture_output=True, text=True, check=True, timeout=30)
        duration = float(json.loads(result.stdout)['format']['duration'])
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError()
        details = {'video_duration': duration}
        # Stills keep up to the panel's 4K size and never upscale. The end poster seeks to the
        # container's final second (-sseof -1). -update 1 is an image2 *output* option: before -i,
        # ffmpeg 7 rejects it ("Option update not found") and the library never starts.
        scale = "scale='min(3840,iw)':'min(2160,ih)':force_original_aspect_ratio=decrease"
        for label, seek in (('poster', ['-ss', '0']), ('end_poster', ['-sseof', '-1'])):
            still = file.with_name(file.name + '.' + label + '.jpg')
            subprocess.run(['ffmpeg', '-v', 'error', '-y', *seek, '-i', str(file),
                '-frames:v', '1', '-update', '1', '-vf', scale,
                '-q:v', '2', str(still)], capture_output=True, check=True, timeout=60)
            if not still.is_file() or not still.stat().st_size:
                raise ValueError()
            details[label] = url + '.' + label + '.jpg'
        return details
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
        raise ContentError('The video could not be prepared. Use a playable MP4 (H.264 recommended), WebM or MOV.') from exc



PAGE_DEMOS = [
    {'id': 'menuboard-main', 'name': 'SUBSTATION · Main menu', 'kind': 'html',
     'url': '/content/menuboard/index.html?scene=A&freeze=1', 'size': 0, 'builtin': True},
    {'id': 'menuboard-pip', 'name': 'SUBSTATION · Menu + picture-in-picture video', 'kind': 'html',
     'url': '/content/menuboard/index.html?scene=C', 'size': 0, 'builtin': True},
]


def demo_entries(assets):
    available = {asset['id'] for asset in assets}
    order = [('welcome', 12), ('menuboard-main', 18), ('promo-a', 0),
             ('menuboard-pip', 20), ('promo-b', 0)]
    return [dict(id=uuid.uuid4().hex, asset_id=identity, duration=duration,
                 enabled=True, start=None, end=None)
            for identity, duration in order if identity in available]


def builtin_4k_asset(root, app_root):
    """The bundled 4K HEVC demo clip as a library asset, or None when the image lacks it."""
    source = app_root / 'media' / DEMO_4K_NAME
    if not source.is_file():
        return None
    file = root / 'media' / DEMO_4K_NAME
    file.parent.mkdir(parents=True, exist_ok=True)
    if not file.exists():
        shutil.copy2(source, file)
    url = '/media/' + DEMO_4K_NAME
    return {'id': DEMO_4K_ID, 'name': '4K HEVC demo', 'kind': 'video', 'url': url,
            'size': file.stat().st_size, 'builtin': True, **video_details(file, url)}


def upgrade_demos(state, root=None, app_root=None):
    """Apply each shipped catalog step once; retain intentionally edited published loops."""
    changed = False
    if state.get('demo_catalog', 1) < 2:
        old = [(e['asset_id'], e['duration'], e['enabled'], e['start'], e['end']) for e in state['entries']]
        shipped = [('welcome', 12, True, None, None), ('menuboard', 48, True, None, None),
                   ('promo-a', 0, True, None, None), ('promo-b', 0, True, None, None)]
        known = {a['id'] for a in state['assets']}
        state['assets'].extend(dict(a) for a in PAGE_DEMOS if a['id'] not in known)
        if old == shipped:
            state['entries'] = demo_entries(state['assets'])
        state.update(demo_catalog=2, revision=state['revision']+1, updated_at=time.time())
        changed = True
    if state.get('demo_catalog', 1) < 3:
        # Adds the library asset only; never touches entries, so the loop is left as published.
        # If the image lacks the clip or preparing it fails, stay at catalog 2 and retry next start.
        if root is None or app_root is None:
            return changed
        if DEMO_4K_ID not in {a['id'] for a in state['assets']}:
            try:
                asset = builtin_4k_asset(root, app_root)
            except ContentError:
                return changed
            if not asset:
                return changed
            state['assets'].append(asset)
        state.update(demo_catalog=3, revision=state['revision']+1, updated_at=time.time())
        changed = True
    return changed


def initial_state(root, app_root):
    # The image carries seeds; the playable copies and schedule live on Admiral's /data volume.
    for name in ('welcome', 'menuboard'):
        source = app_root / 'www' / name
        target = root / 'content' / name
        if source.is_dir() and not target.exists():
            shutil.copytree(source, target)
    (root / 'media').mkdir(parents=True, exist_ok=True)

    assets = [{'id': 'welcome', 'name': 'Welcome to Admiral', 'kind': 'html',
               'url': '/content/welcome/index.html', 'size': 0, 'builtin': True},
              {'id': 'menuboard', 'name': 'SUBSTATION · Complete original demo', 'kind': 'html',
               'url': '/content/menuboard/index.html', 'size': 0, 'builtin': True}]
    for name in ('promo-a.mp4', 'promo-b.mp4'):
        source = app_root / 'media' / name
        file = root / 'media' / name
        if source.is_file():
            if not file.exists():
                shutil.copy2(source, file)
            assets.append({'id': name[:-4], 'name': 'SUBSTATION · ' + ('Fresh daily' if 'a.' in name else 'Made your way'),
                           'kind': 'video', 'url': '/media/' + name, 'size': file.stat().st_size, 'builtin': True,
                           **video_details(file, '/media/' + name)})
    four_k = builtin_4k_asset(root, app_root)
    if four_k:
        assets.append(four_k)
    assets.extend(dict(a) for a in PAGE_DEMOS)
    return {'revision': 1, 'demo_catalog': 3, 'assets': assets, 'entries': demo_entries(assets), 'updated_at': time.time()}


def save(root, state):
    root.mkdir(parents=True, exist_ok=True)
    temporary = root / ('.library-' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('w') as stream:
            json.dump(state, stream, ensure_ascii=False, allow_nan=False)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(root / 'library.json')
    finally:
        temporary.unlink(missing_ok=True)


def read(root, app_root):
    with LOCK:
        path = root / 'library.json'
        if not path.exists():
            save(root, initial_state(root, app_root))
        state = json.loads(path.read_text())
        if upgrade_demos(state, root, app_root):
            save(root, state)
        return state


def timestamp(value):
    if value is None or value == '':
        return None
    if not isinstance(value, str):
        raise ContentError('Schedule times must be ISO dates with a timezone.')
    try:
        date = datetime.fromisoformat(value.replace('Z', '+00:00'))
        if date.tzinfo is None:
            raise ValueError()
        return date.astimezone(timezone.utc).isoformat()
    except (ValueError, OverflowError):
        raise ContentError('Schedule times must include a valid date and timezone.') from None


def seconds(value):
    return datetime.fromisoformat(value).timestamp() if value else None


def playlist(state, now=None):
    now = time.time() if now is None else now
    assets = {a['id']: a for a in state['assets']}
    active, boundaries = [], []
    for entry in state['entries']:
        if not entry['enabled']:
            continue
        start, end = seconds(entry['start']), seconds(entry['end'])
        boundaries.extend(t for t in (start, end) if t is not None and t > now)
        if (start is None or start <= now) and (end is None or now < end) and entry['asset_id'] in assets:
            active.append({**entry, 'asset': assets[entry['asset_id']]})
    return {'revision': state['revision'], 'entries': active, 'now': now,
            'next_change': min(boundaries) if boundaries else None}


def update(root, app_root, body):
    with LOCK:
        state = read(root, app_root)
        if not isinstance(body, dict):
            raise ContentError('Expected a playlist object.')
        if body.get('revision') != state['revision']:
            raise ContentError('The library changed in another window. Reload the loop before saving.', 409)
        entries = body.get('entries')
        if not isinstance(entries, list) or len(entries) > 200:
            raise ContentError('A loop can contain up to 200 items.')
        assets = {a['id']: a for a in state['assets']}
        clean, ids = [], set()
        for item in entries:
            if not isinstance(item, dict) or not isinstance(item.get('asset_id'), str) or item['asset_id'] not in assets:
                raise ContentError('Choose an existing library item.')
            identity = item.get('id')
            if not isinstance(identity, str) or not identity or len(identity) > 80 or identity in ids:
                raise ContentError('Each loop item needs a unique ID.')
            ids.add(identity)
            duration = item.get('duration')
            if type(duration) not in (int, float) or not math.isfinite(duration) or not 0 <= duration <= 86400:
                raise ContentError('Duration must be between 1 and 86,400 seconds, or 0 for a full video.')
            if duration == 0 and assets[item['asset_id']]['kind'] != 'video':
                raise ContentError('Images and HTML need a duration of at least one second.')
            if 0 < duration < 1 or type(item.get('enabled')) is not bool:
                raise ContentError('Check the duration and enabled setting.')
            start, end = timestamp(item.get('start')), timestamp(item.get('end'))
            if start and end and seconds(start) >= seconds(end):
                raise ContentError('The end time must be after the start time.')
            clean.append(dict(id=identity, asset_id=item['asset_id'], duration=duration,
                              enabled=item['enabled'], start=start, end=end))
        state.update(entries=clean, revision=state['revision'] + 1, updated_at=time.time())
        save(root, state)
        return state


def unpack(source, target):
    try:
        with zipfile.ZipFile(source) as archive:
            infos = archive.infolist()
            if not infos or len(infos) > 1500 or sum(i.file_size for i in infos) > MAX_UNPACKED:
                raise ContentError('HTML packages are limited to 1,500 files and 512 MB unpacked.')
            paths = set()
            for info in infos:
                path = PurePosixPath(info.filename)
                mode = info.external_attr >> 16
                if (path.is_absolute() or '..' in path.parts or '\\' in info.filename or ':' in info.filename
                        or stat.S_ISLNK(mode) or info.flag_bits & 1 or str(path) in paths):
                    raise ContentError('The ZIP contains unsafe, duplicate or encrypted paths.')
                paths.add(str(path))
                if not info.is_dir() and stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                    raise ContentError('HTML packages must contain regular files.')
            written = 0
            for info in infos:
                dest = target / info.filename
                if info.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                    continue
                dest.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as src, dest.open('wb') as out:
                    while chunk := src.read(65536):
                        written += len(chunk)
                        if written > MAX_UNPACKED:
                            raise ContentError('The unpacked package exceeds 512 MB.')
                        out.write(chunk)
        candidates = list(target.rglob('index.html'))
        if (target / 'index.html').is_file():
            return 'index.html'
        if candidates:
            shortest = min(len(p.relative_to(target).parts) for p in candidates)
            top = [p for p in candidates if len(p.relative_to(target).parts) == shortest]
            if len(top) == 1:
                return top[0].relative_to(target).as_posix()
        raise ContentError('The HTML package needs an index.html entry page.')
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError, EOFError, OSError) as exc:
        raise ContentError('This ZIP could not be unpacked. Check the package and try again.') from exc


def upload(root, app_root, filename, stream, length):
    filename = Path(filename).name
    suffix = Path(filename).suffix.lower()
    if suffix not in TYPES:
        raise ContentError('Upload an MP4, WebM, MOV, image, HTML file or ZIP package.')
    if not 0 < length <= MAX_UPLOAD:
        raise ContentError('Choose a non-empty file up to 512 MB.', 413)
    identity = uuid.uuid4().hex
    folder = root / 'content' / identity
    folder.mkdir(parents=True)
    source = folder / ('upload' + suffix)
    try:
        remaining = length
        with source.open('wb') as out:
            while remaining:
                chunk = stream.read(min(65536, remaining))
                if not chunk:
                    raise ContentError('The upload was interrupted.')
                out.write(chunk)
                remaining -= len(chunk)
        if suffix == '.zip':
            # Extract into a separate directory so a ZIP cannot overwrite its own input.
            package = folder / 'package'
            package.mkdir()
            relative = 'package/' + unpack(source, package)
            source.unlink()
        else:
            relative = source.name
        asset = dict(id=identity, name=filename, kind=TYPES[suffix],
                     url='/content/' + identity + '/' + quote(relative), size=length,
                     builtin=False, created_at=time.time())
        if asset['kind'] == 'video':
            asset.update(video_details(source, asset['url']))
        with LOCK:
            state = read(root, app_root)
            state['assets'].append(asset)
            state.update(revision=state['revision'] + 1, updated_at=time.time())
            save(root, state)
        return asset
    except Exception:
        shutil.rmtree(folder, ignore_errors=True)
        raise


def delete(root, app_root, identity):
    with LOCK:
        state = read(root, app_root)
        asset = next((a for a in state['assets'] if a['id'] == identity), None)
        if asset is None:
            raise ContentError('Content not found.', 404)
        if asset['builtin']:
            raise ContentError('The included demo content stays in your library.')
        if any(e['asset_id'] == identity for e in state['entries']):
            raise ContentError('Remove this content from the loop and publish before deleting it.', 409)
        state['assets'].remove(asset)
        state.update(revision=state['revision'] + 1, updated_at=time.time())
        save(root, state)
        shutil.rmtree(root / 'content' / identity, ignore_errors=True)
        return state
