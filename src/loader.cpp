#include "loader.h"
#include "indirect.h"
#include "recomp_modules.h"

#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>
#include <vector>

#include <ppc_config.h>
#include <ppc_context.h>

#include <image.h>
#ifdef FM2_TITLE_UPDATE
#include <xex_patcher.h>
#endif

#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "log.h"
#include "memory.h"
#include "user_dir.h"

LoadedTitle g_title;

namespace
{
constexpr uint32_t kXexHeaderTls = 0x00020104;
constexpr uint32_t kXexHeaderStackSize = 0x00020200;
constexpr uint32_t kXexHeaderImports = 0x000103FF;
constexpr uint32_t kXexHeaderExecutionId = 0x00040006;
constexpr uint32_t kXexHeaderSystemFlags = 0x00030000;
constexpr uint32_t kXexHeaderExportsByName = 0x00E10402;
constexpr uint32_t kXexModuleDll = 0x8; // XEX module flags (header + 4)

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        Fatal("cannot open %s", path.string().c_str());
    return { std::istreambuf_iterator<char>(f), {} };
}

uint32_t Be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t AllocZeroed(uint32_t size)
{
    return GuestAlloc(Heap::Virtual, size, 16);
}

// Guest storage for a kernel variable export, by xboxkrnl ordinal.
uint32_t KernelVariable(uint32_t ordinal)
{
    switch (ordinal)
    {
    case 0x01B: // ExThreadObjectType
    {
        const uint32_t type = AllocZeroed(0x40);
        StoreBE32(type, 0x54687264); // 'Thrd', only compared by address
        return type;
    }
    case 0x059: // KeDebugMonitorData
    case 0x266: // KeCertMonitorData
    case 0x1C1: // VdHSIOCalibrationLock
        return AllocZeroed(0x100);
    case 0x0AD: // KeTimeStampBundle: InterruptTime, SystemTime, TickCount
        g_title.keTimeStampBundle = AllocZeroed(0x18);
        return g_title.keTimeStampBundle;
    case 0x1AE: // ExLoadedCommandLine
    {
        const uint32_t s = AllocZeroed(0x100);
        strcpy(GuestPtr<char>(s), "\"default.xex\"");
        return s;
    }
    case 0x193: // XexExecutableModuleHandle: pointer to the module handle
    {
        const uint32_t p = AllocZeroed(4);
        StoreBE32(p, g_title.ldrEntry);
        return p;
    }
    case 0x1C0: // VdGpuClockInMHz
    {
        const uint32_t p = AllocZeroed(4);
        StoreBE32(p, 500);
        return p;
    }
    case 0x158: // XboxKrnlVersion: major, minor, build, qfe
    {
        const uint32_t p = AllocZeroed(8);
        auto* v = GuestPtr<uint16_t>(p);
        v[0] = __builtin_bswap16(2);
        v[1] = __builtin_bswap16(0);
        v[2] = __builtin_bswap16(17559);
        v[3] = 0;
        return p;
    }
    case 0x1BE: // VdGlobalDevice
        return AllocZeroed(4);
    default:
        Log("loader: unknown kernel variable ordinal %03X, giving it zeroed storage", ordinal);
        return AllocZeroed(0x100);
    }
}

void ResolveVariableImports(const uint8_t* xex, uint32_t textStart, uint32_t textEnd)
{
    const uint32_t importsOffset = [&] {
        const uint32_t count = Be32(xex + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            if (Be32(xex + 0x18 + i * 8) == kXexHeaderImports)
                return Be32(xex + 0x18 + i * 8 + 4);
        }
        return 0u;
    }();
    if (!importsOffset)
        return;

    const uint8_t* header = xex + importsOffset;
    const uint32_t stringTableSize = Be32(header + 4);
    const uint32_t libraryCount = Be32(header + 8);
    std::vector<std::string> names;
    for (const char* p = reinterpret_cast<const char*>(header + 12); names.size() < libraryCount;)
    {
        names.emplace_back(p);
        p += ((names.back().size() + 1) + 3) & ~size_t(3);
    }

    const uint8_t* library = header + 12 + stringTableSize;
    for (uint32_t lib = 0; lib < libraryCount; lib++)
    {
        const uint32_t librarySize = Be32(library);
        const uint16_t nameIndex = (library[0x24] << 8) | library[0x25];
        const uint16_t recordCount = (library[0x26] << 8) | library[0x27];
        auto inText = [&](uint32_t a) { return a >= textStart && a < textEnd; };
        for (uint32_t r = 0; r < recordCount; r++)
        {
            const uint32_t record = Be32(library + 0x28 + r * 4);
            if (inText(record))
                continue; // function thunk, replaced by the recompiled import
            if (r + 1 < recordCount)
            {
                // A function import: an address-table slot followed by its
                // thunk. Point the slot at the thunk for indirect calls.
                const uint32_t next = Be32(library + 0x28 + (r + 1) * 4);
                if (inText(next))
                {
                    StoreBE32(record, next);
                    continue;
                }
            }
            // XenonUtils leaves variable slots byte-swapped: host order now.
            const uint32_t value = *GuestPtr<uint32_t>(record);
            const uint32_t ordinal = value & 0xFFFF;
            if (names[nameIndex] != "xboxkrnl.exe")
            {
                Log("loader: variable import %03X from %s not supported", ordinal, names[nameIndex].c_str());
                StoreBE32(record, AllocZeroed(0x100));
                continue;
            }
            StoreBE32(record, KernelVariable(ordinal));
        }
        library += librarySize;
    }
}

