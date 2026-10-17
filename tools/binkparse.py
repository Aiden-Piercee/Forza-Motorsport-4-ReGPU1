"""Parse-only Bink 1 ('i') video reference, after FFmpeg's libavcodec/bink.c.

binkparse.py file.bik frame binkdata.h

Prints the bit position at each plane start/end and each row start of one
frame, so the decoder's own reader positions can be checked against it.
Pixel values are not reconstructed; only what decides how many bits are read.
"""
import re
import struct
import sys

path, frame_no, data_h = sys.argv[1], int(sys.argv[2]), sys.argv[3]

src = open(data_h).read()


def table(name):
    body = src[src.index(name):]
    body = body[body.index('{') + 1:body.index('};')]
    return [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\d+', body)]


tree_bits = table('bink_tree_bits')
tree_lens = table('bink_tree_lens')
# vlc tables: code (LSB-first) -> symbol index
VLCS = []
for t in range(16):
    codes = {}
    for i in range(16):
        codes[(tree_lens[t * 16 + i], tree_bits[t * 16 + i])] = i
    VLCS.append(codes)


class Bits:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def get(self, n):
        v = 0
        for i in range(n):
            byte = self.data[(self.pos + i) >> 3]
            v |= ((byte >> ((self.pos + i) & 7)) & 1) << i
        self.pos += n
        return v

    def bit(self):
        return self.get(1)

    def huff(self, tree):
        vlc_num, syms = tree
        codes = VLCS[vlc_num]
        code = 0
        for n in range(1, 17):
            code |= self.bit() << (n - 1)
            if (n, code) in codes:
                return syms[codes[(n, code)]]
        raise ValueError('bad vlc at %d' % self.pos)


def merge(gb, src_, size):
    a, b = src_[:size], src_[size:2 * size]
    out = []
    while a and b:
        out.append(a.pop(0) if not gb.bit() else b.pop(0))
    return out + a + b


def read_tree(gb):
    vlc_num = gb.get(4)
    if not vlc_num:
        return (0, list(range(16)))
    if gb.bit():
        ln = gb.get(3)
        syms, seen = [], set()
        for i in range(ln + 1):
            s = gb.get(4)
            syms.append(s)
            seen.add(s)
        syms += [i for i in range(16) if i not in seen][:16 - len(syms)]
        return (vlc_num, syms)
    ln = gb.get(2)
    cur = list(range(16))
    for i in range(ln + 1):
        size = 1 << i
        nxt = []
        for t in range(0, 16, size * 2):
            nxt += merge(gb, cur[t:t + size * 2], size)
        cur = nxt
    return (vlc_num, cur)


def log2(v):
    return v.bit_length() - 1


BT, SBT, COL, PAT, XO, YO, IDC, EDC, RUN = range(9)


class Bundle:
    def __init__(self, ln):
        self.len, self.vals, self.ptr, self.dead = ln, [], 0, False

    def ready(self):
        # CHECK_READ_VAL: decode only when everything decoded so far was consumed
        return not self.dead and len(self.vals) <= self.ptr

    def get(self):
        v = self.vals[self.ptr]
        self.ptr += 1
        return v


def check(gb, b):
    if not b.ready():
        return None
    t = gb.get(b.len)
    if not t:
        b.dead = True
        return None
    return t


def read_block_types(gb, b, tree):
    t = check(gb, b)
    if t is None:
        return
    end = len(b.vals) + t
    if gb.bit():
        b.vals += [gb.get(4)] * t
        return
    last = 0
    while len(b.vals) < end:
        v = gb.huff(tree)
        if v < 12:
            last = v
            b.vals.append(v)
        else:
            run = [4, 8, 12, 32][v - 12]
            if end - len(b.vals) < run:
                raise ValueError('block type run overshoot at bit %d' % gb.pos)
            b.vals += [last] * run


def read_runs(gb, b, tree):
    t = check(gb, b)
    if t is None:
        return
    if gb.bit():
        b.vals += [gb.get(4)] * t
    else:
        for _ in range(t):
            b.vals.append(gb.huff(tree))


def read_motion(gb, b, tree):
    t = check(gb, b)
    if t is None:
        return
    if gb.bit():
        v = gb.get(4)
        if v and gb.bit():
            v = -v
        b.vals += [v] * t
    else:
        for _ in range(t):
            v = gb.huff(tree)
            if v and gb.bit():
                v = -v
            b.vals.append(v)


def read_patterns(gb, b, tree):
    t = check(gb, b)
    if t is None:
        return
    for _ in range(t):
        v = gb.huff(tree)
        v |= gb.huff(tree) << 4
        b.vals.append(v)


def read_colors(gb, b, tree, ctx):
    t = check(gb, b)
    if t is None:
        return
    if gb.bit():
        ctx['last'] = gb.huff(ctx['high'][ctx['last']])
        v = (ctx['last'] << 4) | gb.huff(tree)
        b.vals += [v] * t
    else:
        for _ in range(t):
            ctx['last'] = gb.huff(ctx['high'][ctx['last']])
            b.vals.append((ctx['last'] << 4) | gb.huff(tree))


def read_dcs(gb, b, has_sign):
    ln = check(gb, b)
    if ln is None:
        return
    v = gb.get(11 - has_sign)
    if v and has_sign and gb.bit():
        v = -v
    b.vals.append(v)
    ln -= 1
    for i in range(0, ln, 8):
        l2 = min(ln - i, 8)
        bsize = gb.get(4)
        for _ in range(l2):
            if bsize:
                v2 = gb.get(bsize)
                if v2 and gb.bit():
                    v2 = -v2
                v += v2
            b.vals.append(v)


def coeff_lists(gb, bits_fn, residue):
    pass


def read_dct(gb):
    cl = [0] * 128
    ml = [0] * 128
    start = end = 64
    for c, m in ((4, 0), (24, 0), (44, 0), (1, 3), (2, 3), (3, 3)):
        cl[end], ml[end] = c, m
        end += 1
    bits = gb.get(4) - 1
    while bits >= 0:
        pos = start
        while pos < end:
            if not (ml[pos] | cl[pos]) or not gb.bit():
                pos += 1
                continue
            cc, mode = cl[pos], ml[pos]
            if mode in (0, 2):
                if mode == 0:
                    cl[pos], ml[pos] = cc + 4, 1
                else:
                    cl[pos], ml[pos] = 0, 0
                    pos += 1
                for i in range(4):
                    if gb.bit():
                        start -= 1
                        cl[start], ml[start] = cc, 3
                    else:
                        if not bits:
                            gb.bit()
                        else:
                            gb.get(bits)
                            gb.bit()
                    cc += 1
            elif mode == 1:
                ml[pos] = 2
                for i in range(3):
                    cc += 4
                    cl[end], ml[end] = cc, 2
                    end += 1
            else:
                if not bits:
                    gb.bit()
                else:
                    gb.get(bits)
                    gb.bit()
                cl[pos], ml[pos] = 0, 0
                pos += 1
        bits -= 1
    gb.get(4)  # quant index


def read_residue(gb, masks):
    cl = [0] * 128
    ml = [0] * 128
    start = end = 64
    for c, m in ((4, 0), (24, 0), (44, 0), (0, 2)):
        cl[end], ml[end] = c, m
        end += 1
    nz = 0
    mask = 1 << gb.get(3)
    while mask:
        for i in range(nz):
            if not gb.bit():
                continue
            masks -= 1
            if masks < 0:
                return
        pos = start
        while pos < end:
            if not (cl[pos] | ml[pos]) or not gb.bit():
                pos += 1
                continue
            cc, mode = cl[pos], ml[pos]
            if mode in (0, 2):
                if mode == 0:
                    cl[pos], ml[pos] = cc + 4, 1
                else:
                    cl[pos], ml[pos] = 0, 0
                    pos += 1
                for i in range(4):
                    if gb.bit():
                        start -= 1
                        cl[start], ml[start] = cc, 3
                    else:
                        nz += 1
                        gb.bit()
                        masks -= 1
                        if masks < 0:
                            return
                    cc += 1
            elif mode == 1:
                ml[pos] = 2
                for i in range(3):
                    cc += 4
                    cl[end], ml[end] = cc, 2
                    end += 1
            else:
                nz += 1
                gb.bit()
                cl[pos], ml[pos] = 0, 0
                pos += 1
                masks -= 1
                if masks < 0:
                    return
        mask >>= 1


def run_block(gb, B):
    gb.get(4)
    i = 0
    while True:
        run = B[RUN].get() + 1
        i += run
        if i > 64:
            raise ValueError('run out of bounds at bit %d' % gb.pos)
        if gb.bit():
            B[COL].get()
        else:
            for _ in range(run):
                B[COL].get()
        if i >= 63:
            break
    if i == 63:
        B[COL].get()


def decode_plane(gb, W, H, is_chroma, log):
    bw = (W + 15) >> 4 if is_chroma else (W + 7) >> 3
    bh = (H + 15) >> 4 if is_chroma else (H + 7) >> 3
    width = W >> is_chroma
    width = max(width, 8)
    width = (width + 7) & ~7
    lens = {BT: log2((width >> 3) + 511) + 1, SBT: log2((width >> 4) + 511) + 1, COL: log2(bw * 64 + 511) + 1,
            IDC: log2((width >> 3) + 511) + 1, EDC: log2((width >> 3) + 511) + 1, XO: log2((width >> 3) + 511) + 1,
            YO: log2((width >> 3) + 511) + 1, PAT: log2((bw << 3) + 511) + 1, RUN: log2(bw * 48 + 511) + 1}
    B = {}
    trees = {}
    ctx = {'last': 0}
    for i in range(9):
        if i == COL:
            ctx['high'] = [read_tree(gb) for _ in range(16)]
        if i not in (IDC, EDC):
            trees[i] = read_tree(gb)
        B[i] = Bundle(lens[i])
    for by in range(bh):
        log('  row %d at bit %d' % (by, gb.pos))
        read_block_types(gb, B[BT], trees[BT])
        read_block_types(gb, B[SBT], trees[SBT])
        read_colors(gb, B[COL], trees[COL], ctx)
        read_patterns(gb, B[PAT], trees[PAT])
        read_motion(gb, B[XO], trees[XO])
        read_motion(gb, B[YO], trees[YO])
        read_dcs(gb, B[IDC], 0)
        read_dcs(gb, B[EDC], 1)
        read_runs(gb, B[RUN], trees[RUN])
        bx = 0
        while bx < bw:
            blk = B[BT].get()
            if ((by & 1) or (bx & 1)) and blk == 1:
                bx += 2
                continue
            if blk == 0:
                pass
            elif blk == 1:
                sub = B[SBT].get()
                if sub == 3:
                    run_block(gb, B)
                elif sub == 5:
                    B[IDC].get()
                    read_dct(gb)
                elif sub == 6:
                    B[COL].get()
                elif sub == 8:
                    B[COL].get(), B[COL].get()
                    for _ in range(8):
                        B[PAT].get()
                elif sub == 9:
                    for _ in range(64):
                        B[COL].get()
                else:
                    raise ValueError('bad 16x16 type %d' % sub)
                bx += 1
            elif blk == 2:
                B[XO].get(), B[YO].get()
            elif blk == 3:
                run_block(gb, B)
            elif blk == 4:
                B[XO].get(), B[YO].get()
                read_residue(gb, gb.get(7))
            elif blk == 5:
                B[IDC].get()
                read_dct(gb)
            elif blk == 6:
                B[COL].get()
            elif blk == 7:
                B[XO].get(), B[YO].get()
                B[EDC].get()
                read_dct(gb)
            elif blk == 8:
                B[COL].get(), B[COL].get()
                for _ in range(8):
                    B[PAT].get()
            elif blk == 9:
                B[COL].ptr += 64
            else:
                raise ValueError('bad block type %d' % blk)
            if by == bh - 1:
                log('    last row bx %d type %d' % (bx, blk))
            bx += 1
    end = gb.pos
    if gb.pos & 31:
        gb.pos += 32 - (gb.pos & 31)
    return end


f = open(path, 'rb').read()
W, H = struct.unpack('<II', f[20:28])
n, = struct.unpack('<I', f[8:12])
tracks, = struct.unpack('<I', f[40:44])
offs = [struct.unpack('<I', f[44 + tracks * 12 + 4 * i:48 + tracks * 12 + 4 * i])[0] & ~1 for i in range(n + 1)]
data = f[offs[frame_no]:offs[frame_no + 1]]
gb = Bits(data + b'\0' * 64)
gb.get(32)
for plane in range(3):
    start = gb.pos
    end = decode_plane(gb, W, H, plane != 0, print)
    print('plane %d: bits %d..%d (bytes +%X..+%X), next plane at +%X' % (plane, start, end, start // 8, end // 8, gb.pos // 8))
