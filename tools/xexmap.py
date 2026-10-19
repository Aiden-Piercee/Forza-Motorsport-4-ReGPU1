#!/usr/bin/env python3
"""Map guest addresses from a base default.xex to a title-updated one.

  xexmap.py build <base.bin> <base.sections> <new.bin> <new.sections> <map.json>
  xexmap.py lookup <map.json> <hex address>...
  xexmap.py report <map.json>

The .bin files are xexdump images (flat, from 0x82000000) and the .sections files its
stdout (the section list). Functions come from .pdata and are matched by a signature of
their instruction words with moved-address fields masked; unique signatures anchor the
match, the order of the functions places the rest, and functions that changed are paired
by position and flagged. Data addresses are learned from matched code: where a lis/addi,
lis/ori or lis/load pair builds an address in both images, that gives base -> new.
"""
import bisect
import collections
import json
import struct
import sys

BASE = 0x82000000


def sections(path):
    out = {}
    for line in open(path):
        p = line.split()
        if len(p) == 3 and p[2] in ('code', 'data'):
            out[p[0]] = (int(p[1], 16), int(p[2 - 1], 16))
    return out


def parse_sections(path):
    out = {}
    for line in open(path):
        p = line.split()
        if len(p) == 4 and p[3] in ('code', 'data'):
            out[p[0]] = (int(p[1], 16), int(p[2], 16))
            if p[3] == 'code':
                out.setdefault('_code', []).append((int(p[1], 16), int(p[2], 16)))
    return out


def functions(img, sec):
    pd, pds = sec['.pdata']
    code = sec['_code']
    out = []
    for off in range(pd - BASE, pd - BASE + pds, 8):
        begin, info = struct.unpack('>II', img[off:off + 8])
        size = ((info >> 8) & 0x3FFFFF) * 4
        if begin and size and any(b <= begin < b + n for b, n in code):
            out.append((begin, size))
    out.sort()
    return out


