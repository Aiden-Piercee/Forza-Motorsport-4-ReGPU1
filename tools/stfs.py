#!/usr/bin/env python3
"""Read and extract Xbox 360 STFS packages (PIRS / LIVE / CON): DLC, title updates, saves.

  stfs.py info <package>             header: type, title id, names, files
  stfs.py extract <package> <dir>    write the package's files under <dir>
  stfs.py update <package> <dir>     unpack a title update (default.xexp, Media.zip) into <dir>
  stfs.py install <package> <dlcdir> unpack under <dlcdir>/<package file name> and record its
                                     display name (<name>.title, UTF-16 LE) for the game's list

Layout (Free60 wiki): a header of 0xA000 bytes (its last 0x1000 hash-table block belongs to
the data area), then 0x1000-byte blocks. Every 0xAA data blocks a level-0 hash table block
precedes them, every 0xAA*0xAA a level-1 one, then one level-2. Read-only packages (PIRS,
LIVE) use one block per table, CON packages two (primary and backup).
"""
import struct
import sys
import os

BLOCK = 0x1000
L0 = 0xAA
L1 = 0xAA * 0xAA


class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.magic = self.f.read(4)
        if self.magic not in (b'PIRS', b'LIVE', b'CON '):
            raise ValueError('not an STFS package: %r' % self.magic)
        self.sep = 2 if self.magic == b'CON ' else 1
        self.f.seek(0x344)
        (self.content_type,) = struct.unpack('>I', self.f.read(4))
        self.f.seek(0x360)
        (self.title_id,) = struct.unpack('>I', self.f.read(4))
        self.f.seek(0x340)
        (header_size,) = struct.unpack('>I', self.f.read(4))
        self.first = (header_size + 0xFFF) & ~0xFFF
        # Volume descriptor at 0x379.
        self.f.seek(0x379)
        vd = self.f.read(0x24)
        self.table_blocks = struct.unpack('<H', vd[3:5])[0]
        self.table_start = vd[5] | vd[6] << 8 | vd[7] << 16
        self.total_blocks, self.free_blocks = struct.unpack('>II', vd[0x1C:0x24])
        # License entries at 0x22C (16 x 16 bytes: licensee id, bits, flags): the mask the
        # console reports to the title for this package.
        self.f.seek(0x22C)
        self.license_mask = 0
        for _ in range(16):
            lic = self.f.read(0x10)
            self.license_mask |= struct.unpack('>I', lic[8:12])[0]
        self.f.seek(0x411)
        self.display_name = self.f.read(0x80).decode('utf-16-be', 'ignore').rstrip('\0')
        self.f.seek(0x1691)
        self.title_name = self.f.read(0x80).decode('utf-16-be', 'ignore').rstrip('\0')

    def _backing(self, block):
        n = block + self.sep * (block // L0 + 1)
        if block >= L0:
            n += self.sep * (block // L1 + 1)
        if block >= L1:
            n += self.sep
        return n

    def block_offset(self, block):
        return self.first + (self._backing(block) - 1) * BLOCK + BLOCK if False else self.first + self._backing(block) * BLOCK - BLOCK + BLOCK

    def read_block(self, block):
        self.f.seek(self.first + (self._backing(block) - 1) * BLOCK + BLOCK)
        return self.f.read(BLOCK)

    def _hash_entry_next(self, block):
        # Level-0 table of this block's group: just before the group's first data block.
        group = block // L0
        first_in_group = group * L0
        table_backing = self._backing(first_in_group) - self.sep
        off = self.first + (table_backing - 1) * BLOCK + BLOCK + (block % L0) * 0x18
        self.f.seek(off)
        entry = self.f.read(0x18)
        return entry[0x15] << 16 | entry[0x16] << 8 | entry[0x17]

    def chain(self, start, count, consecutive):
        if consecutive:
            return list(range(start, start + count))
        blocks, b = [], start
        while len(blocks) < count and b != 0xFFFFFF:
            blocks.append(b)
            b = self._hash_entry_next(b)
        return blocks

    def files(self):
        # The file table: table_blocks blocks from table_start, chained like any file.
        data = b''.join(self.read_block(b) for b in self.chain(self.table_start, self.table_blocks, False))
        out = []
        for i in range(len(data) // 0x40):
            e = data[i * 0x40:(i + 1) * 0x40]
            flags = e[0x28]
            name_len = flags & 0x3F
            if name_len == 0:
                break
            name = e[:name_len].decode('ascii', 'replace')
            blocks = e[0x2C] | e[0x2D] << 8 | e[0x2E] << 16
            start = e[0x2F] | e[0x30] << 8 | e[0x31] << 16
            parent = struct.unpack('>h', e[0x32:0x34])[0]
            size = struct.unpack('>I', e[0x34:0x38])[0]
            out.append(dict(name=name, directory=bool(flags & 0x80), consecutive=bool(flags & 0x40),
                            blocks=blocks, start=start, parent=parent, size=size))
        return out

    def path_of(self, files, i):
        parts = []
        while i >= 0:
            parts.append(files[i]['name'])
            i = files[i]['parent']
        return '/'.join(reversed(parts))

    def extract(self, dest):
        files = self.files()
        for i, e in enumerate(files):
            target = os.path.join(dest, self.path_of(files, i))
            if e['directory']:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            remaining = e['size']
            with open(target, 'wb') as out:
                for b in self.chain(e['start'], e['blocks'], e['consecutive']):
                    chunk = self.read_block(b)
                    out.write(chunk[:remaining])
                    remaining -= min(remaining, BLOCK)
                    if remaining <= 0:
                        break
        return files


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('info', 'extract', 'install', 'update'):
        print(__doc__)
        return 1
    s = Stfs(sys.argv[2])
    print('%s type %08X title %08X blocks %d  "%s" / "%s"' % (
        s.magic.decode(), s.content_type, s.title_id, s.total_blocks, s.display_name, s.title_name))
    if sys.argv[1] == 'info':
        files = s.files()
        for i, e in enumerate(files):
            print('%s%s  %d bytes' % (s.path_of(files, i), '/' if e['directory'] else '', e['size']))
    elif sys.argv[1] == 'update':
        if s.content_type != 0x000B0000:
            print('not a title update (content type %08X)' % s.content_type)
            return 1
        files = s.extract(sys.argv[3])
        print('title update unpacked: %s' % ', '.join(e['name'] for e in files))
    elif sys.argv[1] == 'install':
        name = os.path.basename(sys.argv[2])
        dest = os.path.join(sys.argv[3], name)
        files = s.extract(dest)
        with open(dest + '.title', 'wb') as t:
            t.write(s.display_name.encode('utf-16-le'))
        with open(dest + '.license', 'w') as t:
            t.write('%08X\n' % s.license_mask)
        print('installed %d entries to %s' % (len(files), dest))
    else:
        files = s.extract(sys.argv[3])
        print('extracted %d entries to %s' % (len(files), sys.argv[3]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
