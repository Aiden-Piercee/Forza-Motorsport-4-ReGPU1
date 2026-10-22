#!/usr/bin/env python3
"""Classify the imports a recompiled XEX calls against what src/ implements.

  importgap.py <names> [<names> ...]    a ppc_recomp_shared.h (PPC_EXTERN_FUNC(__imp__Name)) or
                                        a text file with one import name per line; several files
                                        (default.xex plus the two facade modules) are merged

Each import is one of:
  implemented  a GUEST_EXPORT / PPC_FUNC(__imp__Name) outside src/kernel/stubs.cpp
  stub         only the weak fallback in stubs.cpp: it logs once and returns 0
  missing      no definition at all: the link fails until tools/genstubs.py is re-run

and is assigned to the first milestone that needs it (boot, menus, race, content, online, nui).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# First milestone that can stall on the import, by name prefix; first match wins.
MILESTONES = [
    ("nui", r"^(XamNui|Nui|XnaNui|XamKinect)"),
    ("online", r"^(NetDll_|XNet|XamVoice|XamSession|XamParty|XampXAuth|XamUserCreate(Stats|Achievement|Titles)|XamUserGetUserFlags|XOnline|XamUserReadStats|XamLiveLog|XamQuery)"),
    ("content", r"^(XamContent|Stfs|XamCreateEnumeratorHandle|XamEnumerate|XamShowDeviceSelectorUI|XamUserWrite|XamUserReadProfile|XamReadTile|XamProfile|XamAvatar|XamUserWriteAchievements|XeCrypt|XeKeys)"),
    ("race", r"^(XMA|XAudio|XAM_Audio|Xma)"),
    ("menus", r"^(XamUser|XamShow|XamInput|XNotify|XamNotify|XamTask|XMsg|XamLoader|XGet|XamGet|XamAlloc|XamFree|XNetLogon)"),
    ("boot", r"^(Ke|Nt|Rtl|Mm|Ex|Ob|Xex|Vd|Hal|Io|Fsc|Kf|Interlocked|Dbg|_|sprintf|vs|__C|Mi|Ps|Xe)"),
]
ORDER = ["boot", "menus", "race", "content", "online", "nui", "other"]


def milestone(name):
    for ms, pat in MILESTONES:
        if re.match(pat, name):
            return ms
    return "other"


def read_names(path):
    text = open(path, errors="replace").read()
    found = re.findall(r"PPC_EXTERN_FUNC\(__imp__([A-Za-z_]\w*)\)", text)
    if not found:
        found = [l.strip() for l in text.splitlines() if re.fullmatch(r"[A-Za-z_]\w*", l.strip())]
    return {n for n in found if not n.startswith("sub_")}


def defined_names():
    real, stub = set(), set()
    for dirpath, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if not f.endswith(".cpp"):
                continue
            path = os.path.join(dirpath, f)
            text = open(path, errors="replace").read()
            names = re.findall(r"GUEST_EXPORT\((\w+)\)", text) + re.findall(r"PPC_FUNC\(__imp__(\w+)\)", text)
            (stub if f == "stubs.cpp" else real).update(names)
    return real, stub - real


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    wanted = set()
    for p in sys.argv[1:]:
        wanted |= read_names(p)
    real, stub = defined_names()
    rows = {m: {"implemented": [], "stub": [], "missing": []} for m in ORDER}
    for n in sorted(wanted):
        state = "implemented" if n in real else "stub" if n in stub else "missing"
        rows[milestone(n)][state].append(n)
    print(f"{len(wanted)} imports\n")
    for m in ORDER:
        r = rows[m]
        if not any(r.values()):
            continue
        print(f"## {m}: {len(r['implemented'])} implemented, {len(r['stub'])} stub-only, {len(r['missing'])} missing")
        for state in ("missing", "stub"):
            if r[state]:
                print(f"  {state}: " + " ".join(r[state]))
        print()
    return 0


sys.exit(main())
