#!/usr/bin/env python3
"""Rewrite guest addresses for a title-updated executable, using a map from xexmap.py.

  xexremap.py source <map.json> <base.bin> <new.bin> <in> <out>
        copy a C++ file, translating 0x82xxxxxx and sub_82xxxxxx literals
  xexremap.py scan <map.json> <base.bin> <new.bin> <src dir>
        list the C++ files under <src dir> that hold guest addresses in the executable's range
  xexremap.py switches <map.json> <base.bin> <new.bin> <base_switches.toml> <new_switches.toml> <out.toml>
        copy XenonAnalyse's switch tables for the new image and add the base tables it missed
        (only where the table and all its labels map, so the code around it is unchanged)
  xexremap.py config <map.json> <base.bin> <new.bin> <in.toml> <out.toml> key=value...
        the same for a XenonRecomp config; key=value pairs replace [main] entries
        (file_path, patch_file_path, patched_file_path, out_directory_path, ...)

Literals outside the executable's address range are left alone. A literal the map cannot
place is left unchanged and reported on stderr (the build must not use it as is).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xexmap

LITERAL = re.compile(r'(?<![0-9A-Za-z])(0x|sub_)(8[0-9A-Fa-f]{7})\b')


def make_translate(map_path, base_bin, new_bin):
    m = xexmap.Map(map_path, base_bin, new_bin)
    base = m.sections['base']
    lo = min(v[0] for k, v in base.items() if k != '_code')
    hi = max(v[0] + v[1] for k, v in base.items() if k != '_code')
    problems = []

    def sub(match, where=''):
        prefix, digits = match.group(1), match.group(2)
        addr = int(digits, 16)
        if not (lo <= addr < hi):
            return match.group(0)
        new, how = m.lookup(addr)
        if new is None:
            problems.append((where, addr, how))
            return match.group(0)
        text = '%08X' % new if digits.isupper() or prefix == 'sub_' else '%08x' % new
        return prefix + text

    return sub, problems


def translate_file(sub, problems, src, dst, skip_comments):
    out = []
    for n, line in enumerate(open(src, encoding='utf-8', errors='surrogateescape'), 1):
        code, hash_, comment = (line, '', '')
        if skip_comments and '#' in line:
            code, comment = line.split('#', 1)
            hash_ = '#'
        code = LITERAL.sub(lambda mt: sub(mt, '%s:%d' % (src, n)), code)
        out.append(code + hash_ + comment)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
        f.write(''.join(out))


def parse_switches(path):
    out = {}
    for blk in open(path).read().split('[[switch]]')[1:]:
        base = int(re.search(r'base = (0x[0-9A-Fa-f]+)', blk).group(1), 16)
        r = int(re.search(r'\nr = (\d+)', blk).group(1))
        default = int(re.search(r'default = (0x[0-9A-Fa-f]+)', blk).group(1), 16)
        labels = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', blk.split('labels = [')[1])]
        out[base] = (r, default, labels)
    return out


def switches(args):
    map_path, base_bin, new_bin, base_toml, new_toml, out_toml = args
    m = xexmap.Map(map_path, base_bin, new_bin)
    base, new = parse_switches(base_toml), parse_switches(new_toml)
    text = open(new_toml).read().rstrip('\n') + '\n'
    added = 0
    for b, (r, default, labels) in sorted(base.items()):
        nb, how = m.lookup(b)
        if nb is None or nb in new or how not in ('exact', 'aligned'):
            continue
        nd = m.lookup(default)[0]
        nl = [m.lookup(x)[0] for x in labels]
        if nd is None or any(x is None for x in nl):
            print('skipped %08X: labels do not all map' % b, file=sys.stderr)
            continue
        entries = '\n'.join('    0x%08X,' % x for x in nl)
        text += '[[switch]]\nbase = 0x%08X\nr = %d\ndefault = 0x%08X\nlabels = [\n%s\n]\n' % (nb, r, nd, entries)
        print('added switch table %08X (from base %08X, %d labels)' % (nb, b, len(nl)))
        added += 1
    open(out_toml, 'w', newline='').write(text)
    print('%d added' % added)
    return 0


def scan(args):
    map_path, base_bin, new_bin, src_dir = args
    m = xexmap.Map(map_path, base_bin, new_bin)
    base = m.sections['base']
    lo = min(v[0] for k, v in base.items() if k != '_code')
    hi = max(v[0] + v[1] for k, v in base.items() if k != '_code')
    for root, _, names in os.walk(src_dir):
        for name in sorted(names):
            if not name.endswith(('.cpp', '.h')):
                continue
            path = os.path.join(root, name)
            text = open(path, encoding='utf-8', errors='replace').read()
            if any(lo <= int(mt.group(2), 16) < hi for mt in LITERAL.finditer(text)):
                print(os.path.relpath(path, src_dir).replace(os.sep, '/'))
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == 'scan':
        return scan(sys.argv[2:6])
    if len(sys.argv) >= 2 and sys.argv[1] == 'switches':
        return switches(sys.argv[2:8])
    if len(sys.argv) < 7 or sys.argv[1] not in ('source', 'config'):
        print(__doc__)
        return 1
    mode, map_path, base_bin, new_bin, src, dst = sys.argv[1:7]
    sub, problems = make_translate(map_path, base_bin, new_bin)
    translate_file(sub, problems, src, dst, skip_comments=(mode == 'config'))
    if mode == 'config':
        text = open(dst, encoding='utf-8').read()
        for kv in sys.argv[7:]:
            key, value = kv.split('=', 1)
            line = '%s = "%s"' % (key, value)
            if re.search(r'^%s\s*=.*$' % re.escape(key), text, re.M):
                text = re.sub(r'^%s\s*=.*$' % re.escape(key), lambda _: line, text, flags=re.M)
            else:
                text = text.replace('[main]\n', '[main]\n' + line + '\n', 1)
        open(dst, 'w', encoding='utf-8', newline='').write(text)
    for where, addr, how in problems:
        print('UNMAPPED %08X (%s) at %s' % (addr, how, where), file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
