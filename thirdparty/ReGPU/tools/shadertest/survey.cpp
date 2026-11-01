// Translates a directory of shaders (<vs|ps>_<hash>.ucode, as tools/extractshaders.py
// writes them) and reports what they use and what fails:
//     survey <ucode dir> [hlsl out dir]
// With an output directory every translation is written there as <name>.hlsl, for
// compiling in bulk (run.sh survey compiles them with dxc).
#include "gpu/hlsl.h"
#include "gpu/ucode.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace regpu;
using namespace regpu::ucode;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "survey <ucode dir> [hlsl out dir]\n");
        return 2;
    }
    const std::string outDir = argc > 2 ? argv[2] : "";
    if (!outDir.empty())
        fs::create_directories(outDir);
    std::map<std::string, int> uses, errors;
    int total = 0, ok = 0;
    for (const auto& entry : fs::directory_iterator(argv[1]))
    {
        const std::string name = entry.path().stem().string();
        if (entry.path().extension() != ".ucode" || name.size() < 3)
            continue;
        const bool pixel = name.compare(0, 3, "ps_") == 0;
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
        std::vector<uint32_t> words(bytes.size() / 4);
        memcpy(words.data(), bytes.data(), words.size() * 4);
        words.resize(UsedDwords(words.data(), uint32_t(words.size())));
        total++;
        const Shader s = Parse(words.data(), uint32_t(words.size()), pixel);
        // What the program uses, counted once per shader.
        std::map<std::string, bool> seen;
        auto use = [&](const std::string& what) { seen[std::string(pixel ? "ps " : "vs ") + what] = true; };
        for (const Cf& cf : s.cf)
        {
            switch (cf.op())
            {
            case CfOp::LoopStart: use(cf.isRepeat() ? "loop (repeat)" : "loop"); break;
            case CfOp::LoopEnd: if (cf.isPredicatedBreak()) use("loop predicated break"); break;
            case CfOp::CondCall: use("call"); break;
            case CfOp::CondJmp: use("jump"); break;
            case CfOp::CondExecPred: case CfOp::CondExecPredEnd: use("predicated exec"); break;
            default: break;
            }
            if (!cf.IsExec())
                continue;
            for (uint32_t k = 0; k < cf.count(); k++)
            {
                const uint32_t index = cf.address() + k;
                if ((index + 1) * 3 > s.words.size())
                    break;
                const uint32_t* w = &s.words[index * 3];
                if ((cf.sequence() >> (2 * k)) & 1)
                {
                    const Fetch fe = DecodeFetch(w);
                    if (fe.opcode == 0)
                        use("vfetch format " + std::to_string(fe.format) + (fe.isSigned ? " signed" : "") + (fe.signedNoZero ? " nozero" : ""));
                    else if (fe.opcode == 1)
                    {
                        static const char* dims[] = { "1D", "2D", "3D/stacked", "cube" };
                        use(std::string("tfetch ") + dims[fe.dimension & 3]);
                        if (fe.useRegisterLod) use("tfetch register LOD");
                        if (fe.useRegisterGradients) use("tfetch register gradients");
                        if (!fe.useComputedLod) use("tfetch explicit LOD");
                        if (fe.magFilter != 3 || fe.minFilter != 3) use("tfetch filter override");
                        if (fe.offsetX != 0 || fe.offsetY != 0) use("tfetch texel offset");
                    }
                    else
                        use("fetch opcode " + std::to_string(fe.opcode));
                    if (fe.srcRelative || fe.destRelative) use("fetch aL-relative register");
                }
                else
                {
                    const Alu a = DecodeAlu(w);
                    if (a.exportData && a.vectorDest >= 32 && a.vectorDest <= 37) use("memory export");
                    if (a.exportData && pixel && a.vectorDest == 61) use("oDepth");
                    if (a.exportData && pixel && a.vectorDest >= 1 && a.vectorDest < 4) use("multiple render targets");
                    if (a.vectorDestRelative || (!a.exportData && a.scalarDestRelative)) use("aL-relative destination");
                    for (const Source& src : a.src)
                        if (src.tempRelative) use("aL-relative temporary");
                }
            }
        }
        for (const auto& [what, _] : seen)
            uses[what]++;
        const TranslatedShader t = TranslateShader(s);
        if (!t.ok)
        {
            errors[t.error]++;
            continue;
        }
        ok++;
        if (!outDir.empty())
        {
            std::ofstream o(outDir + "/" + name + ".hlsl", std::ios::binary);
            o << t.hlsl;
        }
    }
    printf("%d shaders, %d translated, %d failed\n\nFeatures (shaders using each):\n", total, ok, total - ok);
    for (const auto& [what, n] : uses)
        printf("  %6d  %s\n", n, what.c_str());
    if (!errors.empty())
    {
        printf("\nTranslation errors:\n");
        for (const auto& [what, n] : errors)
            printf("  %6d  %s\n", n, what.c_str());
    }
    return 0;
}
