// dis <default.xex> <hex address> <count>: disassemble guest code.
#include <cstdio>
#include <cstdlib>
#include <file.h>
#include <image.h>
#include <disasm.h>
#include <byteswap.h>

int main(int argc, char** argv)
{
    if (argc < 4)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    uint32_t addr = std::strtoul(argv[2], nullptr, 16);
    const int count = std::atoi(argv[3]);
    for (int i = 0; i < count; i++, addr += 4)
    {
        auto* code = (const uint32_t*)image.Find(addr);
        ppc_insn insn;
        ppc::Disassemble(code, addr, insn);
        std::printf("%08X  %08X  %s %s\n", addr, ByteSwap(*code), insn.opcode ? insn.opcode->name : "?", insn.op_str);
    }
}
