#!/usr/bin/env python3
"""Measure how fast a captured track plays against its reference.

  audiospeed.py <dump.wav (FM2_DUMP_AUDIO, 6 ch float)> <reference.wav> [window seconds]

For each window of the dump, finds where in the reference it matches best (log spectrogram
correlation) and prints the position, so a track that plays too fast shows a slope above 1.
"""
import struct
import sys

import numpy as np

RATE = 48000
N, HOP = 2048, 1024


def read_wav(path, max_seconds=None):
    with open(path, 'rb') as f:
        data = f.read()
    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, pos)
        body = pos + 8
        if tag == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', data, body)
        elif tag == b'data':
            size = min(size, len(data) - body)
            raw = data[body:body + size]
            ch, bits, kind = fmt[1], fmt[5], fmt[0]
            if kind == 3 or (kind == 0xFFFE and bits == 32):
                x = np.frombuffer(raw[:len(raw) // 4 * 4], dtype='<f4')
            else:
                x = np.frombuffer(raw[:len(raw) // 2 * 2], dtype='<i2').astype(np.float32) / 32768.0
            x = x[:len(x) // ch * ch].reshape(-1, ch)
            return x, fmt[2]
        pos = body + size + (size & 1)
    raise SystemExit('no data chunk')


def features(mono):
    win = np.hanning(N).astype(np.float32)
    n = (len(mono) - N) // HOP
    idx = np.arange(N)[None, :] + HOP * np.arange(n)[:, None]
    spec = np.abs(np.fft.rfft(mono[idx] * win, axis=1))
    edges = np.unique(np.geomspace(4, N // 2, 49).astype(int))
    bands = np.stack([spec[:, a:b].mean(axis=1) for a, b in zip(edges[:-1], edges[1:])], axis=1)
    f = np.log1p(bands * 50)
    f -= f.mean(axis=1, keepdims=True)
    return f / (np.linalg.norm(f, axis=1, keepdims=True) + 1e-6)


def main():
    dump, ref = sys.argv[1], sys.argv[2]
    window = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
    d, drate = read_wav(dump)
    r, rrate = read_wav(ref)
    dm = d[:, 0] + d[:, 1]
    rm = r[:, 0] + (r[:, 1] if r.shape[1] > 1 else r[:, 0])
    df, rf = features(dm), features(rm)
    frames = int(window * RATE / HOP)
    hop_s = HOP / RATE
    print('dump %.1f s, reference %.1f s' % (len(dm) / RATE, len(rm) / RATE))
    last = None
    for start in range(0, len(df) - frames, frames // 2):
        seg = df[start:start + frames]
        if np.abs(dm[start * HOP:(start + frames) * HOP]).max() < 1e-3:
            continue
        best, where = -1, 0
        for pos in range(0, len(rf) - frames, 2):
            score = float((seg * rf[pos:pos + frames]).sum() / frames)
            if score > best:
                best, where = score, pos
        t, p = start * hop_s, where * hop_s
        slope = '' if last is None or t == last[0] else '  speed %.2fx' % ((p - last[1]) / (t - last[0]))
        print('dump %6.1f s -> reference %6.1f s  (match %.2f)%s' % (t, p, best, slope))
        last = (t, p)


main()
