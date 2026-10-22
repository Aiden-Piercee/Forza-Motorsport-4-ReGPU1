"""Build a test XEX2 holding one function per tested instruction, plus the
XenonRecomp config, the expected disassembly of every word, and a header that
maps test names to the recompiled function symbols.

Every instruction word is encoded by hand from the PowerPC / VMX / VMX128
field layouts below, and the expected disassembly is written by hand too;
check_disasm.py compares it with the `// mnemonic operands` comments that
XenonRecomp emits."""
import struct, sys, os

BASE = 0x82000000
TEXT_RVA = 0x1000

# ---- encoders -------------------------------------------------------------
def D(op, rt, ra, d):            return (op << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFF)
def DS(op, rt, ra, d, xo):       return (op << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC) | xo
def X(rt, ra, rb, xo, rc=0):     return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc
def XO(rt, ra, rb, xo, rc=0, oe=0): return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo << 1) | rc
def XL(bt, ba, bb, xo, lk=0):    return (19 << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1) | lk
def B(bo, bi, bd):               return (16 << 26) | (bo << 21) | (bi << 16) | (bd & 0xFFFC)
def MD(rs, ra, sh, mb, xo, rc):  # rldic* : mb/me 6-bit field is stored rotated (mb[0:4] || mb[5])
    mbf = ((mb & 0x1F) << 1) | (mb >> 5)
    return (30 << 26) | (rs << 21) | (ra << 16) | ((sh & 0x1F) << 11) | (mbf << 5) | (xo << 2) | (((sh >> 5) & 1) << 1) | rc
def M(op, rs, ra, sh, mb, me, rc): return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc
def VX(vd, va, vb, xo):          return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo
def VXR(vd, va, vb, xo, rc):     return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (rc << 10) | xo
# VMX128: VD128 = word[2:3]<<5 | word[21:25]; VA128 = word[10]<<6 | word[5]<<5 | word[16:20];
# VB128 = word[0:1]<<5 | word[11:15]   (bit numbers are LSB-0 here)
def vd128(v): return ((v & 0x1F) << 21) | (((v >> 5) & 3) << 2)
def va128(v): return ((v & 0x1F) << 16) | (((v >> 5) & 1) << 5) | (((v >> 6) & 1) << 10)
def vb128(v): return ((v & 0x1F) << 11) | ((v >> 5) & 3)
def VX128(vd, va, vb, xop):      return (5 << 26) | (xop & 0x3D0) | vd128(vd) | va128(va) | vb128(vb)
def VX128_1(vd, ra, rb, xop):    return (4 << 26) | (xop & 0x7F3) | vd128(vd) | (ra << 16) | (rb << 11)
def VX128_3(vd, vb, uimm, xop):  return (6 << 26) | (xop & 0x7F0) | vd128(vd) | vb128(vb) | (uimm << 16)

BLR = (0x4E800020, "blr")
def li(rt, v): return (D(14, rt, 0, v), f"li r{rt},{v}")

CRB = ["lt", "gt", "eq", "so"]
def crb(bit):
    return CRB[bit & 3] if bit < 4 else f"4*cr{bit >> 2}+{CRB[bit & 3]}"

# ---- tests ----------------------------------------------------------------
# a test is (name, [items]); an item is (word, expected) or a callable
# (pc, addr_of) -> (word, expected) for branches.
tests = []
def T(name, *items): tests.append((name, list(items)))

def br(bo, bi, target_label, mnem, biop):
    """conditional branch to a label: ('@n' = n instructions into this function, or a test name)"""
    def f(pc, addr_of, fstart):
        tgt = fstart + 4 * int(target_label[1:]) if target_label.startswith("@") else addr_of[target_label]
        ops = (biop + "," if biop else "") + f"0x{tgt:x}"
        return (B(bo, bi, tgt - pc), f"{mnem} {ops}")
    return f

def two_way(name, branch):
    # branch at 0; fallthrough r3=1; target (@3) r3=2
    T(name, branch, li(3, 1), BLR, li(3, 2), BLR)

T("entry", BLR)  # the entry point is renamed _xstart; keep tests off it

