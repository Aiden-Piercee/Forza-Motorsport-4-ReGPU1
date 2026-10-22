// Link check for the recompiled FM4 images: every image's code, the import stubs
// and the image table link into one program. Running it checks the table: each
// mapping is inside its image's code range, and no guest address is mapped twice.
#include <cstdio>
#include <unordered_set>

#include <ppc_context.h>

#include "recomp_modules.h"

// The runtime's hooks for the recompiled code (src/ppc_hooks.h); never called here.
void PPCMsrChanged(PPCContext&, uint64_t)
{
}

int main()
{
    std::unordered_set<uint32_t> seen;
    int errors = 0;
    for (size_t i = 0; i < g_recompiledModuleCount; i++)
    {
        const RecompiledModule& m = g_recompiledModules[i];
        size_t count = 0;
        for (const PPCFuncMapping* f = m.mappings; f->host; f++, count++)
        {
            const uint32_t guest = uint32_t(f->guest);
            if (guest - m.codeBase >= m.codeSize)
            {
                std::printf("%s: %08X is outside the code range %08X+%X\n", m.guestPath, guest, m.codeBase, m.codeSize);
                errors++;
            }
            if (!seen.insert(guest).second)
            {
                std::printf("%s: %08X is mapped twice\n", m.guestPath, guest);
                errors++;
            }
        }
        std::printf("%-34s image %08X+%08X code %08X+%08X %zu functions\n", m.guestPath, m.imageBase, m.imageSize, m.codeBase,
            m.codeSize, count);
    }
    std::printf(errors ? "%d errors\n" : "ok\n", errors);
    return errors ? 1 : 0;
}
