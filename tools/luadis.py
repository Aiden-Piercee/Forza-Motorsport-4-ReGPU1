"""Lists a compiled Lua 5.1 chunk (the game's .lua files are big-endian luac output).

  luadis.py <file.lua | zip:member>      e.g. luadis.py "C:/.../update/Media.zip:UI/Behaviors/.../P_MAI_Footer.lua"
"""
import struct
import sys
import zipfile

OPS = ['MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL', 'GETTABLE', 'SETGLOBAL', 'SETUPVAL', 'SETTABLE',
       'NEWTABLE', 'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'UNM', 'NOT', 'LEN', 'CONCAT', 'JMP', 'EQ', 'LT', 'LE',
       'TEST', 'TESTSET', 'CALL', 'TAILCALL', 'RETURN', 'FORLOOP', 'FORPREP', 'TFORLOOP', 'SETLIST', 'CLOSE', 'CLOSURE', 'VARARG']
ABX = {'LOADK', 'GETGLOBAL', 'SETGLOBAL', 'CLOSURE'}
ASBX = {'JMP', 'FORLOOP', 'FORPREP'}


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 12
        h = data[:12]
        assert h[:4] == b'\x1bLua' and h[4] == 0x51, 'not Lua 5.1 bytecode'
        self.e = '>' if h[6] == 0 else '<'
        self.st = h[8]

    def u8(self):
        self.p += 1
        return self.d[self.p - 1]

    def i32(self):
        self.p += 4
        return struct.unpack(self.e + 'i', self.d[self.p - 4:self.p])[0]

    def size(self):
        return self.i32() if self.st == 4 else struct.unpack(self.e + 'q', self.take(8))[0]

    def take(self, n):
        self.p += n
        return self.d[self.p - n:self.p]

    def string(self):
        n = self.size()
        return self.take(n)[:-1].decode('latin1') if n else None


def function(r, depth=0):
    pad = '  ' * depth
    src = r.string()
    line, last = r.i32(), r.i32()
    nup, npar, va, stack = r.u8(), r.u8(), r.u8(), r.u8()
    code = [r.i32() & 0xFFFFFFFF for _ in range(r.i32())]
    consts = []
    for _ in range(r.i32()):
        t = r.u8()
        consts.append(None if t == 0 else bool(r.u8()) if t == 1 else struct.unpack(r.e + 'd', r.take(8))[0] if t == 3 else r.string())
    protos = []
    nproto = r.i32()
    print('%sfunction (line %d, %d params, %d upvalues, %d consts)' % (pad, line, npar, nup, len(consts)))

    def rk(x):
        return 'K(%r)' % (consts[x - 256],) if x >= 256 else 'R%d' % x

    for pc, ins in enumerate(code):
        op = OPS[ins & 0x3F]
        a, c, b, bx = (ins >> 6) & 0xFF, (ins >> 14) & 0x1FF, (ins >> 23) & 0x1FF, ins >> 14
        if op in ABX:
            arg = '%d %s' % (a, repr(consts[bx]) if op != 'CLOSURE' else 'proto%d' % bx)
        elif op in ASBX:
            arg = '%d -> %d' % (a, pc + 1 + bx - 131071)
        else:
            arg = '%d %s %s' % (a, rk(b) if op in ('SETTABLE', 'ADD', 'SUB', 'MUL', 'DIV', 'EQ', 'LT', 'LE') else b,
                                rk(c) if op in ('GETTABLE', 'SETTABLE', 'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'EQ', 'LT', 'LE') else c)
        print('%s  %3d %-9s %s' % (pad, pc, op, arg))
    for i in range(nproto):
        print('%s proto%d:' % (pad, i))
        function(r, depth + 1)
    for _ in range(r.i32()):
        r.i32()  # line info
    for _ in range(r.i32()):
        r.string(), r.i32(), r.i32()  # locals
    for _ in range(r.i32()):
        r.string()  # upvalue names


def main(argv):
    path = argv[1]
    if '.zip:' in path:
        zp, member = path.split('.zip:', 1)
        data = zipfile.ZipFile(zp + '.zip').read(member)
    else:
        data = open(path, 'rb').read()
    function(Reader(data))


if __name__ == '__main__':
    main(sys.argv)