# --- update-form loads/stores (D: rT,d(rA); X: rT,rA,rB)
T("lbzu",   (D(35, 3, 4, 5), "lbzu r3,5(r4)"), BLR)
T("lwzu",   (D(33, 3, 4, -12), "lwzu r3,-12(r4)"), BLR)
T("ldu",    (DS(58, 3, 4, 24, 1), "ldu r3,24(r4)"), BLR)
T("stbu",   (D(39, 3, 4, 3), "stbu r3,3(r4)"), BLR)
T("stwu",   (D(37, 3, 4, -16), "stwu r3,-16(r4)"), BLR)
T("stwux",  (X(3, 4, 5, 183), "stwux r3,r4,r5"), BLR)
T("stdu",   (DS(62, 3, 4, -8, 1), "stdu r3,-8(r4)"), BLR)
T("lbzux",  (X(3, 4, 5, 119), "lbzux r3,r4,r5"), BLR)
T("lhzu",   (D(41, 3, 4, -2), "lhzu r3,-2(r4)"), BLR)
T("lhzux",  (X(3, 4, 5, 311), "lhzux r3,r4,r5"), BLR)
T("lhau",   (D(43, 3, 4, 6), "lhau r3,6(r4)"), BLR)
T("lhaux",  (X(3, 4, 5, 375), "lhaux r3,r4,r5"), BLR)
T("lwzux",  (X(3, 4, 5, 55), "lwzux r3,r4,r5"), BLR)
T("ldux",   (X(3, 4, 5, 53), "ldux r3,r4,r5"), BLR)
T("lfsu",   (D(49, 1, 4, 8), "lfsu f1,8(r4)"), BLR)
T("lfsux",  (X(1, 4, 5, 567), "lfsux f1,r4,r5"), BLR)
T("lfdu",   (D(51, 1, 4, -8), "lfdu f1,-8(r4)"), BLR)
T("lfdux",  (X(1, 4, 5, 631), "lfdux f1,r4,r5"), BLR)
T("stbux",  (X(3, 4, 5, 247), "stbux r3,r4,r5"), BLR)
T("sthu",   (D(45, 3, 4, 10), "sthu r3,10(r4)"), BLR)
T("sthux",  (X(3, 4, 5, 439), "sthux r3,r4,r5"), BLR)
T("stdux",  (X(3, 4, 5, 181), "stdux r3,r4,r5"), BLR)
T("stfsu",  (D(53, 1, 4, -4), "stfsu f1,-4(r4)"), BLR)
T("stfsux", (X(1, 4, 5, 695), "stfsux f1,r4,r5"), BLR)
T("stfdu",  (D(55, 1, 4, 16), "stfdu f1,16(r4)"), BLR)
T("stfdux", (X(1, 4, 5, 759), "stfdux f1,r4,r5"), BLR)
T("lhbrx",  (X(3, 4, 5, 790), "lhbrx r3,r4,r5"), BLR)
T("lhbrx0", (X(3, 0, 5, 790), "lhbrx r3,0,r5"), BLR)

