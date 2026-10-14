#!/usr/bin/env python3
"""Prepare a video for the persistent single-decoder engine.

Output: NAME.hevc (HEVC Annex-B elementary stream) + NAME.engine.json (manifest).
Engine profile: HEVC Main 8-bit 4:2:0, exact WIDTHxHEIGHT, CFR FPS, closed GOPs with an
IDR (and in-band VPS/SPS/PPS) at every GOP start, zero picture reordering. All assets
prepared with the same profile share one SPS (compat_key) so the engine can feed them
back to back into one decoder without renegotiation.

The input is read-only. Outputs are written to temporary files in --out-dir and renamed;
an output that already exists is refused unless --force (which replaces outputs only).
Audio is never invented or silently dropped: a source with audio needs --audio drop.
Only local ffmpeg/ffprobe are used; nothing is uploaded.
"""
import argparse
import hashlib
import json
import os
import resource
import shutil
import subprocess
import sys
import time
from fractions import Fraction
from pathlib import Path

ENGINE_PROTOCOL = 1
IDR_TYPES = (19, 20)
VCL_MAX = 31
FORBIDDEN_TYPES = {6: 'RADL', 7: 'RADL', 8: 'RASL', 9: 'RASL', 16: 'BLA', 17: 'BLA', 18: 'BLA', 21: 'CRA'}


class Incompatible(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def run(cmd, **kwargs):
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, text=True, **kwargs)


def ffprobe_json(path, *extra):
    out = run(['ffprobe', '-v', 'error', '-of', 'json', *extra, path]).stdout
    return json.loads(out)


# ---------------------------------------------------------------- bitstream parsing
def split_nals(data):
    """Yield (offset_of_start_code, payload_start, payload_end) for each Annex-B NAL."""
    starts = []
    i, n = 0, len(data)
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        sc = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append((sc, j + 3))
        i = j + 3
    for k, (sc, begin) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        while end > begin and data[end - 1] == 0 and k + 1 < len(starts):
            end -= 1
        yield sc, begin, end


def rbsp(payload):
    out = bytearray()
    zeros = 0
    for b in payload:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


