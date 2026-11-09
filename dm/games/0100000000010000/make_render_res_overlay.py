#!/usr/bin/env python3
"""Build a RomFS overlay that scales Super Mario Odyssey's docked render resolution table.

Super Mario Odyssey picks its docked scene size from SystemData/ProjectGraphicsQuality.szs (ProjectGraphicsQuality.byml,
"ProjectGraphicsQualityParamArray"): the "Console" entry of ParamType 0 is the largest size, 1600x900, and its dynamic
resolution ranks 1..5 are 1440x810 .. 800x450; "SnapShot" is 1600x900. Several passes size their buffers for the
largest step, while the light pre-pass draws at the current step: raising only the scene size in code leaves those
buffers at 1600x900 and draws light in blocks on the ground.

Usage:
    python3 dm/games/0100000000010000/make_render_res_overlay.py <romfs_dir_or_img> <out_dir> <width>x<height>
"""
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import romfs_image

NAME = 'SystemData/ProjectGraphicsQuality.szs'
MEMBER = 'ProjectGraphicsQuality.byml'
SHIPPED = (1600, 900)


def yaz0_decode(s):
    assert s[:4] == b'Yaz0', 'not a Yaz0 stream'
    uncomp_size = struct.unpack_from('>I', s, 4)[0]
    out = bytearray()
    p, valid, code = 16, 0, 0
    while len(out) < uncomp_size:
        if valid == 0:
            code, p, valid = s[p], p + 1, 8
        if code & 0x80:
            out.append(s[p]); p += 1
        else:
            b1, b2 = s[p], s[p + 1]; p += 2
            dist = ((b1 & 0x0f) << 8) | b2
            n = (b1 >> 4) + 2 if (b1 >> 4) else s[p] + 0x12
            if (b1 >> 4) == 0:
                p += 1
            src = len(out) - dist - 1
            for _ in range(n):
                out.append(out[src]); src += 1
        code = (code << 1) & 0xff
        valid -= 1
    return bytes(out)


def yaz0_store(data):
    """Yaz0 with every byte a literal: a flag byte 0xff before each 8 bytes."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8))
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append((0xff00 >> len(chunk)) & 0xff if len(chunk) < 8 else 0xff)
        out += chunk
    return bytes(out)


def sarc_member(data, name):
    """(offset, size) of a member in a little-endian SARC."""
    assert data[:4] == b'SARC' and struct.unpack_from('<H', data, 6)[0] == 0xfeff, 'not a little-endian SARC'
    data_off = struct.unpack_from('<I', data, 0x0c)[0]
    sfat = struct.unpack_from('<H', data, 4)[0]
    count = struct.unpack_from('<H', data, sfat + 6)[0]
    sfnt = sfat + 12 + 16 * count
    names = sfnt + 8
    for k in range(count):
        attr, start, end = struct.unpack_from('<III', data, sfat + 12 + 16 * k + 4)
        if attr >> 24 != 1:
            continue
        noff = names + (attr & 0xffff) * 4
        if data[noff:data.index(b'\0', noff)].decode() == name:
            return data_off + start, end - start
    raise KeyError(name)


def quality_entries(b):
    """[(entry dict of name -> (type, value, offset of the value))] of the byml's ProjectGraphicsQualityParamArray."""
    assert b[:2] == b'YB', 'not a little-endian byml'
    key_off, str_off, root_off = struct.unpack_from('<III', b, 4)

    def strtab(off):
        n = struct.unpack_from('<I', b, off)[0] >> 8
        offs = struct.unpack_from(f'<{n + 1}I', b, off + 4)
        return [b[off + o:b.index(b'\0', off + o)].decode() for o in offs[:n]]

    keys, strs = strtab(key_off), strtab(str_off)

    def dict_entries(off):
        assert b[off] == 0xc1
        n = struct.unpack_from('<I', b, off)[0] >> 8
        out = {}
        for i in range(n):
            e = off + 4 + 8 * i
            k = struct.unpack_from('<I', b, e)[0] & 0xffffff
            out[keys[k]] = (b[e + 3], struct.unpack_from('<I', b, e + 4)[0], e + 4)
        return out

    root = dict_entries(root_off)
    t, arr, _ = root['ProjectGraphicsQualityParamArray']
    assert t == 0xc0 and b[arr] == 0xc0
    n = struct.unpack_from('<I', b, arr)[0] >> 8
    base = arr + 4 + ((n + 3) & ~3)
    entries = []
    for i in range(n):
        assert b[arr + 4 + i] == 0xc1
        e = dict_entries(struct.unpack_from('<I', b, base + 4 * i)[0])
        e['Name'] = (0xa0, strs[e['Name'][1]], e['Name'][2])
        entries.append(e)
    return entries


