"""For each jump table, compare its targets with the .pdata function that holds it."""
import bisect, re, struct, sys
BASE = 0x82000000
img = open(sys.argv[1], "rb").read()
PDATA, PSIZE = 0x82176400, 0x4FBC8
funcs = []
for off in range(PDATA - BASE, PDATA - BASE + PSIZE, 8):
    begin, packed = struct.unpack_from(">II", img, off)
    if begin:
        funcs.append((begin, begin + ((packed >> 8) & 0x3FFFFF) * 4))
funcs.sort()
starts = [f[0] for f in funcs]
text = open(sys.argv[2]).read()
bad = set(sys.argv[3:])
for m in re.finditer(r"base = (0x[0-9A-F]+).*?labels = \[(.*?)\]", text, re.S):
    base = int(m.group(1), 16)
    hit = [b for b in bad if 0 <= int(b, 16) - base < 0x40]
    if not hit:
        continue
    labels = [int(x, 16) for x in re.findall(r"0x[0-9A-F]+", m.group(2))]
    i = bisect.bisect_right(starts, base) - 1
    fs, fe = funcs[i]
    nxt = funcs[i + 1][0]
    print(f"{base:08X} pdata fn {fs:08X}-{fe:08X} (next {nxt:08X}) labels {min(labels):08X}..{max(labels):08X}")
print(len(funcs), "pdata functions")
