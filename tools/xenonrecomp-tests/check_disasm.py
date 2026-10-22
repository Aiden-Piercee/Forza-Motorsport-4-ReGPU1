"""Compare XenonRecomp's `// mnemonic operands` comments with the hand-written
expected disassembly from gen_tests.py, and fail on any instruction XenonRecomp
did not recognise."""
import re, sys, glob

outdir = sys.argv[1]
expect = {}
for line in open(outdir + "/expected_disasm.txt"):
    a, w, e = line.rstrip("\n").split(" ", 2)
    expect[int(a, 16)] = (w, e)

got = {}
for path in glob.glob(outdir + "/out/ppc_recomp.*.cpp"):
    fn = None
    for line in open(path):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__(?:sub_([0-9A-F]+)|_xstart)\)", line)
        if m:
            fn = int(m.group(1), 16) if m.group(1) else 0x82001000; pc = fn; continue
        m = re.match(r"\t// (.*)$", line)
        if fn is not None and m and not line.startswith("\t// ERROR"):
            got[pc] = m.group(1).strip(); pc += 4

bad = 0
for a, (w, e) in sorted(expect.items()):
    g = got.get(a)
    if g != e:
        print(f"MISMATCH {a:08X} {w}: expected '{e}', got '{g}'"); bad += 1
print(f"disasm: {len(expect) - bad}/{len(expect)} words match")
sys.exit(1 if bad else 0)