def words(img, begin, size):
    return struct.unpack('>%dI' % (size // 4), img[begin - BASE:begin - BASE + size])


def signature(ws):
    sig = []
    for w in ws:
        op = w >> 26
        if op == 18:
            w &= ~0x03FFFFFC
        elif op in (14, 15, 24, 25) or 32 <= op <= 55:
            w &= ~0xFFFF
        sig.append(w)
    return hash(tuple(sig))


def lis(seq):
    """Longest increasing subsequence of (i, j) pairs by j, given sorted by i."""
    tails, idx, prev = [], [], [-1] * len(seq)
    for k, (_, j) in enumerate(seq):
        pos = bisect.bisect_left(tails, j)
        if pos == len(tails):
            tails.append(j)
            idx.append(k)
        else:
            tails[pos] = j
            idx[pos] = k
        prev[k] = idx[pos - 1] if pos else -1
    out, k = [], idx[-1] if idx else -1
    while k >= 0:
        out.append(seq[k])
        k = prev[k]
    return out[::-1]


def match(fb, fn, ib, inn):
    sb = [(s, signature(words(ib, b, s))) for b, s in fb]
    sn = [(s, signature(words(inn, b, s))) for b, s in fn]
    cb, cn = collections.Counter(sb), collections.Counter(sn)
    pos_new = {}
    for j, k in enumerate(sn):
        if cn[k] == 1:
            pos_new[k] = j
    anchors = [(i, pos_new[k]) for i, k in enumerate(sb) if cb[k] == 1 and k in pos_new]
    anchors = lis(anchors)
    pairs = {}
    for i, j in anchors:
        pairs[i] = (j, 'exact')
    # Fill the gaps between consecutive anchors in order.
    bounds = [(-1, -1)] + anchors + [(len(fb), len(fn))]
    for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
        gi, gj = list(range(i0 + 1, i1)), list(range(j0 + 1, j1))
        # Equal signatures first, in order.
        a, b = 0, 0
        while a < len(gi) and b < len(gj):
            if sb[gi[a]] == sn[gj[b]]:
                pairs[gi[a]] = (gj[b], 'exact')
                a += 1
                b += 1
            else:
                # Look ahead a short way for a resync.
                found = False
                for da in range(0, 6):
                    for db in range(0, 6):
                        if (da or db) and a + da < len(gi) and b + db < len(gj) and sb[gi[a + da]] == sn[gj[b + db]]:
                            # Changed functions between: pair them by position.
                            for x in range(min(da, db)):
                                pairs[gi[a + x]] = (gj[b + x], 'changed')
                            a, b = a + da, b + db
                            found = True
                            break
                    if found:
                        break
                if not found:
                    pairs[gi[a]] = (gj[b], 'changed')
                    a += 1
                    b += 1
    return pairs


def signed16(v):
    return v - 0x10000 if v & 0x8000 else v


def learn_data(fb, fn, pairs, ib, inn, lo, hi):
    """Data addresses built by lis + (addi | ori | load/store) in matched functions."""
    votes = collections.defaultdict(collections.Counter)
    for i, (j, kind) in pairs.items():
        if kind != 'exact':
            continue
        wb, wn = words(ib, *fb[i]), words(inn, *fn[j])
        regs = {}
        for a, b in zip(wb, wn):
            op = a >> 26
            if op == 15 and ((a >> 16) & 31) == 0:           # lis rD, imm
                regs[(a >> 21) & 31] = (a & 0xFFFF, b & 0xFFFF)
                continue
            if op == 14 or 32 <= op <= 55:                   # addi, loads, stores: base is RA
                r = (a >> 16) & 31
                if r in regs:
                    hb, hn = regs[r]
                    base_addr = (hb << 16) + signed16(a & 0xFFFF)
                    new_addr = (hn << 16) + signed16(b & 0xFFFF)
                    if lo <= base_addr < hi and lo <= new_addr < hi:
                        votes[base_addr][new_addr] += 1
            elif op == 24:                                   # ori rA, rS, imm: source is RS
                r = (a >> 21) & 31
                if r in regs:
                    hb, hn = regs[r]
                    base_addr = (hb << 16) | (a & 0xFFFF)
                    new_addr = (hn << 16) | (b & 0xFFFF)
                    if lo <= base_addr < hi and lo <= new_addr < hi:
                        votes[base_addr][new_addr] += 1
    data = {}
    for k, v in votes.items():
        data[k] = v.most_common(1)[0][0]
    return data


def learn_prefix_words(fb, fn, pairs, ib, inn, lo, hi):
    """Address words stored just before functions (exception handler records, thunk targets):
    where an exact pair has the same word position holding different in-range values, vote."""
    votes = collections.defaultdict(collections.Counter)
    for i, (j, kind) in pairs.items():
        if kind != 'exact' or fb[i][1] != fn[j][1]:
            continue
        for k in (1, 2, 3, 4):
            wb = struct.unpack('>I', ib[fb[i][0] - BASE - 4 * k:fb[i][0] - BASE - 4 * k + 4])[0]
            wn = struct.unpack('>I', inn[fn[j][0] - BASE - 4 * k:fn[j][0] - BASE - 4 * k + 4])[0]
            if lo <= wb < hi and lo <= wn < hi and wb != wn:
                votes[wb][wn] += 1
    return {k: v.most_common(1)[0][0] for k, v in votes.items()}


def build(args):
    ib, sb_path, inn, sn_path, out = args
    ib, inn = open(ib, 'rb').read(), open(inn, 'rb').read()
    sb, sn = parse_sections(sb_path), parse_sections(sn_path)
    fb, fn = functions(ib, sb), functions(inn, sn)
    print('functions: base %d, new %d' % (len(fb), len(fn)))
    pairs = match(fb, fn, ib, inn)
    exact = sum(1 for _, k in pairs.values() if k == 'exact')
    changed = sum(1 for _, k in pairs.values() if k == 'changed')
    print('matched: %d exact, %d changed (paired by position), %d base functions unmatched' % (
        exact, changed, len(fb) - len(pairs)))
    lo = min(v[0] for k, v in sb.items() if k != '_code')
    hi = max(v[0] + v[1] for k, v in sb.items() if k != '_code')
    data = learn_data(fb, fn, pairs, ib, inn, lo, hi)
    prefix = learn_prefix_words(fb, fn, pairs, ib, inn, lo, hi)
    for k, v in prefix.items():
        data.setdefault(k, v)
    print('data addresses learned: %d (%d from words before functions)' % (len(data), len(prefix)))
    m = dict(
        functions=[[fb[i][0], fb[i][1], fn[j][0], fn[j][1], kind] for i, (j, kind) in sorted(pairs.items())],
        unmatched=[[fb[i][0], fb[i][1]] for i in range(len(fb)) if i not in pairs],
        data={'%08X' % k: '%08X' % v for k, v in data.items()},
        sections=dict(base=sb, new=sn),
    )
    json.dump(m, open(out, 'w'))
    print('wrote', out)


def masked_words(img, begin, size):
    return [w for w in (masked(x) for x in words(img, begin, size))]


def masked(w):
    op = w >> 26
    if op == 18:
        return w & ~0x03FFFFFC
    if op in (14, 15, 24, 25) or 32 <= op <= 55:
        return w & ~0xFFFF
    return w


class Map:
    """Looks addresses up; needs the two flat images (default: next to the map as base.bin/image.bin)."""

    def __init__(self, path, base_bin, new_bin):
        import difflib
        self.difflib = difflib
        m = json.load(open(path))
        self.f = sorted(m['functions'])
        self.starts = [x[0] for x in self.f]
        self.data = {int(k, 16): int(v, 16) for k, v in m['data'].items()}
        self.sections = m['sections']
        self.unmatched = m['unmatched']
        self.ib = open(base_bin, 'rb').read()
        self.inn = open(new_bin, 'rb').read()
        self.code = [(b, b + n) for b, n in self.sections['base']['_code']]
        self._align = {}

    def _words_masked(self, img, begin, size):
        return [masked(w) for w in words(img, begin, size)]

    def _aligned(self, key, bb, bs, nb, ns):
        """Instruction index map base -> new for two code ranges (cached)."""
        if key not in self._align:
            a = self._words_masked(self.ib, bb, bs)
            b = self._words_masked(self.inn, nb, ns)
            sm = self.difflib.SequenceMatcher(None, a, b, autojunk=False)
            idx = {}
            for blk in sm.get_matching_blocks():
                for k in range(blk.size):
                    idx[blk.a + k] = blk.b + k
            self._align[key] = idx
        return self._align[key]

    def lookup(self, addr):
        """(new address, how): exact, aligned, data, or None with the reason."""
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            b, s, n, ns, kind = self.f[i]
            if b <= addr < b + s:
                if s == ns and kind == 'exact':
                    return n + (addr - b), 'exact'
                idx = self._aligned(('f', b), b, s, n, ns)
                j = idx.get((addr - b) // 4)
                if j is None:
                    return None, 'changed code'
                return n + j * 4 + (addr & 3), 'aligned'
            # Between functions: a value learned from the code that uses it beats alignment.
            if addr in self.data:
                return self.data[addr], 'data'
            # Helpers, thunks, padding: align the gap to its neighbours.
            if i + 1 < len(self.f) and any(a <= addr < z for a, z in self.code):
                pb_end = b + s
                nb_start = self.f[i + 1][0]
                pn_end = n + ns
                nn_start = self.f[i + 1][2]
                if pb_end <= addr < nb_start and nn_start >= pn_end:
                    gap_b, gap_n = nb_start - pb_end, nn_start - pn_end
                    if gap_b == gap_n:
                        return pn_end + (addr - pb_end), 'gap'
                    idx = self._aligned(('g', pb_end), pb_end, gap_b, pn_end, gap_n)
                    j = idx.get((addr - pb_end) // 4)
                    if j is not None:
                        return pn_end + j * 4 + (addr & 3), 'gap aligned'
        if addr in self.data:
            return self.data[addr], 'data'
        return None, 'unknown'


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('build', 'lookup', 'report'):
        print(__doc__)
        return 1
    if sys.argv[1] == 'build':
        build(sys.argv[2:7])
    elif sys.argv[1] == 'lookup':
        m = Map(sys.argv[2], 'out/base.bin', 'out/tu4/image.bin')
        for a in sys.argv[3:]:
            n, how = m.lookup(int(a, 16))
            print('%s -> %s (%s)' % (a, '%08X' % n if n else '-', how))
    else:
        m = Map(sys.argv[2], 'out/base.bin', 'out/tu4/image.bin')
        print('%d unmatched base functions' % len(m.unmatched))
        for b, s in m.unmatched[:40]:
            print('  %08X  %d bytes' % (b, s))
    return 0


if __name__ == '__main__':
    sys.exit(main())
