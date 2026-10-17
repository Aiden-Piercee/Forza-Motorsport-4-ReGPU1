"""Checks which known shader microcode blobs appear verbatim in the game data.

staticshaders.py <game dir> <shadercache dir> [report.json]

Loads every *.ucode (the shaders the running game has used), then scans all loose
files and zip members for them as 4-byte aligned big-endian words and reports, per
data file, which shaders it contains. Run on its own it tells us how much of the
shader set can be found without playing.
"""
import glob
import json
import os
import sys
import zipfile
from collections import defaultdict

import numpy as np

game, cache = sys.argv[1], sys.argv[2]
shaders = {}
for p in glob.glob(os.path.join(cache, '*.ucode')):
    shaders[os.path.basename(p)[:-6]] = np.frombuffer(open(p, 'rb').read(), dtype='>u4')
print('known shaders', len(shaders))
K = [np.uint64(x) for x in (0x9E3779B97F4A7C15, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5)]


def prefix_hash(words):
    w = words.astype(np.uint64)
    n = len(w) - 3
    return (w[0:n] * K[0]) ^ (w[1:n + 1] * K[1]) ^ (w[2:n + 2] * K[2]) ^ (w[3:n + 3] * K[3])


first = defaultdict(list)
for name, w in shaders.items():
    if len(w) >= 4:
        first[int(prefix_hash(w[:4])[0])].append(name)
hashset = np.array(sorted(first), dtype=np.uint64)
found = defaultdict(set)  # shader -> data files


def scan(data, source):
    if len(data) < 16:
        return
    buf = data[:len(data) // 4 * 4]
    words = np.frombuffer(buf, dtype='>u4')
    if len(words) < 4:
        return
    ph = prefix_hash(words)
    for i in np.nonzero(np.isin(ph, hashset))[0]:
        for name in first[int(ph[i])]:
            w = shaders[name][:max(4, len(shaders[name]) - 4)]  # the last words are a per-load trailer
            if i + len(w) <= len(words) and np.array_equal(words[i:i + len(w)], w):
                found[name].add(source)


for dirpath, _, names in os.walk(game):
    for fname in names:
        path = os.path.join(dirpath, fname)
        rel = os.path.relpath(path, game)
        try:
            if fname.lower().endswith('.zip'):
                with zipfile.ZipFile(path) as z:
                    for info in z.infolist():
                        if 0 < info.file_size < 100 << 20 and not info.filename.lower().endswith(('.dds', '.xma', '.fsb', '.bik')):
                            scan(z.read(info), rel + '!' + info.filename)
            elif os.path.getsize(path) < 300 << 20 and not fname.lower().endswith(('.bik', '.fsb', '.xma', '.dds')):
                scan(open(path, 'rb').read(), rel)
        except Exception as e:
            print('skip', rel, e, file=sys.stderr)
    print('..', os.path.relpath(dirpath, game), len(found), flush=True)

print('shaders found in the data files: %d of %d' % (len(found), len(shaders)))
missing = [n for n in shaders if n not in found]
print('not found:', len(missing), missing[:10])
if len(sys.argv) > 3:
    json.dump({'found': {k: sorted(v)[:5] for k, v in found.items()}, 'missing': missing}, open(sys.argv[3], 'w'))
