"""Extracts every shader object from the game data into the renderer's shader cache.

extractshaders.py <game dir> <shadercache dir> [image.bin ...]

A shader object carries an IM_LOAD_IMMEDIATE packet: big-endian word
0xC0nn2B00 (type 3, opcode 0x2B), then the type (0 vertex, 1 pixel), the size in
dwords, and the microcode. Every packet found in the loose files, the zip members
and the dumped executable image is validated and written as <vs|ps>_<hash>.ucode,
where the hash is FNV-1a over the microcode bytes XOR the type (the same name the
renderer uses). Shaders already in the cache are skipped.
"""
import os
import sys
import zipfile
from collections import Counter

import numpy as np

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = 0xFFFFFFFFFFFFFFFF


def fnv1a(b):
    h = FNV_OFFSET
    for x in b:
        h = ((h ^ x) * FNV_PRIME) & MASK
    return h


def find_packets(data, out):
    """Adds (type, microcode bytes) for every valid IM_LOAD_IMMEDIATE packet in data."""
    buf = data[:len(data) // 4 * 4]
    if len(buf) < 16:
        return
    words = np.frombuffer(buf, dtype='>u4')
    # word = C0 nn 2B 00 with the count in nn's low bits: (w & 0xFFFF00FC) == 0x002B00C0
    hits = np.nonzero((words & np.uint32(0xC000FFFF)) == np.uint32(0xC0002B00))[0]
    for i in hits:
        if i + 3 > len(words):
            continue
        count = int(((words[i] >> 16) & 0x3F) << 8 | (words[i] >> 8) & 0xFF) if False else None
        # count field: bits 16..29 of the header; packet body is count+1 dwords
        count = int((words[i] >> 16) & 0x3FFF)
        kind = int(words[i + 1])
        size = int(words[i + 2])
        if kind > 1 or size < 2 or size > 0x2000 or count != size + 1:
            continue
        if i + 3 + size > len(words):
            continue
        code = buf[(i + 3) * 4:(i + 3 + size) * 4]
        out.append((kind, code))


def main():
    game, cache = sys.argv[1], sys.argv[2]
    images = sys.argv[3:]
    os.makedirs(cache, exist_ok=True)
    existing = set(n[:-6] for n in os.listdir(cache) if n.endswith('.ucode'))
    found = {}
    sources = Counter()

    def add(source, kind, code):
        h = fnv1a(code) ^ kind
        name = ('ps' if kind == 1 else 'vs') + '_%016x' % h
        if name not in found:
            found[name] = (kind, code)
            sources[source] += 1

    for img in images:
        out = []
        find_packets(open(img, 'rb').read(), out)
        for kind, code in out:
            add('image', kind, code)
    for dirpath, _, names in os.walk(game):
        for fname in names:
            path = os.path.join(dirpath, fname)
            rel = os.path.relpath(path, game)
            try:
                if fname.lower().endswith('.zip'):
                    with zipfile.ZipFile(path) as z:
                        for info in z.infolist():
                            if 0 < info.file_size < 100 << 20 and not info.filename.lower().endswith(('.dds', '.xma', '.fsb', '.bik')):
                                out = []
                                find_packets(z.read(info), out)
                                for kind, code in out:
                                    add(rel, kind, code)
                elif os.path.getsize(path) < 300 << 20 and not fname.lower().endswith(('.bik', '.fsb', '.xma', '.dds')):
                    out = []
                    find_packets(open(path, 'rb').read(), out)
                    for kind, code in out:
                        add(rel, kind, code)
            except Exception as e:
                print('skip', rel, e, file=sys.stderr)
    new = 0
    for name, (kind, code) in found.items():
        if name[3:] in existing:
            continue
        with open(os.path.join(cache, name + '.ucode'), 'wb') as f:
            f.write(code)
        new += 1
    print('distinct shaders in the data: %d (vs %d, ps %d)' % (len(found),
          sum(1 for k, _ in found.values() if k == 0), sum(1 for k, _ in found.values() if k == 1)))
    print('already in the cache: %d, new written: %d' % (len(found) - new, new))
    print('by source (top 5):', sources.most_common(5))


main()