# --- branches
two_way("bdzf",  br(0x02, 6, "@3", "bdzf", crb(6)))           # 4*cr1+eq
two_way("bdzf0", br(0x02, 0, "@3", "bdzf", crb(0)))           # lt
two_way("bdzt",  br(0x0A, 9, "@3", "bdzt", crb(9)))           # 4*cr2+gt
two_way("bdnzf", br(0x00, 8, "@3", "bdnzf", crb(8)))          # 4*cr2+lt (not eq)
two_way("bdnzt", br(0x08, 31, "@3", "bdnzt", crb(31)))        # 4*cr7+so
two_way("bso",   br(0x0C, 4 * 3 + 3, "@3", "bso", "cr3"))
two_way("bso0",  br(0x0C, 3, "@3", "bso", ""))
two_way("bns",   br(0x04, 4 * 5 + 3, "@3", "bns", "cr5"))
T("farget", li(3, 7), BLR)
T("bdzf_far", br(0x02, 5, "farget", "bdzf", crb(5)), li(3, 1), BLR)
T("bso_far",  br(0x0C, 7, "farget", "bso", "cr1"), li(3, 1), BLR)
T("bsolr",   (XL(0x0C, 4 * 2 + 3, 0, 16), "bsolr cr2"), li(3, 1), BLR)
T("bnslr",   (XL(0x04, 4 * 6 + 3, 0, 16), "bnslr cr6"), li(3, 1), BLR)
T("bdnzlr",  (XL(0x10, 0, 0, 16), "bdnzlr"), li(3, 1), BLR)
T("bdzlr",   (XL(0x12, 0, 0, 16), "bdzlr"), li(3, 1), BLR)
T("bdzflr",  (XL(0x02, 14, 0, 16), "bdzflr " + crb(14)), li(3, 1), BLR)
T("bdztlr",  (XL(0x0A, 1, 0, 16), "bdztlr " + crb(1)), li(3, 1), BLR)
T("bdnzflr", (XL(0x00, 19, 0, 16), "bdnzflr " + crb(19)), li(3, 1), BLR)
T("bdnztlr", (XL(0x08, 26, 0, 16), "bdnztlr " + crb(26)), li(3, 1), BLR)
T("bsoctr",  (XL(0x0C, 4 * 1 + 3, 0, 528), "bsoctr cr1"), li(3, 1), BLR)
T("bnsctr",  (XL(0x04, 4 * 4 + 3, 0, 528), "bnsctr cr4"), li(3, 1), BLR)

# --- CR logical: bt = 4*cr1+so (7), ba = 4*cr2+lt (8), bb = 4*cr7+gt (29)
for nm, xo in [("crand", 257), ("crandc", 129), ("creqv", 289), ("crnand", 225),
               ("crnor", 33), ("cror", 449), ("crorc", 417), ("crxor", 193)]:
    T(nm, (XL(7, 8, 29, xo), f"{nm} {crb(7)},{crb(8)},{crb(29)}"), BLR)
T("crset",  (XL(7, 7, 7, 289), f"crset {crb(7)}"), BLR)
T("crclr",  (XL(7, 7, 7, 193), f"crclr {crb(7)}"), BLR)
T("crmove", (XL(7, 8, 8, 449), f"crmove {crb(7)},{crb(8)}"), BLR)
T("crnot",  (XL(7, 8, 8, 33), f"crnot {crb(7)},{crb(8)}"), BLR)

# --- carry arithmetic (rD=r3, rA=r4, rB=r5)
T("addc",    (XO(3, 4, 5, 10), "addc r3,r4,r5"), BLR)
T("addc.",   (XO(3, 4, 5, 10, 1), "addc. r3,r4,r5"), BLR)
T("addc_alias", (XO(4, 4, 5, 10), "addc r4,r4,r5"), BLR)
T("addme",   (XO(3, 4, 0, 234), "addme r3,r4"), BLR)
T("addme.",  (XO(3, 4, 0, 234, 1), "addme. r3,r4"), BLR)
T("subfme",  (XO(3, 4, 0, 232), "subfme r3,r4"), BLR)
T("subfme.", (XO(3, 4, 0, 232, 1), "subfme. r3,r4"), BLR)
T("subfze",  (XO(3, 4, 0, 200), "subfze r3,r4"), BLR)
T("subfze.", (XO(3, 4, 0, 200, 1), "subfze. r3,r4"), BLR)
T("subfze_alias", (XO(4, 4, 0, 200), "subfze r4,r4"), BLR)
T("mulhd",   (XO(3, 4, 5, 73), "mulhd r3,r4,r5"), BLR)
T("mulhd.",  (XO(3, 4, 5, 73, 1), "mulhd. r3,r4,r5"), BLR)
T("mulhw.",  (XO(3, 4, 5, 75, 1), "mulhw. r3,r4,r5"), BLR)
T("mulld.",  (XO(3, 4, 5, 233, 1), "mulld. r3,r4,r5"), BLR)
# --- rotates with Rc
T("rldicl.", (MD(4, 3, 12, 20, 0, 1), "rldicl. r3,r4,12,20"), BLR)
T("rldicl.63", (MD(4, 3, 63, 1, 0, 1), "rldicl. r3,r4,63,1"), BLR)
T("rldicr.", (MD(4, 3, 40, 47, 1, 1), "rldicr. r3,r4,40,47"), BLR)
T("rldimi.", (MD(4, 3, 16, 32, 3, 1), "rldimi. r3,r4,16,32"), BLR)
T("clrldi.", (MD(4, 3, 0, 33, 0, 1), "clrldi. r3,r4,33"), BLR)
T("rotldi.", (MD(4, 3, 8, 0, 0, 1), "rotldi. r3,r4,8"), BLR)
T("rlwimi.", (M(20, 4, 3, 8, 4, 19, 1), "rlwimi. r3,r4,8,4,19"), BLR)