uint32_t OptHeader(const uint8_t* xex, uint32_t key, bool* found = nullptr)
{
    const uint32_t count = Be32(xex + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        if (Be32(xex + 0x18 + i * 8) == key)
        {
            if (found)
                *found = true;
            return Be32(xex + 0x18 + i * 8 + 4);
        }
    }
    if (found)
        *found = false;
    return 0;
}
}

uint32_t XexHeaderField(uint32_t headerBase, uint32_t key)
{
    const uint8_t* xex = GuestPtr<uint8_t>(headerBase);
    const uint32_t count = Be32(xex + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = 0x18 + i * 8;
        if (Be32(xex + entry) != key)
            continue;
        switch (key & 0xFF)
        {
        case 0x00:
            return Be32(xex + entry + 4);
        case 0x01:
            return headerBase + entry + 4;
        default:
            return headerBase + Be32(xex + entry + 4);
        }
    }
    return 0;
}

const std::filesystem::path& TitleUpdateDir()
{
    static const std::filesystem::path dir = [] {
        char buffer[MAX_PATH];
        if (GetEnvironmentVariableA("FM2_TITLE_UPDATE_DIR", buffer, MAX_PATH))
            return std::filesystem::path(buffer);
        return DocumentsUserDir() / "update";
    }();
    return dir;
}

namespace
{
std::vector<std::unique_ptr<LoadedModule>> g_modules;
std::mutex g_modulesMutex; // g_modules itself; the records stay where they are

std::string BaseName(std::string name)
{
    const size_t slash = name.find_last_of("\\/:");
    if (slash != std::string::npos)
        name.erase(0, slash + 1);
    for (char& c : name)
        c = char(tolower((unsigned char)c));
    return name;
}

// Reads one XEX and places its image in guest memory, checked against the recompiled code (the
// title's when desc == nullptr), and binds its variable imports. Returns the XEX file.
std::vector<uint8_t> MapXex(const std::filesystem::path& xexPath, const RecompiledModule* desc, Image& image)
{
    std::vector<uint8_t> file = ReadFile(xexPath);
#ifdef FM2_TITLE_UPDATE
    if (!desc)
    {
        // This build's recompiled code is the title-updated executable: the update's patch
        // turns the disc's default.xex into it (the same patch XenonRecomp applied).
        const std::filesystem::path xexp = TitleUpdateDir() / "default.xexp";
        if (!std::filesystem::exists(xexp))
            Fatal("this build needs the title update: %s is missing (tools/stfs.py update <package> <folder>)", xexp.string().c_str());
        const std::vector<uint8_t> patch = ReadFile(xexp);
        std::vector<uint8_t> patched;
        const auto result = XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), patched, false);
        if (result != XexPatcher::Result::Success)
            Fatal("could not apply %s to %s (result %d): is it the right title update for this disc?", xexp.string().c_str(),
                xexPath.string().c_str(), int(result));
        file = std::move(patched);
        Log("loader: applied the title update %s (%zu bytes)", xexp.string().c_str(), patch.size());
    }
#endif
    image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
        Fatal("%s: could not decrypt/decompress the XEX", xexPath.string().c_str());
    const uint64_t expectedBase = desc ? desc->imageBase : PPC_IMAGE_BASE;
    if (image.base != expectedBase)
        Fatal("%s: image base %08zX does not match the recompiled code (%08llX); wrong XEX?", xexPath.string().c_str(), image.base, expectedBase);
    if (desc && image.size != desc->imageSize)
        Fatal("%s: image size %08X does not match the recompiled code (%08X); wrong XEX?", xexPath.string().c_str(), image.size, desc->imageSize);

    // PE headers, then every section.
    memcpy(GuestPtr(image.base), image.data.get(), 0x600);
    uint32_t textStart = 0, textEnd = 0;
    for (const Section& s : image.sections)
    {
        // The console maps the security info's image size and nothing after it. The XEX tool leaves
        // PE sections there that are not loaded (.reloc, and in FM4's modules .edata, .idata and
        // .XBLD): they are outside XenonUtils' buffer too.
        const uint64_t offset = s.base - image.base;
        if (offset >= image.size)
            continue;
        memcpy(GuestPtr(uint32_t(s.base)), s.data, size_t(std::min<uint64_t>(s.size, image.size - offset)));
        if (s.name == ".text")
        {
            textStart = uint32_t(s.base);
            textEnd = uint32_t(s.base + s.size);
        }
    }
    ResolveVariableImports(file.data(), textStart, textEnd);
    return file;
}

