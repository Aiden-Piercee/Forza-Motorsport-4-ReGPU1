"""Xbox 360 disc image (XDVDFS) reader: list files or extract them.

  py -3 tools/xiso.py list    <image.iso>
  py -3 tools/xiso.py extract <image.iso> <out_dir> [name ...]
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game partition starts: plain XDVDFS, XGD2 (redump), XGD3, XGD1.
PARTITION_OFFSETS = (0, 0xFD90000, 0x2080000, 0x18300000)


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("not an Xbox disc image")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    pending = [0]
    seen = set()
    while pending:
        off = pending.pop() * 4
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:
            continue
        name = table[off + 14 : off + 14 + nlen].decode("latin-1")
        path = prefix + name
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, path + "/")
        else:
            yield path, base + start * SECTOR, length
        if left:
            pending.append(left)
        if right:
            pending.append(right)


def files(f):
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + 20)
    root, size = struct.unpack("<II", f.read(8))
    return list(walk(f, base, root, size))


def main():
    cmd, iso = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        entries = files(f)
        if cmd == "list":
            for path, _, length in sorted(entries):
                print(f"{length:>12}  {path}")
            return
        out, wanted = sys.argv[3], {n.lower() for n in sys.argv[4:]}
        for path, pos, length in entries:
            if wanted and path.lower() not in wanted:
                continue
            dst = os.path.join(out, *path.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(pos)
            with open(dst, "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 1 << 24))
                    o.write(chunk)
                    left -= len(chunk)


if __name__ == "__main__":
    main()