class Bits:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def u(self, n):
        value = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3] if (self.pos >> 3) < len(self.data) else 0
            value = (value << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return value

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 31:
                raise Incompatible('incompatible: corrupt Exp-Golomb code in SPS')
        return (1 << zeros) - 1 + self.u(zeros)


def parse_sps(payload):
    bits = Bits(rbsp(payload)[2:])
    bits.u(4)
    max_sub_layers = bits.u(3)
    bits.u(1)
    bits.u(2)
    bits.u(1)
    profile_idc = bits.u(5)
    bits.u(32)
    bits.u(4)
    bits.u(43)
    bits.u(1)
    level_idc = bits.u(8)
    profile_present, level_present = [], []
    for _ in range(max_sub_layers):
        profile_present.append(bits.u(1))
        level_present.append(bits.u(1))
    if max_sub_layers:
        for _ in range(max_sub_layers, 8):
            bits.u(2)
    for i in range(max_sub_layers):
        if profile_present[i]:
            bits.u(88)
        if level_present[i]:
            bits.u(8)
    bits.ue()
    chroma = bits.ue()
    if chroma == 3:
        bits.u(1)
    width, height = bits.ue(), bits.ue()
    if bits.u(1):
        sub_w = 2 if chroma in (1, 2) else 1
        sub_h = 2 if chroma == 1 else 1
        left, right, top, bottom = bits.ue(), bits.ue(), bits.ue(), bits.ue()
        width -= sub_w * (left + right)
        height -= sub_h * (top + bottom)
    depth_luma, depth_chroma = bits.ue() + 8, bits.ue() + 8
    bits.ue()
    ordering_present = bits.u(1)
    reorder = latency = dpb = 0
    for _ in range(0 if ordering_present else max_sub_layers, max_sub_layers + 1):
        dpb = bits.ue() + 1
        reorder = bits.ue()
        latency = bits.ue()
    return {'profile_idc': profile_idc, 'level_idc': level_idc, 'chroma_format_idc': chroma,
            'width': width, 'height': height, 'bit_depth': depth_luma, 'bit_depth_chroma': depth_chroma,
            'max_dec_pic_buffering': dpb, 'max_num_reorder': reorder, 'max_latency_increase_plus1': latency,
            'max_sub_layers': max_sub_layers + 1}


def first_slice(payload):
    return len(payload) > 2 and (payload[2] & 0x80) != 0


def analyse_es(data):
    """Group NALs into access units and check the engine stream structure."""
    units, current = [], None
    sps_set, pps_set = set(), set()
    sps_info = None
    for start, begin, end in split_nals(data):
        payload = data[begin:end]
        if len(payload) < 2:
            continue
        nal_type = (payload[0] >> 1) & 0x3F
        if nal_type in FORBIDDEN_TYPES:
            raise Incompatible(f'incompatible: {FORBIDDEN_TYPES[nal_type]} NAL (type {nal_type}) found; '
                               'engine assets need closed GOPs with IDR starts and no reordered leading pictures')
        starts_au = (nal_type in (32, 33, 34, 35, 39) and (current is None or current['vcl'])) or \
                    (nal_type <= VCL_MAX and first_slice(payload) and (current is None or current['vcl']))
        if starts_au:
            if current is not None:
                current['size'] = start - current['offset']
                units.append(current)
            current = {'offset': start, 'vcl': False, 'key': False, 'sps': False}
        if current is None:
            raise Incompatible('incompatible: stream does not start with an access unit')
        if nal_type == 33:
            current['sps'] = True
            sps_set.add(hashlib.sha256(rbsp(payload)).hexdigest())
            sps_info = parse_sps(payload)
        elif nal_type == 34:
            pps_set.add(hashlib.sha256(rbsp(payload)).hexdigest())
        elif nal_type <= VCL_MAX:
            current['vcl'] = True
            if nal_type in IDR_TYPES:
                current['key'] = True
    if current is not None:
        current['size'] = len(data) - current['offset']
        units.append(current)
    if not units or not units[0]['key']:
        raise Incompatible('incompatible: first access unit is not an IDR')
    if not sps_info:
        raise Incompatible('incompatible: no SPS found')
    if len(sps_set) != 1:
        raise Incompatible(f'incompatible: {len(sps_set)} different SPS in one asset')
    for index, unit in enumerate(units):
        if not unit['vcl']:
            raise Incompatible(f'incompatible: access unit {index} has no picture')
        if unit['key'] and not unit['sps']:
            raise Incompatible(f'incompatible: IDR at access unit {index} lacks in-band VPS/SPS/PPS '
                               '(encode with repeat-headers so every GOP is self-contained)')
    return units, sps_info, sps_set.pop(), sorted(pps_set)


def validate(es_path, args):
    data = Path(es_path).read_bytes()
    units, sps, sps_hash, pps_hashes = analyse_es(data)
    problems = []
    if sps['profile_idc'] != 1:
        problems.append(f"profile_idc {sps['profile_idc']} (need 1 = Main)")
    if sps['bit_depth'] != 8 or sps['bit_depth_chroma'] != 8:
        problems.append(f"bit depth {sps['bit_depth']}/{sps['bit_depth_chroma']} (need 8)")
    if sps['chroma_format_idc'] != 1:
        problems.append(f"chroma_format_idc {sps['chroma_format_idc']} (need 4:2:0)")
    if (sps['width'], sps['height']) != (args.width, args.height):
        problems.append(f"size {sps['width']}x{sps['height']} (need {args.width}x{args.height})")
    if sps['max_num_reorder'] != 0:
        problems.append(f"sps_max_num_reorder_pics {sps['max_num_reorder']} (need 0: no B-frame reordering)")
    probe = ffprobe_json(es_path, '-count_frames', '-select_streams', 'v:0', '-show_entries',
                         'stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_frames,'
                         'color_range,color_space,color_transfer,color_primaries')
    stream = probe['streams'][0]
    rate = Fraction(stream.get('r_frame_rate', '0/1'))
    if rate != Fraction(args.fps, 1):
        problems.append(f"fps {stream.get('r_frame_rate')} (need {args.fps}/1 constant frame rate in the VUI timing)")
    if stream.get('pix_fmt') != 'yuv420p':
        problems.append(f"pix_fmt {stream.get('pix_fmt')} (need yuv420p)")
    decoded = int(stream.get('nb_read_frames') or 0)
    if decoded != len(units):
        problems.append(f'ffprobe decoded {decoded} frames but the stream has {len(units)} access units')
    colour = {k: stream.get(k, 'unknown') for k in ('color_range', 'color_space', 'color_transfer', 'color_primaries')}
    if colour['color_range'] not in ('tv', 'unknown') or colour['color_space'] not in ('bt709', 'unknown'):
        problems.append(f'colour {colour} (need bt709 limited range)')
    keys = [i for i, u in enumerate(units) if u['key']]
    gop = keys[1] - keys[0] if len(keys) > 1 else len(units)
    if any(b - a != gop for a, b in zip(keys, keys[1:])) or keys[-1] + gop < len(units):
        problems.append(f'irregular GOP structure {keys[:8]}...')
    if problems:
        raise Incompatible('incompatible: ' + '; '.join(problems))
    fps_n, fps_d = args.fps, 1
    access_units = [{'offset': u['offset'], 'size': u['size'], 'key': u['key'],
                     'pts': i * 1_000_000_000 * fps_d // fps_n} for i, u in enumerate(units)]
    return {
        'codec': 'hevc', 'profile': 'Main', 'bit_depth': 8, 'chroma': '4:2:0',
        'width': sps['width'], 'height': sps['height'], 'level_idc': sps['level_idc'],
        'fps_n': fps_n, 'fps_d': fps_d, 'frames': len(units),
        'duration_ns': len(units) * 1_000_000_000 * fps_d // fps_n,
        'max_num_reorder': sps['max_num_reorder'], 'max_dec_pic_buffering': sps['max_dec_pic_buffering'],
        'gop_frames': gop, 'gops': keys, 'compat_key': sps_hash, 'pps_sha256': pps_hashes,
        'colour': colour, 'es_size': len(data), 'es_sha256': hashlib.sha256(data).hexdigest(),
        'access_units': access_units,
    }


# ---------------------------------------------------------------- preparation
def x265_params(args):
    return ':'.join([
        f'keyint={args.fps}', f'min-keyint={args.fps}', 'scenecut=0', 'open-gop=0', 'bframes=0',
        'repeat-headers=1', 'info=0', 'aud=0', 'hrd=0', f'level-idc={args.level}',
        f'vbv-maxrate={args.max_kbps}', f'vbv-bufsize={args.max_kbps}',
        'colorprim=bt709', 'transfer=bt709', 'colormatrix=bt709', 'range=limited', 'log-level=error',
    ])


def transcode(source, target, args):
    vf = (f'scale={args.width}:{args.height}:force_original_aspect_ratio=decrease:flags=lanczos'
          f':out_color_matrix=bt709:out_range=tv,'
          f'pad={args.width}:{args.height}:(ow-iw)/2:(oh-ih)/2:color=black,'
          f'fps={args.fps},format=yuv420p,setsar=1')
    run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-i', source, '-map', '0:v:0', '-an', '-sn', '-dn',
         '-vf', vf, '-c:v', 'libx265', '-preset', args.preset, '-crf', str(args.crf), '-profile:v', 'main',
         '-pix_fmt', 'yuv420p', '-x265-params', x265_params(args), '-f', 'hevc', target])


def remux(source, target):
    run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-i', source, '-map', '0:v:0', '-an', '-sn', '-dn',
         '-c:v', 'copy', '-bsf:v', 'hevc_mp4toannexb', '-f', 'hevc', target])


