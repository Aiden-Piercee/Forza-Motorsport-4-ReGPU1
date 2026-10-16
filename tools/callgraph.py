"""Index the recompiled code: callers of each function and the function
that contains each guest address.

  py -3 -I tools/callgraph.py build            -> out/callgraph.json
  py -3 -I tools/callgraph.py callers sub_X     -> functions calling sub_X
  py -3 -I tools/callgraph.py at 8236CD78       -> function containing an address
"""
import glob, json, re, sys
DB = "out/callgraph.json"
if sys.argv[1] == "build":
    callers, starts = {}, {}
    fn = None
    for path in glob.glob("out/ppc/ppc_recomp.*.cpp"):
        for line in open(path):
            m = re.match(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)", line)
            if m:
                fn = m.group(1)
                starts[fn] = None
                continue
            m = re.search(r"\b((?:sub|__imp__)[A-Za-z0-9_]+)\(ctx, base\)", line)
            if m and fn:
                callee = m.group(1).replace("__imp__", "")
                callers.setdefault(callee, set()).add(fn)
            m = re.match(r"\t// .*", line)
    json.dump({k: sorted(v) for k, v in callers.items()}, open(DB, "w"))
    print(len(callers), "callees indexed")
elif sys.argv[1] == "callers":
    print("\n".join(json.load(open(DB)).get(sys.argv[2], [])))
elif sys.argv[1] == "at":
    a = int(sys.argv[2], 16)
    best = None
    for line in open("out/ppc/ppc_func_mapping.cpp"):
        m = re.search(r"0x([0-9A-F]+), (sub_[0-9A-F]+)", line)
        if m and int(m.group(1), 16) <= a:
            best = m.group(2)
    print(best)