# --- VMX (vD=v3, vA=v4, vB=v5)
for nm, xo in [("vslh", 324), ("vsrh", 580), ("vsrah", 836), ("vsrab", 772), ("vrlh", 68),
               ("vsubshs", 1856), ("vsubuhs", 1600), ("vsububm", 1024), ("vsubsbs", 1792),
               ("vadduhs", 576), ("vaddshs", 832), ("vmaxsh", 322), ("vminsh", 834), ("vmaxuh", 66),
               ("vminuh", 578), ("vavguh", 1090), ("vnor", 1284), ("vpkshss", 398), ("vpkswss", 462),
               ("vpkswus", 334), ("vpkuhus", 142), ("vpkuwus", 206), ("vpkuhum", 14), ("vpkuwum", 78)]:
    T(nm, (VX(3, 4, 5, xo), f"{nm} v3,v4,v5"), BLR)
T("vslh_alias", (VX(4, 4, 4, 324), "vslh v4,v4,v4"), BLR)
for simm in (-16, -1, 0, 1, 15):
    T(f"vspltish{simm}", (VX(3, simm & 0x1F, 0, 844), f"vspltish v3,{simm}"), BLR)
for nm, xo in [("vcmpequh", 70), ("vcmpgtsh", 838), ("vcmpgtuh", 582), ("vcmpgtsw", 902),
               ("vcmpgtuw", 646), ("vcmpgtsb", 774), ("vcmpgtub", 518)]:
    T(nm, (VXR(3, 4, 5, xo, 0), f"{nm} v3,v4,v5"), BLR)
    T(nm + ".", (VXR(3, 4, 5, xo, 1), f"{nm}. v3,v4,v5"), BLR)

# --- VMX128 (vD=v70, vA=v33, vB=v100 to exercise every register-number bit)
for nm, xop in [("vpkswss128", 640), ("vpkswus128", 704), ("vpkshss128", 512), ("vpkshus128", 576),
                ("vpkuhus128", 832), ("vpkuwus128", 960), ("vpkuwum128", 896), ("vpkuhum128", 768),
                ("vnor128", 656), ("vslo128", 912), ("vsro128", 976)]:
    T(nm, (VX128(70, 33, 100, xop), f"{nm} v70,v33,v100"), BLR)
# vsldoi128: opcode 4 with bit 4 set; SHB in bits 6-9. The last three collide with the
# PPC405 mullhwu. / mulhhwu / macchw table entries and must still come out as vsldoi128.
def VSLDOI128(vd, va, vb, sh): return (4 << 26) | 0x10 | (sh << 6) | vd128(vd) | va128(va) | vb128(vb)
T("vsldoi128", (VSLDOI128(70, 33, 100, 5), "vsldoi128 v70,v33,v100,5"), BLR)
T("vsldoi128_mullhwu", (VSLDOI128(3, 4, 40, 12), "vsldoi128 v3,v4,v40,12"), BLR)
T("vsldoi128_mulhhwu", (VSLDOI128(3, 4, 5, 0), "vsldoi128 v3,v4,v5,0"), BLR)
T("vsldoi128_macchw", (VSLDOI128(67, 4, 5, 5), "vsldoi128 v67,v4,v5,5"), BLR)
T("vslo", (VX(3, 4, 5, 1036), "vslo v3,v4,v5"), BLR)
T("vsro", (VX(3, 4, 5, 1100), "vsro v3,v4,v5"), BLR)
for u in (0, 3, 31):
    T(f"vcfpuxws128_{u}", (VX128_3(70, 100, u, 624), f"vcfpuxws128 v70,v100,{u}"), BLR)