def expected_frames(probe, fps):
    stream = next(s for s in probe['streams'] if s.get('codec_type') == 'video')
    duration = stream.get('duration') or probe.get('format', {}).get('duration')
    return round(Fraction(duration) * fps) if duration else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', nargs='?')
    parser.add_argument('--out-dir')
    parser.add_argument('--name')
    parser.add_argument('--mode', choices=('auto', 'transcode', 'remux'), default='auto')
    parser.add_argument('--audio', choices=('error', 'drop'), default='error')
    parser.add_argument('--width', type=int, default=3840)
    parser.add_argument('--height', type=int, default=2160)
    parser.add_argument('--fps', type=int, default=60)
    parser.add_argument('--level', default='5.1')
    parser.add_argument('--crf', type=int, default=20)
    parser.add_argument('--max-kbps', type=int, default=40000)
    parser.add_argument('--preset', default='medium')
    parser.add_argument('--reference', action='store_true', help='write NAME.framemd5 from an ffmpeg decode')
    parser.add_argument('--force', action='store_true', help='replace existing outputs (never the input)')
    parser.add_argument('--validate-only', metavar='ES')
    args = parser.parse_args(argv)
    try:
        if args.validate_only:
            result = validate(args.validate_only, args)
            result.pop('access_units')
            print(json.dumps(result, indent=2))
            return 0
        if not args.input or not args.out_dir:
            parser.error('INPUT and --out-dir are required')
        source = Path(args.input).resolve(strict=True)
        out_dir = Path(args.out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        name = args.name or source.stem
        es_path = (out_dir / f'{name}.hevc').resolve()
        manifest_path = (out_dir / f'{name}.engine.json').resolve()
        ref_path = (out_dir / f'{name}.framemd5').resolve()
        outputs = [es_path, manifest_path] + ([ref_path] if args.reference else [])
        if source in outputs:
            raise SystemExit('refusing: an output path is the input file; originals are never overwritten')
        existing = [str(p) for p in outputs if p.exists()]
        if existing and not args.force:
            raise SystemExit(f'refusing: output exists ({", ".join(existing)}); pass --force to replace outputs')
        started, usage0 = time.monotonic(), resource.getrusage(resource.RUSAGE_CHILDREN)
        source_hash = sha256_file(source)
        probe = ffprobe_json(source, '-show_streams', '-show_format')
        has_audio = any(s.get('codec_type') == 'audio' for s in probe['streams'])
        if has_audio and args.audio != 'drop':
            raise SystemExit('refusing: source has audio; the engine plays video only. '
                             'Pass --audio drop to discard it explicitly.')
        video = next((s for s in probe['streams'] if s.get('codec_type') == 'video'), None)
        if not video:
            raise SystemExit('refusing: source has no video stream')
        tmp = es_path.with_name(f'.{name}.{os.getpid()}.hevc.tmp')
        mode = args.mode
        try:
            if mode in ('auto', 'remux') and video.get('codec_name') == 'hevc':
                remux(source, tmp)
                try:
                    info = validate(tmp, args)
                    mode = 'remux'
                except Incompatible:
                    if mode == 'remux':
                        raise
                    mode = 'transcode'
            elif mode == 'remux':
                raise Incompatible(f"incompatible: remux needs HEVC input, got {video.get('codec_name')}")
            else:
                mode = 'transcode'
            if mode == 'transcode':
                transcode(source, tmp, args)
                info = validate(tmp, args)
                want = expected_frames(probe, args.fps)
                if want is not None and want != info['frames']:
                    raise Incompatible(f"incompatible: transcode produced {info['frames']} frames, expected {want}")
            if args.reference:
                ref_tmp = ref_path.with_name(f'.{name}.{os.getpid()}.framemd5.tmp')
                run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-i', tmp, '-map', '0:v:0',
                     '-pix_fmt', 'yuv420p', '-f', 'framemd5', ref_tmp])
                info['reference_framemd5_sha256'] = sha256_file(ref_tmp)
                os.replace(ref_tmp, ref_path)
            usage1 = resource.getrusage(resource.RUSAGE_CHILDREN)
            manifest = {
                'engine_protocol': ENGINE_PROTOCOL, 'name': name, 'es': es_path.name,
                **{k: v for k, v in info.items() if k != 'access_units'},
                'audio': 'dropped' if has_audio else 'none',
                'source': source.name, 'source_sha256': source_hash, 'source_size': source.stat().st_size,
                'source_codec': video.get('codec_name'),
                'source_size_px': f"{video.get('width')}x{video.get('height')}",
                'source_rate': video.get('r_frame_rate'),
                'prepare': {'mode': mode, 'crf': args.crf if mode == 'transcode' else None,
                            'preset': args.preset if mode == 'transcode' else None,
                            'x265_params': x265_params(args) if mode == 'transcode' else None,
                            'ffmpeg': run(['ffmpeg', '-version']).stdout.splitlines()[0],
                            'wall_seconds': round(time.monotonic() - started, 2),
                            'cpu_seconds': round((usage1.ru_utime - usage0.ru_utime) + (usage1.ru_stime - usage0.ru_stime), 2)},
                'access_units': info['access_units'],
            }
            os.replace(tmp, es_path)
            manifest_tmp = manifest_path.with_name(f'.{name}.{os.getpid()}.json.tmp')
            manifest_tmp.write_text(json.dumps(manifest, separators=(',', ':')) + '\n')
            os.replace(manifest_tmp, manifest_path)
        finally:
            tmp.unlink(missing_ok=True)
        summary = {k: manifest[k] for k in ('name', 'frames', 'width', 'height', 'fps_n', 'es_size', 'es_sha256',
                                            'compat_key', 'audio', 'source_sha256')}
        summary['prepare'] = manifest['prepare']
        print(json.dumps(summary, indent=2))
        return 0
    except Incompatible as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as exc:
        print(f'ffmpeg failed: {exc.stderr.strip()[-2000:]}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
        sys.exit('ffmpeg and ffprobe are required')
    sys.exit(main())
