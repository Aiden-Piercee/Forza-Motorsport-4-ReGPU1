// xenos_dis [--hlsl] <shader.bin>...: disassemble captured Xenos microcode
// (vs_*.bin / ps_*.bin from FM2_DUMP_SHADERS); --hlsl translates and
// compiles each one instead and reports the result.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include <windows.h>

#include "gpu/hlsl.h"
#include "gpu/ucode.h"

int main(int argc, char** argv)
{
    bool hlsl = false, print = false;
    int failures = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--hlsl"))
        {
            hlsl = true;
            continue;
        }
        if (!strcmp(argv[i], "--print"))
        {
            print = true;
            continue;
        }
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<char> bytes{ std::istreambuf_iterator<char>(f), {} };
        const std::string name = argv[i];
        const bool pixel = name.find("ps_") != std::string::npos;
        const auto shader = ucode::Parse(reinterpret_cast<const uint32_t*>(bytes.data()), uint32_t(bytes.size() / 4), pixel);
        if (!hlsl)
        {
            printf("=== %s\n%s\n", argv[i], ucode::Disassemble(shader).c_str());
            continue;
        }
        const TranslatedShader t = TranslateShader(shader);
        if (!t.ok)
        {
            printf("FAIL translate %s: %s\n", argv[i], t.error.c_str());
            failures++;
            continue;
        }
        if (print)
            printf("%s\n", t.hlsl.c_str());
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(t.hlsl.data(), t.hlsl.size(), argv[i], nullptr, nullptr, "main", pixel ? "ps_5_1" : "vs_5_1",
            D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code, &errors);
        if (FAILED(hr))
        {
            printf("FAIL compile %s:\n%s\n", argv[i], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            failures++;
        }
        else
            printf("ok   %s (%zu bytes DXBC)\n", argv[i], code->GetBufferSize());
    }
    return failures;
}
