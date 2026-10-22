#!/usr/bin/env python3
"""Find the title's statically linked XAPI fiber functions in an xexdump image.

  findfibers.py <image.bin> <image.sections>   (out/fm4.bin, out/fm4.sections after --prepare)

src/hooks/fibers.cpp replaces ConvertThreadToFiber, ConvertFiberToThread,
CreateFiber, DeleteFiber, SwitchToFiber and the hand-written register swap that
SwitchToFiber jumps to. This finds them by what they do, not by bytes from
another title:

  - the five XAPI functions read or write the current fiber at KTHREAD+0x164,
    reached through KPCR+0x100 (lwz rX,0x100(r13)), and the creating ones
    allocate the 0xA50-byte fiber object;
  - the register swap stores r14..r31 through one register and loads them
    through another (FM4 keeps it inside SwitchToFiber, after its prologue);
  - SwitchToFiber, or a stub before it, branches to the swap's function.

It prints every function with that evidence in address order, plus a guessed
name. The XAPI object keeps these functions together (FM2: 82754CD0..82754EF0),
so expect five neighbours. Confirm each guess with build/tools/dis.exe before
using the addresses.
"""
import bisect
import struct
import sys


def parse_sections(path):
    base, sections = None, {}
    for line in open(path):
        p = line.split()
        if len(p) >= 2 and p[0] == "base":
            base = int(p[1], 16)
        elif len(p) == 4 and p[3] in ("code", "data"):
            sections[p[0]] = (int(p[1], 16), int(p[2], 16), p[3] == "code")
    return base, sections


def pdata_functions(img, base, sections):
    start, size, _ = sections[".pdata"]
    out = []
    for off in range(start - base, start - base + size, 8):
        begin, packed = struct.unpack_from(">II", img, off)
        if begin:
            out.append((begin, ((packed >> 8) & 0x3FFFFF) * 4))
    return sorted(out)


def gpr_run(words, opcode):
    """The base register if the words hold `std/ld r14..r31, d(rB)` in order with
    one base register, else None."""
    want, ra = 14, None
    for w in words:
        if (w >> 26) == opcode and (w >> 21) & 31 == want and w & 3 == 0:
            if ra is None:
                ra = (w >> 16) & 31
            if (w >> 16) & 31 == ra:
                want += 1
                if want == 32:
                    return ra
    return None


def branch_target(w, addr):
    if (w >> 26) != 18:
        return None, False
    off = w & 0x03FFFFFC
    if off & 0x02000000:
        off -= 0x04000000
    return (addr + off) & 0xFFFFFFFF, bool(w & 1)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], "rb").read()
    base, sections = parse_sections(sys.argv[2])
    code = [(s, n) for s, n, c in sections.values() if c]
    funcs = pdata_functions(img, base, sections)
    starts = [f[0] for f in funcs]

    # Register swaps: scan all code (the swap may have no .pdata entry), in
    # windows starting at each `std r14`.
    swaps = []
    for s, n in code:
        words = struct.unpack_from(f">{n // 4}I", img, s - base)
        for i, w in enumerate(words):
            if (w >> 26) != 62 or (w >> 21) & 31 != 14 or w & 3 != 0:
                continue
            # The swap saves into one fiber and restores from another (FM4: from
            # r3), with the FPRs and VMX registers in between. The save/restore
            # helpers use the same base for both, so they do not count.
            saved = gpr_run(words[i:i + 64], 62)
            restored = gpr_run(words[i:i + 400], 58)
            if saved is not None and restored is not None and saved != restored:
                swaps.append(s + i * 4)
    # The swap's entry is the start of its block: the .pdata function holding
    # the first store, or the store itself.
    swap_entries = set()
    for a in swaps:
        k = bisect.bisect_right(starts, a) - 1
        swap_entries.add(funcs[k][0] if k >= 0 and a < funcs[k][0] + funcs[k][1] else a)

    rows = []
    for begin, size in funcs:
        words = struct.unpack_from(f">{size // 4}I", img, begin - base)
        kpcr = any((w >> 26) == 32 and (w >> 16) & 31 == 13 and w & 0xFFFF == 0x100 for w in words)
        load164 = any((w >> 26) == 32 and w & 0xFFFF == 0x164 for w in words)
        store164 = any((w >> 26) == 36 and w & 0xFFFF == 0x164 for w in words)
        a50 = any((w >> 26) in (14, 24) and w & 0xFFFF == 0xA50 for w in words)
        to_swap = False
        for i, w in enumerate(words):
            t, _ = branch_target(w, begin + i * 4)
            if t is not None and t in swap_entries:
                to_swap = True
        if not (kpcr and (load164 or store164)) and not (a50 and kpcr):
            continue
        if to_swap:
            guess = "SwitchToFiber"
        elif a50 and store164:
            guess = "ConvertThreadToFiber"
        elif a50:
            guess = "CreateFiber"
        elif store164:
            guess = "ConvertFiberToThread"
        else:
            guess = "DeleteFiber?"
        flags = [n for n, v in (("kpcr", kpcr), ("lwz+164", load164), ("stw+164", store164), ("li 0xA50", a50), ("->swap", to_swap)) if v]
        rows.append((begin, size, guess, flags))

    print(f"register swap candidates: {', '.join(f'{a:08X}' for a in sorted(swap_entries)) or 'none'}")
    print(f"\n{len(rows)} functions using the current fiber (KPCR+0x100 -> KTHREAD+0x164) or the fiber object size:")
    for begin, size, guess, flags in rows:
        print(f"  {begin:08X} size {size:#6x}  {guess:<22} {' '.join(flags)}")


if __name__ == "__main__":
    main()
