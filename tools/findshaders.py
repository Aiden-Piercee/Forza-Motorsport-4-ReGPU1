"""Finds shader microcode in the game data by its structure (no packet header needed).

findshaders.py <game dir> <out dir> [real shader dir]

Shaders sit raw inside the .rmb.bin mesh files (in the track/car zips), 16-byte
aligned. A candidate is accepted when its control flow decodes cleanly: valid
opcodes up to an end flag, the first exec address equal to the control-flow length,
and exec ranges that tile the instruction slots without gaps. The shader is the
words up to the last used slot; the loaded copy carries a per-load stamp after
that, so the renderer hashes the same used length.

Writes <vs|ps>_<hash>.ucode per distinct shader. With a directory of shaders the
game actually drew, reports how many of them were found.
"""
import glob
import os
import struct
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = 0xFFFFFFFFFFFFFFFF
EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}
END_OPS = {2, 4, 6, 14}
VALID_OPS = set(range(16))
MAX_SLOTS = 512


def fnv1a(b):
    h = FNV_OFFSET
    for x in b:
        h = ((h ^ x) * FNV_PRIME) & MASK
    return h


def used_dwords(words, start, limit):
    """Length in dwords of the shader at words[start], or 0 if it isn't one."""
    cfs = []
    triple = 0
    while True:
        if triple > 64 or start + triple * 3 + 2 >= limit:
            return 0, None
        w0, w1, w2 = (int(x) for x in words[start + triple * 3:start + triple * 3 + 3])
        pair = ((w0, w1 & 0xFFFF), ((w1 >> 16) | ((w2 << 16) & 0xFFFFFFFF), w2 >> 16))
        triple += 1
        ended = False
        for lo, hi in pair:
            op = (hi >> 12) & 0xF
            cfs.append((op, lo, hi))
            if op in END_OPS:
                ended = True
                break
        if ended:
            break
    execs = [(lo & 0xFFF, (lo >> 12) & 7, lo >> 16 & 0xFFF) for op, lo, hi in cfs if op in EXEC_OPS]
    execs = [e for e in execs if e[1]]
    if not execs:
        return 0, None
    # Instructions start right after the control flow and the exec ranges tile them.
    if min(a for a, _, _ in execs) != triple:
        return 0, None
    slot = triple
    for a, c, _ in sorted(execs):
        if a != slot:
            return 0, None
        slot = a + c
    if slot > MAX_SLOTS or start + slot * 3 > limit:
        return 0, None
    # Unused bits that are zero in real control flow.
    for op, lo, hi in cfs:
        if op == 12 and (lo & 0xFFFFFFF8 or hi & 0x09FF):
            return 0, None
        if op in (1, 2) and hi & 0x0FFF and (hi & 0x0FFF) != 0x0200:
            pass
    vertex = False
    for a, c, seq in execs:
        for i in range(c):
            if (seq >> (2 * i)) & 1:
                fw0 = int(words[start + (a + i) * 3])
                if (fw0 & 0x1F) == 0:
                    vertex = True
    pixel = not vertex and not any(op == 12 and ((hi >> 9) & 3) == 1 for op, lo, hi in cfs)
    return slot * 3, pixel


def scan(data, step=4):
    # step: words between candidates (FM2's meshes align shaders to 16 bytes, FM4's .fxobj to 4).
    out = []
    buf = data[:len(data) // 4 * 4]
    if len(buf) < 48:
        return out
    words = np.frombuffer(buf, dtype='>u4')
    n = len(words)
    idx = np.arange(0, n - 3, step)
    w0 = words[idx]
    w1 = words[idx + 1]
    op0 = (w1 >> 12) & 0xF
    addr0 = w0 & 0xFFF
    # First control-flow instruction: an exec whose address is the control-flow length, or an alloc.
    keep = ((np.isin(op0, list(EXEC_OPS)) & (addr0 >= 1) & (addr0 <= 64)) | ((op0 == 12) & ((w0 & 0xFFFFFFF8) == 0)))
    for i in idx[keep]:
        length, pixel = used_dwords(words, int(i), n)
        if length:
            code = buf[i * 4:(i + length) * 4]
            out.append((pixel, code))
    return out


def scan_zip(path):
    found = {}
    try:
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                # Other members (.carbin, .bix, .xgf, ...) only add false positives.
                if '.rmb' in info.filename.lower() and info.file_size:
                    for pixel, code in scan(z.read(info)):
                        found[(pixel, code)] = None
    except Exception as e:
        print('skip', path, e, file=sys.stderr)
    return list(found)


def scan_file(path):
    # Loose files: FM4's .fxobj effects (zipped ones dumped by tools/fxobjdump) and executable images.
    try:
        return list({k: None for k in scan(open(path, 'rb').read(), 1)})
    except Exception as e:
        print('skip', path, e, file=sys.stderr)
        return []


def normalized_hash(code, pixel):
    """The renderer's shader hash over the used words of a loaded shader file."""
    words = np.frombuffer(code[:len(code) // 4 * 4], dtype='>u4')
    length, _ = used_dwords(words, 0, len(words))
    if not length:
        return None
    return fnv1a(code[:length * 4]) ^ (1 if pixel else 0)


def main():
    game, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    zips = glob.glob(os.path.join(game, '**', '*.zip'), recursive=True)
    loose = [p for ext in ('*.fxobj', '*.bin') for p in glob.glob(os.path.join(game, '**', ext), recursive=True)]
    shaders = {}
    with ProcessPoolExecutor() as pool:
        results = list(pool.map(scan_zip, zips, chunksize=4)) + list(pool.map(scan_file, loose, chunksize=16))
        for n, result in enumerate(results):
            for pixel, code in result:
                shaders[fnv1a(code) ^ (1 if pixel else 0)] = (pixel, code)
            if n % 100 == 0:
                print('..', n, 'of', len(zips), 'zips,', len(shaders), 'shaders', flush=True)
    vs = sum(1 for p, _ in shaders.values() if not p)
    print('distinct shaders: %d (vs %d, ps %d)' % (len(shaders), vs, len(shaders) - vs))
    for h, (pixel, code) in shaders.items():
        name = '%s_%016x.ucode' % ('ps' if pixel else 'vs', h)
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(code)
    if len(sys.argv) > 3:
        real = {}
        for p in glob.glob(os.path.join(sys.argv[3], '*.ucode')):
            pixel = os.path.basename(p).startswith('ps')
            h = normalized_hash(open(p, 'rb').read(), pixel)
            real[os.path.basename(p)] = h
        distinct = set(real.values())
        hits = distinct & set(shaders)
        print('drawn shaders: %d files, %d after normalising, %d found (%.1f%%)' % (len(real), len(distinct), len(hits),
              100.0 * len(hits) / max(1, len(distinct))))
        wrongtype = sum(1 for h in distinct - hits if (h ^ 1) in shaders)
        print('missed only because of the vs/ps guess:', wrongtype)


if __name__ == '__main__':
    main()