T("vctuxs", (VX(3, 0, 5, 906) | (2 << 16), "vctuxs v3,v5,2"), BLR)
for nm, xop, s in [("lvxl128", 707, "v101"), ("stvxl128", 963, "v101"), ("lvlxl128", 1539, "v101"),
                   ("lvrxl128", 1603, "v101"), ("stvlxl128", 1795, "v101"), ("stvrxl128", 1859, "v101")]:
    T(nm, (VX128_1(101, 4, 5, xop), f"{nm} {s},r4,r5"), BLR)
for nm, xo in [("lvlxl", 775), ("lvrxl", 807), ("stvlxl", 903), ("stvrxl", 935), ("lvehx", 39), ("lvebx", 7)]:
    T(nm, (X(3, 4, 5, xo), f"{nm} v3,r4,r5"), BLR)

# ---- layout ---------------------------------------------------------------
def build(outdir):
    addr_of, fn_layout = {}, []
    pc = BASE + TEXT_RVA
    for name, items in tests:
        addr_of[name] = pc
        fn_layout.append((name, pc, items))
        pc += 4 * len(items)
    text_size = (pc - (BASE + TEXT_RVA) + 0xFFF) & ~0xFFF
    PDATA_RVA = TEXT_RVA + text_size
    pdata_size = (8 * len(tests) + 0xFFF) & ~0xFFF
    IMG = PDATA_RVA + pdata_size

    img = bytearray(IMG)
    struct.pack_into("<I", img, 0x3C, 0x80)
    img[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", img, 0x84, 0x1F2, 2)
    sh = 0x80 + 248
    def sec(i, name, rva, size, ch):
        o = sh + i * 40
        img[o:o+8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", img, o + 8, size, rva, size, rva)
        struct.pack_into("<I", img, o + 36, ch)
    sec(0, b".text", TEXT_RVA, text_size, 0x60000020)
    sec(1, b".pdata", PDATA_RVA, 8 * len(tests), 0x40000040)

    expect = []
    for i, (name, fstart, items) in enumerate(fn_layout):
        for j, it in enumerate(items):
            a = fstart + 4 * j
            word, exp = it(a, addr_of, fstart) if callable(it) else it
            struct.pack_into(">I", img, a - BASE, word & 0xFFFFFFFF)
            expect.append((a, word & 0xFFFFFFFF, exp))
        struct.pack_into(">II", img, PDATA_RVA + 8 * i, fstart, (len(items) << 8) | 1)

    nopt = 3
    hdr_end = 24 + nopt * 8
    ffi_off = hdr_end; ffi = struct.pack(">IHH", 8, 0, 0)
    sec_off = (ffi_off + len(ffi) + 15) & ~15
    security = bytearray(0x180); struct.pack_into(">II", security, 0, 0x180, IMG); struct.pack_into(">I", security, 0x110, BASE)
    header_size = (sec_off + len(security) + 0xFFF) & ~0xFFF
    h = bytearray(header_size)
    struct.pack_into(">4sIIIII", h, 0, b"XEX2", 0, header_size, 0, sec_off, nopt)
    for i, (k, v) in enumerate([(0x3FF, ffi_off), (0x00010100, BASE + TEXT_RVA), (0x00010201, BASE)]):
        struct.pack_into(">II", h, 24 + 8 * i, k, v)
    h[ffi_off:ffi_off + len(ffi)] = ffi
    h[sec_off:sec_off + len(security)] = security
    os.makedirs(outdir + "/out", exist_ok=True)
    open(outdir + "/default.xex", "wb").write(bytes(h) + bytes(img))
    open(outdir + "/config.toml", "w").write('[main]\nfile_path = "default.xex"\nout_directory_path = "out"\n')
    with open(outdir + "/expected_disasm.txt", "w") as f:
        for a, w, e in expect:
            f.write(f"{a:08X} {w:08X} {e}\n")
    with open(outdir + "/test_funcs.h", "w") as f:
        f.write("#pragma once\n")
        for name, a, _ in fn_layout:
            f.write(f'#define T_{name.replace(".", "_dot").replace("-", "m")} sub_{a:X}\n')
            f.write(f'#define A_{name.replace(".", "_dot").replace("-", "m")} 0x{a:X}u\n')

build(sys.argv[1])
