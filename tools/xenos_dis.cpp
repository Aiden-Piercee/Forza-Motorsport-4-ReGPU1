// xenos_dis <shader.bin>...: disassemble captured Xenos microcode
// (vs_*.bin / ps_*.bin from FM2_DUMP_SHADERS).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gpu/ucode.h"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<char> bytes{ std::istreambuf_iterator<char>(f), {} };
        const std::string name = argv[i];
        const bool pixel = name.find("ps_") != std::string::npos;
        const auto shader = ucode::Parse(reinterpret_cast<const uint32_t*>(bytes.data()), uint32_t(bytes.size() / 4), pixel);
        printf("=== %s\n%s\n", argv[i], ucode::Disassemble(shader).c_str());
    }
}
