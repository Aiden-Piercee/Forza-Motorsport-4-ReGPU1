"""List recompiled functions whose last instruction can fall through into the
next function (XenonAnalyse split a body): fallthrough.py out/ppc"""
import glob
import re
import sys

END = {'b', 'blr', 'bctr', 'rfi', 'trap', 'tw', 'twi'}
for path in sorted(glob.glob(sys.argv[1] + '/ppc_recomp.*.cpp')):
    name, last = None, None
    for line in open(path, errors='replace'):
        m = re.match(r'PPC_FUNC_IMPL\(__imp__(?:[a-z]+_)?sub_([0-9A-F]+)\)', line)
        if m:
            name, last = m.group(1), None
            continue
        m = re.match(r'\t// ([a-z.+-]+)', line)
        if m:
            last = m.group(1).rstrip('+-')
            continue
        if line.startswith('}') and name:
            if last and last not in END:
                print(name, last)
            name = None
