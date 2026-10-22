#!/usr/bin/env python3
"""Find the image-specific values a XenonRecomp config needs, in an xexdump image.

  fm4_config.py <image.bin> <image.sections> [--write <config.toml>]

<image.bin> and <image.sections> are xexdump's output file and its stdout (the
"base ... size ... entry ..." line and the section list). Works for any image base,
so it serves default.xex and both facade modules.

Prints:
  - the eight register save/restore helper addresses (exactly one match each, or
    NOT FOUND; a module without a helper keeps 0 and XenonRecomp treats calls to
    it as ordinary functions),
  - the exception-data words XenonRecomp must skip: .pdata entries with
    ExceptionFlag set are preceded by 8 bytes of handler data, whose first word is
    the handler's address,
  - setjmp/longjmp candidates: functions that store (or load) r14..r31 to (or
    from) consecutive offsets of r3. Fiber switches and context captures match
    too, so confirm each candidate with tools/xexdump's dis
    (build/tools/dis.exe <xex> <address> <count>) before using it.

--write updates the helper keys and the generated exception-data block of the
config in place. setjmp_address and longjmp_address are never written.
"""
import bisect
import collections
import re
import struct
import sys

HELPERS = {
    "restgprlr_14_address": "e9c1ff68",
    "savegprlr_14_address": "f9c1ff68",
    "restfpr_14_address": "c9ccff70",
    "savefpr_14_address": "d9ccff70",
    "restvmx_14_address": "3960fee07dcb60ce",
    "savevmx_14_address": "3960fee07dcb61ce",
    "restvmx_64_address": "3960fc00100b60cb",
    "savevmx_64_address": "3960fc00100b61cb",
}
EH_BEGIN = "    # BEGIN fm4_config.py exception data"
EH_END = "    # END fm4_config.py exception data"


def parse_sections(path):
    base, sections = None, {}
    for line in open(path):
        p = line.split()
        if len(p) >= 2 and p[0] == "base":
            base = int(p[1], 16)
        elif len(p) == 4 and p[3] in ("code", "data"):
            sections[p[0]] = (int(p[1], 16), int(p[2], 16), p[3] == "code")
    if base is None:
        sys.exit(f"{path}: no 'base' line (is it xexdump's stdout?)")
    return base, sections


def find_helpers(img, base, code):
    out = {}
    for name, hx in HELPERS.items():
        pat, hits, i = bytes.fromhex(hx), [], img.find(bytes.fromhex(hx))
        while i != -1:
            addr = base + i
            if i % 4 == 0 and any(s <= addr < s + n for s, n in code):
                hits.append(addr)
            i = img.find(pat, i + 1)
        out[name] = hits
    return out


def read_pdata(img, base, sections):
    if ".pdata" not in sections:
        sys.exit("no .pdata section")
    start, size, _ = sections[".pdata"]
    funcs = []
    for off in range(start - base, start - base + size, 8):
        begin, packed = struct.unpack_from(">II", img, off)
        if begin:
            funcs.append((begin, ((packed >> 8) & 0x3FFFFF) * 4, bool(packed >> 31)))
    funcs.sort()
    return funcs


def exception_words(img, base, funcs):
    counts = collections.Counter()
    for begin, _, has_eh in funcs:
        if has_eh:
            counts[struct.unpack_from(">I", img, begin - 8 - base)[0]] += 1
    return counts


def gpr_run_candidates(img, base, code, opcode):
    """Starts of runs of 18 instructions `std/ld rN, d(r3)` for N = 14..31 with
    ascending, 8-byte spaced offsets (other instructions may sit in between)."""
    hits = []
    for s, n in code:
        words = struct.unpack_from(f">{n // 4}I", img, s - base)
        i = 0
        while i < len(words):
            w = words[i]
            if (w >> 26) == opcode and (w >> 21) & 31 == 14 and (w >> 16) & 31 == 3 and w & 3 == 0:
                want, last, j = 15, w & 0xFFFC, i + 1
                while j < len(words) and j - i < 48 and want <= 31:
                    x = words[j]
                    if (x >> 26) == opcode and (x >> 21) & 31 == want and (x >> 16) & 31 == 3 and x & 3 == 0:
                        if (x & 0xFFFC) != last + 8:
                            break
                        last, want = x & 0xFFFC, want + 1
                    j += 1
                if want == 32:
                    hits.append(s + i * 4)
                    i = j
                    continue
            i += 1
    return hits


def owner(funcs, starts, addr):
    k = bisect.bisect_right(starts, addr) - 1
    if k >= 0 and funcs[k][0] <= addr < funcs[k][0] + funcs[k][1]:
        return f"in .pdata function {funcs[k][0]:08X}"
    return "outside any .pdata function"


def write_config(path, helpers, eh):
    text = open(path, encoding="utf-8").read()
    for name, hits in helpers.items():
        value = f"0x{hits[0]:08X}" if len(hits) == 1 else "0x0"
        text, n = re.subn(rf"(?m)^{name}\s*=\s*0x[0-9A-Fa-f]+", f"{name} = {value}", text)
        if n != 1:
            sys.exit(f"{path}: expected one '{name} = ...' line, found {n}")
    begin, end = text.find(EH_BEGIN), text.find(EH_END)
    if begin < 0 or end < begin:
        sys.exit(f"{path}: missing the '{EH_BEGIN.strip()}' ... '{EH_END.strip()}' block")
    lines = [f"    {{ data = 0x{w:08X}, size = 8 }}, # handler of {c} functions" for w, c in eh.most_common()]
    text = text[:begin] + EH_BEGIN + "\n" + "".join(l + "\n" for l in lines) + text[end:]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"updated {path}")


def main():
    args = sys.argv[1:]
    write = None
    if "--write" in args:
        k = args.index("--write")
        write = args[k + 1]
        del args[k:k + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    img = open(args[0], "rb").read()
    base, sections = parse_sections(args[1])
    code = [(s, n) for s, n, is_code in sections.values() if is_code]

    helpers = find_helpers(img, base, code)
    for name, hits in helpers.items():
        print(f"{name} = {', '.join(f'0x{h:08X}' for h in hits) or 'NOT FOUND'}")
    ambiguous = [n for n, h in helpers.items() if len(h) > 1]

    funcs = read_pdata(img, base, sections)
    starts = [f[0] for f in funcs]
    eh = exception_words(img, base, funcs)
    print(f"\n{len(funcs)} .pdata functions, {sum(eh.values())} with exception data")
    for w, c in eh.most_common():
        print(f"  exception data word 0x{w:08X}: {c} functions")

    for label, opcode in (("setjmp (std r14..r31 -> r3)", 62), ("longjmp (ld r14..r31 <- r3)", 58)):
        hits = gpr_run_candidates(img, base, code, opcode)
        print(f"\n{label} candidates: {len(hits)}")
        for h in hits:
            print(f"  {h:08X} {owner(funcs, starts, h)}")

    if write:
        if ambiguous:
            sys.exit(f"not writing: more than one match for {', '.join(ambiguous)}")
        write_config(write, helpers, eh)


if __name__ == "__main__":
    main()
