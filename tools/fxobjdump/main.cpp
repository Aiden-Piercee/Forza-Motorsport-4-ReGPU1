// fxobjdump <game dir> <out dir> [filter]
// Copies every zip member whose name contains `filter` (default .fxobj) out of the game's zips,
// as <out dir>/<zip path>/<member>, so tools/staticshaders.py can survey them. Prints how many
// members matched and how many decoded.
#include <cstdio>
#include <fstream>
#include <string>

#include "zip.h"

namespace fs = std::filesystem;

static std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(tolower(c));
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "fxobjdump <game dir> <out dir> [filter]\n");
        return 1;
    }
    const fs::path game = argv[1], outDir = argv[2];
    const std::string filter = argc > 3 ? argv[3] : ".fxobj";
    size_t zips = 0, written = 0;
    for (const auto& e : fs::recursive_directory_iterator(game, fs::directory_options::follow_directory_symlink))
    {
        if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".zip")
            continue;
        std::ifstream in(e.path(), std::ios::binary | std::ios::ate);
        std::vector<uint8_t> zip(size_t(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(zip.data()), std::streamsize(zip.size()));
        zips++;
        const fs::path rel = fs::relative(e.path(), game);
        ForEachZipMember(zip, filter.c_str(), [&](const std::string& name, const std::vector<uint8_t>& data) {
            const fs::path dst = outDir / rel / name;
            fs::create_directories(dst.parent_path());
            std::ofstream(dst, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            written++;
        });
    }
    printf("%zu zips, %zu members written\n", zips, written);
    return 0;
}