def even(x):
    return int(round(x / 2)) * 2


def scaled_table(byml, w, h):
    """The byml with Console and SnapShot sizes scaled to w x h; returns (bytes, [(name, rank, old, new)])."""
    b = bytearray(byml)
    changes = []
    for e in quality_entries(byml):
        name, rank = e['Name'][1], e['Rank'][1]
        if name not in ('Console', 'SnapShot'):
            continue
        (tw, ow, pw), (th, oh, ph) = e['ScreenWidthScale'], e['ScreenHeightScale']
        assert tw == 0xd1 and th == 0xd1, 'ScreenWidthScale / ScreenHeightScale are not integers'
        nw, nh = even(ow * w / SHIPPED[0]), even(oh * h / SHIPPED[1])
        struct.pack_into('<I', b, pw, nw); struct.pack_into('<I', b, ph, nh)
        changes.append((name, rank, (ow, oh), (nw, nh)))
    top = [c for c in changes if c[0] == 'Console' and c[2] == SHIPPED]
    assert len(top) == 1 and top[0][3] == (w, h), f'expected one Console entry of {SHIPPED[0]}x{SHIPPED[1]}, found {top}'
    return bytes(b), changes


def load_orig_szs(romfs_src: Path) -> bytes:
    if romfs_src.is_dir():
        return (romfs_src / NAME).read_bytes()
    idx = romfs_image.read_index(str(romfs_src))
    if NAME not in idx or idx[NAME][0]:
        raise FileNotFoundError(f'{NAME} not found in RomFS image {romfs_src}')
    _, off, size = idx[NAME]
    with open(romfs_src, 'rb') as f:
        f.seek(off)
        return f.read(size)


def main() -> int:
    if len(sys.argv) != 4 or 'x' not in sys.argv[3]:
        sys.exit(f'usage: {sys.argv[0]} <romfs_dir_or_img> <out_dir> <width>x<height>')
    romfs_src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    w, h = (int(x) for x in sys.argv[3].lower().split('x', 1))
    assert 640 <= w <= 3840 and 360 <= h <= 2160 and w % 2 == 0 and h % 2 == 0, 'size must be even, 640x360 .. 3840x2160'

    original = load_orig_szs(romfs_src)
    sarc = bytearray(yaz0_decode(original))
    off, size = sarc_member(bytes(sarc), MEMBER)
    table, changes = scaled_table(bytes(sarc[off:off + size]), w, h)
    sarc[off:off + size] = table
    patched = yaz0_store(bytes(sarc))
    assert yaz0_decode(patched) == bytes(sarc), 'Yaz0 round trip failed'

    (out / Path(NAME).parent).mkdir(parents=True, exist_ok=True)
    (out / NAME).write_bytes(patched)
    sha = lambda d: hashlib.sha256(d).hexdigest()
    (out / 'manifest.tsv').write_text(f'{NAME}\t{len(patched)}\t{sha(patched)}\t{sha(original)}\n')
    (out / 'render-res.txt').write_text(f'{w}x{h}\n')
    for name, rank, old, new in changes:
        print(f'{name:9s} rank {rank}: {old[0]}x{old[1]} -> {new[0]}x{new[1]}')
    print(f'wrote {out / NAME} ({len(patched)} bytes) and {out / "manifest.tsv"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
