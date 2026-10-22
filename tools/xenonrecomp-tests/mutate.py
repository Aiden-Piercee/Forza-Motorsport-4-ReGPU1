"""Sanity check of the harness itself: plant typical emitter bugs in the
generated code (after run_tests.sh) and make sure the harness flags each one."""
import re, glob, subprocess, shutil, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
B = os.environ.get("B", HERE + "/../../out/xenonrecomp-tests")
XR = os.environ.get("XR", HERE + "/../../thirdparty/XenonRecomp")
FLAGS = ["-std=c++20", "-O1", "-msse4.1", "-fno-strict-aliasing", "-include", HERE + "/ppc_hooks_test.h",
         "-I" + B + "/out", "-I" + B, "-I" + XR + "/thirdparty/simde", "-Wno-unused-label"]

addr = {}
for line in open(B + "/test_funcs.h"):
    m = re.match(r"#define T_(\S+) sub_([0-9A-F]+)", line)
    if m: addr[m.group(1)] = m.group(2)

# (test function, old text, new text, harness group that must fail)
mutations = [
    ("bdnzf", "!ctx.cr2.lt", "!ctx.cr2.eq", "bdnzf"),                   # upstream's eq shortcut
    ("bdzf", "ctx.ctr.u32 == 0 &&", "ctx.ctr.u32 != 0 &&", "bdzf"),
    ("bso", "ctx.cr3.so", "ctx.cr3.eq", "bso"),
    ("vpkswss128", "ctx.v100.s32), simde_mm_load_si128((simde__m128i*)ctx.v33", "ctx.v33.s32), simde_mm_load_si128((simde__m128i*)ctx.v100", "vpkswss128"),
    ("vpkuhus128", "simde_mm_min_epu16(", "(", "vpkuhus128"),
    ("subfze", "== 0) &", "== 0) |", "subfze"),
    ("addme", "!= 0) |", "!= 0) &", "addme"),
    ("addc", "ctx.r4.u32 + ctx.r5.u32 < ctx.r4.u32", "ctx.r4.u64 + ctx.r5.u64 < ctx.r4.u64", "addc"),
    ("vsrah", ".s16[3] = ctx.v4.s16[3]", ".s16[3] = ctx.v4.u16[3]", "vsrah"),
    ("vslh", "& 0xF);", "& 0x1F);", "vslh"),
    ("lhzu", "ctx.r4.u32 = ea;", "", "lhzu"),
    ("vcmpgtuh_dot", "ctx.cr6.setFromMask", "(void)", "vcmpgtuh."),
    ("vslo128", "& 0x78", "& 0x7", "vslo128"),
    ("vspltishm16", "0xFFF0", "0x0010", "vspltish"),
    ("stfsu", "temp.f32 = float(ctx.f1.f64)", "temp.u32 = uint32_t(ctx.f1.u64)", "stfsu"),
    ("rldicl_dot", "compare<int64_t>(ctx.r3.s64", "compare<int32_t>(ctx.r3.s32", "rldicl."),
    ("crorc", "| !ctx.cr7.gt", "| ctx.cr7.gt", "crorc"),
    ("stvrxl128", "- i - 1", "+ i", "stvrxl128"),
    ("vcfpuxws128_3", "* 8.0", "* 4.0", "vcfpuxws128"),
    ("vsldoi128_mullhwu", ", 4));", ", 3));", "vsldoi128 (decoded as mullhwu.)"),
]

src_files = glob.glob(B + "/out/ppc_recomp.*.cpp")
objs = [f + ".o" for f in src_files]
bad = 0
for fn, old, new, group in mutations:
    a = addr[fn]
    for f in src_files:
        text = open(f).read()
        m = re.search(r"PPC_FUNC_IMPL\(__imp__sub_%s\) \{.*?\n\}\n" % a, text, re.S)
        if m: break
    body = m.group(0)
    if old not in body:
        print(f"[{fn}] mutation text not found: {old}"); bad += 1; continue
    mutated = text.replace(body, body.replace(old, new, 1))
    tmp = B + "/mut.cpp"
    open(tmp, "w").write(mutated)
    subprocess.check_call(["clang++", *FLAGS, "-c", tmp, "-o", tmp + ".o"])
    link = [o if o != f + ".o" else tmp + ".o" for o in objs]
    subprocess.check_call(["clang++", *link, B + "/harness.o", "-o", B + "/harness_mut"])
    out = subprocess.run([B + "/harness_mut"], capture_output=True, text=True).stdout
    caught = re.search(r"^%s\s+FAIL" % re.escape(group), out, re.M) is not None
    print(f"{'caught ' if caught else 'MISSED '} {fn}: '{old}' -> '{new}'")
    bad += not caught
print("all mutations caught" if not bad else f"{bad} mutations missed")
sys.exit(1 if bad else 0)