// Loads one XEX into guest memory. The title (desc == nullptr) uses the main recompiled
// image's mapping table; a module brings its own descriptor.
LoadedModule& LoadXex(const std::filesystem::path& xexPath, const RecompiledModule* desc)
{
    Image image;
    const std::vector<uint8_t> file = MapXex(xexPath, desc, image);

    // Indirect-call table for every recompiled image, built once with the title.
    if (!desc)
        IndirectInit();

    LoadedModule& module = [] () -> LoadedModule& {
        std::lock_guard lock(g_modulesMutex);
        return *g_modules.emplace_back(std::make_unique<LoadedModule>());
    }();
    module.path = xexPath;
    module.name = BaseName(xexPath.filename().string());
    module.dll = (Be32(file.data() + 4) & kXexModuleDll) != 0;
    module.desc = desc;
    module.imageBase = uint32_t(image.base);
    module.imageSize = image.size;

    // XEX header, kept in guest memory for RtlImageXexHeaderField and XAM.
    const uint32_t headerSize = Be32(file.data() + 8);
    module.xexHeader = GuestAlloc(Heap::Virtual, headerSize, 0x1000);
    memcpy(GuestPtr(module.xexHeader), file.data(), headerSize);

    module.entryPoint = uint32_t(image.entry_point);
    module.stackSize = OptHeader(file.data(), kXexHeaderStackSize);
    module.systemFlags = OptHeader(file.data(), kXexHeaderSystemFlags);
    if (const uint32_t tls = OptHeader(file.data(), kXexHeaderTls))
    {
        module.tlsSlotCount = Be32(file.data() + tls);
        module.tlsTemplate = Be32(file.data() + tls + 4);
        module.tlsDataSize = Be32(file.data() + tls + 8);
    }
    if (const uint32_t exec = OptHeader(file.data(), kXexHeaderExecutionId))
    {
        module.executionId = module.xexHeader + exec;
        module.titleId = Be32(file.data() + exec + 0xC);
    }
    module.exportTable = Be32(file.data() + Be32(file.data() + 0x10) + 0x160);
    if (const uint32_t byName = OptHeader(file.data(), kXexHeaderExportsByName))
        module.exportsByName = uint32_t(image.base) + Be32(file.data() + byName);

    // Module handle: LDR_DATA_TABLE_ENTRY with the fields titles read.
    module.handle = GuestAlloc(Heap::Virtual, 0x100);
    const uint32_t ldr = module.handle;
    for (uint32_t list = 0; list < 0x18; list += 8)
    {
        StoreBE32(ldr + list, ldr + list);
        StoreBE32(ldr + list + 4, ldr + list);
    }
    StoreBE32(ldr + 0x18, uint32_t(image.base));
    StoreBE32(ldr + 0x1C, uint32_t(image.base));
    StoreBE32(ldr + 0x20, image.size);
    StoreBE32(ldr + 0x38, image.size);
    StoreBE32(ldr + 0x3C, module.entryPoint);
    StoreBE32(ldr + 0x58, module.xexHeader);

    Log("loader: %s title %08X entry %08X stack %#x tls %u slots + %#x bytes @%08X", xexPath.string().c_str(),
        module.titleId, module.entryPoint, module.stackSize, module.tlsSlotCount, module.tlsDataSize, module.tlsTemplate);
    return module;
}
}

void LoadTitle(const std::filesystem::path& xexPath)
{
    LoadedModule& m = LoadXex(xexPath, nullptr);
    m.refCount = 1;
    g_title.entryPoint = m.entryPoint;
    g_title.stackSize = m.stackSize;
    g_title.xexHeader = m.xexHeader;
    g_title.ldrEntry = m.handle;
    g_title.tlsTemplate = m.tlsTemplate;
    g_title.tlsDataSize = m.tlsDataSize;
    g_title.tlsSlotCount = m.tlsSlotCount;
    g_title.systemFlags = m.systemFlags;
    g_title.titleId = m.titleId;
    g_title.executionId = m.executionId;
}

LoadedModule* FindModuleByHandle(uint32_t handle)
{
    std::lock_guard lock(g_modulesMutex);
    for (auto& m : g_modules)
        if (m->handle == handle)
            return m.get();
    return nullptr;
}

