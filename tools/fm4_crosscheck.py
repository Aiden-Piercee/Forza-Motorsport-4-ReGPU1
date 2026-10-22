#!/usr/bin/env python3
"""Compare XenonRecomp's function entries with another function list for the same image.

  fm4_crosscheck.py <out dir> <functions.toml> [--where <hex address>...]

<out dir> is a XenonRecomp output directory (its ppc_func_mapping.cpp lists every
function entry). <functions.toml> is a list made by other tooling, one
`0xADDRESS = { size = ... }  # comment` per line, such as the ReXGlue-era
fm4/fm4_functions.toml of fm4-recomp. It is only read: nothing from it is copied.

Every address that list has as a function start but XenonRecomp does not is an
indirect-call target the dispatch table would miss (a null entry), unless no code
ever calls it indirectly. They are listed by the list's own comment category, with
the XenonRecomp function that holds each one, as candidates to check and, where
the check confirms them, add to the config's `functions` with the reason.

--where prints the XenonRecomp function holding each given address (for hooks).
"""
import bisect
import collections
import re
import sys


def read_mapping(out_dir):
    starts = {}
    for line in open(f"{out_dir}/ppc_func_mapping.cpp"):
        m = re.match(r"\s*\{ 0x([0-9A-F]+), (\w+) \},", line)
        if m and m.group(2) != "nullptr":
            starts[int(m.group(1), 16)] = m.group(2)
    return starts


def read_list(path):
    entries = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*0x([0-9A-Fa-f]+)\s*=\s*\{([^}]*)\}\s*(#.*)?$", line)
        if not m:
            continue
        size = re.search(r"size\s*=\s*0x([0-9A-Fa-f]+)", m.group(2))
        comment = (m.group(3) or "").lstrip("#").strip()
        # The category is the comment's first clause without addresses.
        category = re.sub(r"\s*(from|of|to)?\s*0x[0-9A-Fa-f]+.*$", "", comment.split("#")[0]).strip()
        category = re.sub(r"^auto:\s*", "", category) or "(no comment)"
        entries.append((int(m.group(1), 16), int(size.group(1), 16) if size else 0, category))
    return entries


def holder(sorted_starts, starts, addr):
    k = bisect.bisect_right(sorted_starts, addr) - 1
    if k < 0:
        return "none"
    s = sorted_starts[k]
    return f"{starts[s]} (+0x{addr - s:X})"


def main():
    args = sys.argv[1:]
    where = []
    if "--where" in args:
        k = args.index("--where")
        where = [int(a, 16) for a in args[k + 1:]]
        args = args[:k]
    if len(args) != 2:
        sys.exit(__doc__)
    starts = read_mapping(args[0])
    sorted_starts = sorted(starts)
    entries = read_list(args[1])
    print(f"XenonRecomp: {len(starts)} function entries; {args[1]}: {len(entries)} entries")

    missing = collections.defaultdict(list)
    for addr, size, category in entries:
        if addr not in starts:
            missing[category].append((addr, size))
    total = sum(len(v) for v in missing.values())
    print(f"{total} of its entries are not XenonRecomp function entries\n")
    for category, items in sorted(missing.items(), key=lambda kv: -len(kv[1])):
        print(f"## {category}: {len(items)}")
        for addr, size in sorted(items):
            print(f"  0x{addr:08X} size 0x{size:X} in {holder(sorted_starts, starts, addr)}")
        print()

    if where:
        print("## --where")
        for addr in where:
            print(f"  0x{addr:08X} {'entry ' + starts[addr] if addr in starts else 'in ' + holder(sorted_starts, starts, addr)}")


if __name__ == "__main__":
    main()
