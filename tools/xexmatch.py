#!/usr/bin/env python3
"""Match functions between two XEX images (a base default.xex and a title-updated one).

  xexmatch.py <base.bin> <patched.bin> [--base-text 821D0000 --base-pdata 82176400 ...]

Both images are dumps made by xexdump (flat, from the image base 0x82000000). Function
boundaries come from .pdata. A function's signature is its instruction words with the
fields that change when code and data move masked: branch targets (b, bl), the 16-bit
immediates of addis/addi/ori and the loads and stores that complete an address. Functions
whose signatures are unique in both images are matched directly; the rest are placed by
walking in address order between matched neighbours.
"""
import struct
import sys
import collections

BASE = 0x82000000


def load(path):
    return open(path, 'rb').read()


def functions(img, pdata, pdata_size, text, text_size):
    out = []
    for off in range(pdata - BASE, pdata - BASE + pdata_size, 8):
        begin, info = struct.unpack('>II', img[off:off + 8])
        if begin == 0:
            continue
        size = ((info >> 8) & 0x3FFFFF) * 4
        if text <= begin < text + text_size and size:
            out.append((begin, size))
    out.sort()
    return out


def signature(img, begin, size):
    words = struct.unpack('>%dI' % (size // 4), img[begin - BASE:begin - BASE + size])
    sig = []
    for w in words:
        op = w >> 26
        if op == 18:                      # b / bl: target is relative to a moved function
            w &= ~0x03FFFFFC
        elif op in (14, 15, 24, 25) or 32 <= op <= 55:   # addi addis ori oris, loads/stores
            w &= ~0xFFFF
        sig.append(w)
    return hash(tuple(sig))


def main():
    base_img, new_img = load(sys.argv[1]), load(sys.argv[2])
    # Section layout (from xexdump): base then patched.
    layout = {
        'base': dict(pdata=0x82176400, pdata_size=0x4FBC8, text=0x821D0000, text_size=0x77F338),
        'new': dict(pdata=0x82183C00, pdata_size=0x50070, text=0x821E0000, text_size=0x78BC08),
    }
    fb = functions(base_img, **layout['base'])
    fn = functions(new_img, **layout['new'])
    print('functions: base %d, patched %d' % (len(fb), len(fn)))
    sb = [(b, s, signature(base_img, b, s)) for b, s in fb]
    sn = [(b, s, signature(new_img, b, s)) for b, s in fn]
    cb = collections.Counter((s, sig) for _, s, sig in sb)
    cn = collections.Counter((s, sig) for _, s, sig in sn)
    new_by_sig = {(s, sig): b for b, s, sig in sn}
    unique = exact = 0
    shifts = collections.Counter()
    for b, s, sig in sb:
        if cb[(s, sig)] == 1 and cn.get((s, sig)) == 1:
            unique += 1
            shifts[new_by_sig[(s, sig)] - b] += 1
    print('matched uniquely: %d of %d base functions (%.1f%%)' % (unique, len(fb), 100.0 * unique / len(fb)))
    print('distinct address shifts among them: %d; most common:' % len(shifts))
    for d, c in shifts.most_common(6):
        print('  %+#x  x%d' % (d, c))
    gone = sum(1 for b, s, sig in sb if (s, sig) not in cn)
    print('base functions with no counterpart (changed or removed): %d' % gone)
    added = sum(1 for b, s, sig in sn if (s, sig) not in cb)
    print('patched functions with no counterpart (changed or new): %d' % added)


if __name__ == '__main__':
    main()
