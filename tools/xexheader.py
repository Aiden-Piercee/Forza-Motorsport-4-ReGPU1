"""Print a XEX2's optional headers and its import records (functions vs variables)."""
import struct, sys
d = open(sys.argv[1], "rb").read()
magic, flags, pe_off, _, sec_off, count = struct.unpack_from(">4sIIIII", d, 0)
print(f"flags {flags:#x} pe {pe_off:#x} security {sec_off:#x} headers {count}")
hdrs = {}
for i in range(count):
    key, val = struct.unpack_from(">II", d, 24 + i * 8)
    hdrs[key] = val
    print(f"  {key:08X} = {val:08X}")
load = struct.unpack_from(">I", d, sec_off + 0x110)[0]
print(f"image size {struct.unpack_from('>I', d, sec_off + 4)[0]:#x} load {load:#x}")
if 0x00020104 in hdrs:
    print("tls slots/raw/datasize/rawsize", struct.unpack_from(">IIII", d, hdrs[0x00020104]))
if 0x00020200 in hdrs:
    print("default stack size", hex(hdrs[0x00020200]))
if 0x00020301 in hdrs:
    print("default heap size", hex(hdrs[0x00020301]))
off = hdrs[0x000103FF]
size, strsize, nlib = struct.unpack_from(">III", d, off)
names, p = [], off + 12
for _ in range(nlib):
    e = d.index(b"\0", p)
    names.append(d[p:e].decode())
    p += ((e - p + 1) + 3) & ~3
p = off + 12 + strsize
for _ in range(nlib):
    lsize = struct.unpack_from(">I", d, p)[0]
    nameidx, nrec = struct.unpack_from(">HH", d, p + 0x24)
    recs = struct.unpack_from(f">{nrec}I", d, p + 0x28)
    print(f"lib {names[nameidx]}: {nrec} records")
    print("  records:", " ".join(f"{r:08X}" for r in recs))
    p += lsize
