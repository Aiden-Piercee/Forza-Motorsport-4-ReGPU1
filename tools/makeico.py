#!/usr/bin/env python3
"""Builds assets/fm2.ico (16, 32, 48, 64, 128 and 256 pixel PNG entries) from assets/icon.png.

  makeico.py [icon.png [out.ico]]

Needs only numpy: the PNG is read and written here (8-bit RGB or RGBA, no interlacing).
"""
import struct
import sys
import zlib

import numpy as np


def read_png(path):
    d = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        if t == b'IDAT':
            idat += body
        pos += 12 + n
    ch = {2: 3, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    out = np.zeros((h, stride), np.int32)
    prev = np.zeros(stride, np.int32)
    i = 0
    for y in range(h):
        f = raw[i]
        line = np.frombuffer(raw[i + 1:i + 1 + stride], np.uint8).astype(np.int32)
        i += 1 + stride
        cur = np.zeros(stride, np.int32)
        for x in range(stride):
            a = cur[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 0: p = 0
            elif f == 1: p = a
            elif f == 2: p = b
            elif f == 3: p = (a + b) // 2
            else:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            cur[x] = (line[x] + p) & 255
        out[y] = cur
        prev = cur
    img = out.reshape(h, w, ch).astype(np.uint8)
    if ch == 3:
        img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], axis=2)
    return img


def write_png(img):
    h, w, _ = img.shape
    raw = b''.join(b'\x00' + img[y].tobytes() for y in range(h))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def resize(img, size):
    """Bilinear, with area averaging when shrinking."""
    h, w, _ = img.shape
    f = img.astype(np.float64)
    if size < w:
        k = w / size
        out = np.zeros((size, size, 4))
        for y in range(size):
            y0, y1 = int(y * k), max(int((y + 1) * k), int(y * k) + 1)
            for x in range(size):
                x0, x1 = int(x * k), max(int((x + 1) * k), int(x * k) + 1)
                out[y, x] = f[y0:y1, x0:x1].mean(axis=(0, 1))
        return np.clip(out + 0.5, 0, 255).astype(np.uint8)
    ys = (np.arange(size) + 0.5) * h / size - 0.5
    xs = (np.arange(size) + 0.5) * w / size - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, h - 1); y1 = np.clip(y0 + 1, 0, h - 1); fy = (ys - np.floor(ys))[:, None, None]
    x0 = np.clip(np.floor(xs).astype(int), 0, w - 1); x1 = np.clip(x0 + 1, 0, w - 1); fx = (xs - np.floor(xs))[None, :, None]
    top = f[y0][:, x0] * (1 - fx) + f[y0][:, x1] * fx
    bottom = f[y1][:, x0] * (1 - fx) + f[y1][:, x1] * fx
    return np.clip(top * (1 - fy) + bottom * fy + 0.5, 0, 255).astype(np.uint8)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'assets/icon.png'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'assets/fm2.ico'
    img = read_png(src)
    sizes = [16, 32, 48, 64, 128, 256]
    pngs = [write_png(img if s == img.shape[0] else resize(img, s)) for s in sizes]
    header = struct.pack('<HHH', 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b''
    for s, p in zip(sizes, pngs):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(p), offset)
        offset += len(p)
    open(dst, 'wb').write(header + entries + b''.join(pngs))
    print('wrote', dst, sizes)


main()
