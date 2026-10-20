"""Compare a dumped video read ring with the file: ringcheck.py log file.bik ring.bin ringStart

For each ring address, the most recent logged read into it says which file
bytes should be there; report slots whose contents differ.
"""
import re
import sys

log, bik, dump, start = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16)
name = bik.replace("\\", "/").split("/")[-1].lower()
data = open(bik, "rb").read()
ring = open(dump, "rb").read()
last = {}
pat = re.compile(r"io: read (\S+) offset (\d+) length (\d+) -> ([0-9A-F]+)")
for line in open(log, errors="replace"):
    m = pat.search(line)
    if not m or m.group(1).lower() != name:
        continue
    offset, length, dest = int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
    if start <= dest < start + len(ring):
        last[dest] = (offset, length)
bad = 0
for dest in sorted(last):
    offset, length = last[dest]
    got = ring[dest - start: dest - start + length]
    want = data[offset: offset + length]
    same = sum(1 for a, b in zip(got, want) if a == b)
    status = "ok" if got == want else "DIFF (%d/%d bytes match)" % (same, len(want))
    if got != want:
        bad += 1
        first = next(i for i, (a, b) in enumerate(zip(got, want)) if a != b)
        status += " first at +%X" % first
    print("%08X <- offset %9d length %6d: %s" % (dest, offset, length, status))
print("%d of %d slots differ" % (bad, len(last)))