LoadedModule* FindModuleByName(const std::string& name)
{
    const std::string wanted = BaseName(name);
    std::lock_guard lock(g_modulesMutex);
    for (auto& m : g_modules)
        if (m->name == wanted)
            return m.get();
    return nullptr;
}

LoadedModule* LoadModule(const std::string& name)
{
    if (LoadedModule* loaded = FindModuleByName(name))
    {
        // Unloaded since (DLL_PROCESS_DETACH, or an attach that failed): the console maps the image
        // afresh, so DllMain starts from the image's initial data again.
        if (loaded->desc && loaded->refCount == 0)
        {
            Image image;
            MapXex(loaded->path, loaded->desc, image);
        }
        return loaded;
    }
    const std::string wanted = BaseName(name);
    const RecompiledModule* descs = g_recompiledModules;
    for (size_t i = 0; i < g_recompiledModuleCount; i++)
    {
        if (BaseName(descs[i].guestPath) != wanted)
            continue;
        const std::filesystem::path path = GameDir() / wanted;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            // Case-insensitive on disk? Retry with the descriptor's own spelling.
            const std::string original = descs[i].guestPath;
            const size_t slash = original.find_last_of("\\/:");
            const std::filesystem::path exact = GameDir() / (slash == std::string::npos ? original : original.substr(slash + 1));
            if (!std::filesystem::exists(exact, ec))
            {
                Log("loader: module %s not found in %s", wanted.c_str(), GameDir().string().c_str());
                return nullptr;
            }
            return &LoadXex(exact, &descs[i]);
        }
        return &LoadXex(path, &descs[i]);
    }
    Log("loader: no recompiled module named %s", name.c_str());
    return nullptr;
}

bool CallModuleEntry(PPCContext& ctx, LoadedModule& module, uint32_t reason)
{
    if (!module.dll || !module.entryPoint)
        return true;
    // A nested call on the caller's thread and stack: below r1 is free at an import call. The caller's
    // registers are put back afterwards.
    const PPCContext saved = ctx;
    ctx.r3.u64 = module.handle;
    ctx.r4.u64 = reason;
    ctx.r5.u64 = 0;
    GuestCall(ctx, module.entryPoint);
    const bool ok = ctx.r3.u32 != 0;
    ctx = saved;
    if (reason == kDllProcessAttach || reason == kDllProcessDetach)
    {
        module.attached = reason == kDllProcessAttach && ok;
        Log("loader: %s DllMain(%s) returned %d", module.name.c_str(), reason == kDllProcessAttach ? "PROCESS_ATTACH" : "PROCESS_DETACH", int(ok));
    }
    return ok;
}

void NotifyModulesOfThread(PPCContext& ctx, uint32_t reason)
{
    std::vector<LoadedModule*> attached;
    {
        std::lock_guard lock(g_modulesMutex);
        for (auto& m : g_modules)
            if (m->dll && m->attached)
                attached.push_back(m.get());
    }
    for (LoadedModule* m : attached)
        CallModuleEntry(ctx, *m, reason);
}

// Exports as the console's loader finds them (big-endian fields).
bool FindModuleExport(const LoadedModule& module, uint32_t ordinal, const char* name, uint32_t* address)
{
    const uint32_t base = module.imageBase;
    if (name)
    {
        // By name: a PE export directory, present only when the XEX header points at one. Its
        // name ordinals index the ordinal table below.
        const uint32_t dir = module.exportsByName;
        if (!dir)
            return false;
        const uint32_t nameCount = LoadBE32(dir + 0x18);
        const uint32_t names = base + LoadBE32(dir + 0x20);
        const uint32_t nameOrdinals = base + LoadBE32(dir + 0x24);
        bool found = false;
        for (uint32_t i = 0; i < nameCount && !found; i++)
        {
            if (strcmp(GuestPtr<char>(base + LoadBE32(names + i * 4)), name) == 0)
            {
                ordinal = LoadBE32(dir + 0x10) + __builtin_bswap16(*GuestPtr<uint16_t>(nameOrdinals + i * 2));
                found = true;
            }
        }
        if (!found)
            return false;
    }
    // By ordinal: the XEX export table (security info), { magic[3], module number[2], version[3],
    // image base >> 16, count, ordinal base, offset[count] }; an export is at (image base << 16) + offset.
    const uint32_t table = module.exportTable;
    if (!table)
        return false;
    const uint32_t count = LoadBE32(table + 0x24);
    const uint32_t ordinalBase = LoadBE32(table + 0x28);
    if (ordinal < ordinalBase || ordinal - ordinalBase >= count)
        return false;
    const uint32_t offset = LoadBE32(table + 0x2C + (ordinal - ordinalBase) * 4);
    if (!offset)
        return false;
    *address = (LoadBE32(table + 0x20) << 16) + offset;
    return true;
}
