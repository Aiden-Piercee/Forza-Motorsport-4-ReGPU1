// Dump the decrypted, decompressed image of a XEX: <out>.bin from the image
// base, plus a section list on stdout.
#include <cstdio>
#include <fstream>
#include <file.h>
#include <image.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("xexdump <default.xex> <out.bin>\n");
        return 1;
    }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    std::printf("base %08zX size %08X entry %08zX\n", image.base, image.size, image.entry_point);
    for (const auto& s : image.sections)
        std::printf("%-8s %08zX %08X %s\n", s.name.c_str(), s.base, s.size, s.flags & SectionFlags_Code ? "code" : "data");

    std::ofstream out(argv[2], std::ios::binary);
    uint32_t written = 0;
    for (; written < image.size && out.good(); written += 0x1000)
        out.write(reinterpret_cast<const char*>(image.data.get()) + written, 0x1000);
    std::printf("wrote %X of %X bytes: %s\n", written, image.size, out.good() ? "ok" : "FAILED");
}
