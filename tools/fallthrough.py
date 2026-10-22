"""List recompiled functions whose last instruction can fall through, but not
into a function: fallthrough.py out/ppc

XenonRecomp (with patches/xenonrecomp-fm4.patch) makes a function that falls
through into the next function call it and return, which is what the guest does,
and marks it "// falls through into". Those are listed only with --all. The rest
run off their end into padding or data, and each needs an explanation (usually
a call to a function that never returns) or a `functions` entry."""
import glob
import re
import sys

END = {'b', 'blr', 'bctr', 'rfi', 'trap', 'tw', 'twi'}
for path in sorted(glob.glob(sys.argv[1] + '/ppc_recomp.*.cpp')):
    name, last, handled = None, None, False
    for line in open(path, errors='replace'):
        m = re.match(r'PPC_FUNC_IMPL\(__imp__(?:[a-z]+_)?sub_([0-9A-F]+)\)', line)
        if m:
            name, last, handled = m.group(1), None, False
            continue
        if line.startswith('\t// falls through into '):
            handled = True
            continue
        m = re.match(r'\t// ([a-z.+-]+)', line)
        if m:
            last = m.group(1).rstrip('+-')
            continue
        if line.startswith('}') and name:
            if last and last not in END and (not handled or '--all' in sys.argv):
                print(name, last, *(['(continues in the next function)'] if handled else []))
            name = None
