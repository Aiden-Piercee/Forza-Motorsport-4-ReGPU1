// XAM: users, profiles, system UI, content packages, input, notifications.
// The port runs headless: one signed-in local profile, every system dialog
// answered with its default.
#include <deque>
#include <filesystem>

#include "kernel/imports.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "loader.h"

namespace
{
constexpr uint64_t kXuid = 0xE000000000F02F02ull;

struct ListenerObject : KernelObject
{
    ListenerObject() : KernelObject(Kind::Other) {}
    uint64_t mask = 0;
    std::deque<std::pair<uint32_t, uint32_t>> queue; // id, param
};

struct EnumeratorObject : KernelObject
{
    EnumeratorObject() : KernelObject(Kind::Other) {}
};

std::mutex g_listenerMutex;
}

// Users ------------------------------------------------------------------------

GUEST_EXPORT(XamUserGetSigninState)
{
    Return(ctx, ctx.r3.u32 == 0 ? 1 : 0); // signed in locally
}

GUEST_EXPORT(XamUserGetXUID)
{
    // (user, type, xuid*)
    if (ctx.r3.u32 != 0)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    StoreBE64(ctx.r5.u32, kXuid);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserGetName)
{
    // (user, buffer, size)
    if (ctx.r3.u32 != 0)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    const char* name = "Player";
    const uint32_t size = ctx.r5.u32;
    if (size)
    {
        strncpy(GuestPtr<char>(ctx.r4.u32), name, size);
        GuestPtr<char>(ctx.r4.u32)[size - 1] = 0;
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserCheckPrivilege)
{
    // (user, mask, result*)
    StoreIfNonNull(ctx.r5.u32, 1);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserAreUsersFriends)
{
    Return(ctx, X_ERROR_NO_SUCH_USER);
}

GUEST_EXPORT(XamUserReadProfileSettings)
{
    // (titleId, user, xuidCount, xuids, settingCount, settingIds*, bufferSize*, buffer, overlapped)
    // Answer every setting as "not set": the title then uses its defaults.
    const uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, sizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    const uint32_t overlapped = StackArg(ctx, 8);
    // XUSER_READ_PROFILE_SETTING_RESULT { u32 count, u32 settings* } then
    // XUSER_PROFILE_SETTING { u32 source, u32 user, u64 xuid? ..., u32 id, XUSER_DATA (0x10) } of 0x28 bytes.
    const uint32_t needed = 8 + settingCount * 0x28;
    if (!buffer || LoadBE32(sizePtr) < needed)
    {
        StoreBE32(sizePtr, needed);
        Return(ctx, 122); // ERROR_INSUFFICIENT_BUFFER
        return;
    }
    memset(GuestPtr(buffer), 0, needed);
    StoreBE32(buffer, settingCount);
    StoreBE32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        const uint32_t s = buffer + 8 + i * 0x28;
        StoreBE32(s + 0x00, 0); // source: not set (default)
        StoreBE32(s + 0x04, ctx.r4.u32);
        StoreBE32(s + 0x10, LoadBE32(settingIds + i * 4));
    }
    if (overlapped)
    {
        CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
        Return(ctx, X_ERROR_IO_PENDING);
        return;
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserCreateStatsEnumerator)
{
    Return(ctx, X_ERROR_FUNCTION_FAILED);
}

// System ------------------------------------------------------------------------

GUEST_EXPORT(XGetAVPack) { Return(ctx, 6); }        // HDMI
GUEST_EXPORT(XGetGameRegion) { Return(ctx, 0xFFFF); }
GUEST_EXPORT(XGetLanguage) { Return(ctx, 1); }      // English
GUEST_EXPORT(XamGetSystemVersion) { Return(ctx, 0x20004B07); }
GUEST_EXPORT(XNetLogonGetTitleID) { Return(ctx, g_title.titleId); }

GUEST_EXPORT(XamGetExecutionId)
{
    StoreIfNonNull(ctx.r3.u32, g_title.executionId);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XamAlloc)
{
    // (flags, size, out*)
    const uint32_t address = GuestAlloc(Heap::Virtual, ctx.r4.u32);
    StoreIfNonNull(ctx.r5.u32, address);
    Return(ctx, address ? X_ERROR_SUCCESS : 8);
}

GUEST_EXPORT(XamFree)
{
    GuestFree(ctx.r3.u32);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamLoaderGetLaunchDataSize)
{
    StoreIfNonNull(ctx.r3.u32, 0);
    Return(ctx, X_ERROR_NOT_FOUND);
}

GUEST_EXPORT(XamLoaderGetLaunchData)
{
    Return(ctx, X_ERROR_NOT_FOUND);
}

GUEST_EXPORT(XamLoaderSetLaunchData)
{
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamLoaderLaunchTitle)
{
    Log("XamLoaderLaunchTitle(\"%s\"): exiting", GuestString(ctx.r3.u32).c_str());
    std::_Exit(0);
}

GUEST_EXPORT(XamLoaderTerminateTitle)
{
    Log("XamLoaderTerminateTitle: exiting");
    std::_Exit(0);
}

// Notifications -----------------------------------------------------------------

namespace
{
std::vector<std::weak_ptr<ListenerObject>> g_listeners;
}

GUEST_EXPORT(XamNotifyCreateListener)
{
    // (areas mask (u64), unknown)
    auto listener = std::make_shared<ListenerObject>();
    listener->mask = ctx.r3.u64;
    // Startup state as a fresh console reports it.
    listener->queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED: user 0
    listener->queue.push_back({ 0x00000009, 0 }); // XN_SYS_UI: hidden
    {
        std::lock_guard lock(g_listenerMutex);
        g_listeners.push_back(listener);
    }
    Return(ctx, CreateHandle(listener));
}

GUEST_EXPORT(XNotifyGetNext)
{
    // (handle, matchId, id*, param*)
    auto object = GetHandle(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, 0);
        return;
    }
    auto& listener = static_cast<ListenerObject&>(*object);
    std::lock_guard lock(g_listenerMutex);
    const uint32_t match = ctx.r4.u32;
    for (auto it = listener.queue.begin(); it != listener.queue.end(); ++it)
    {
        if (match && it->first != match)
            continue;
        StoreIfNonNull(ctx.r5.u32, it->first);
        StoreIfNonNull(ctx.r6.u32, it->second);
        listener.queue.erase(it);
        Return(ctx, 1);
        return;
    }
    Return(ctx, 0);
}

// System UI: answered at once ------------------------------------------------------

namespace
{
void FinishUi(PPCContext& ctx, uint32_t overlapped, uint32_t result = X_ERROR_SUCCESS)
{
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result);
        Return(ctx, X_ERROR_IO_PENDING);
    }
    else
        Return(ctx, result);
}
}

GUEST_EXPORT(XamShowSigninUI) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowFriendsUI) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowGamerCardUIForXUID) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowDirtyDiscErrorUI)
{
    Fatal("the title reported a disc read error (XamShowDirtyDiscErrorUI): check the game files");
}

GUEST_EXPORT(XamShowDeviceSelectorUI)
{
    // (user, contentType, flags, totalRequested (u64), deviceId*, overlapped)
    StoreIfNonNull(ctx.r8.u32, 1); // the hard drive
    FinishUi(ctx, ctx.r9.u32);
}

GUEST_EXPORT(XamShowMessageBoxUI)
{
    // (user, title, text, buttonCount, buttons, focus, flags, result*, overlapped)
    std::string text;
    for (uint32_t i = 0, p = ctx.r5.u32; p && i < 256; i++)
    {
        const uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(p)[i]);
        if (!c)
            break;
        text += c < 0x80 ? char(c) : '?';
    }
    Log("message box: %s", text.c_str());
    StoreIfNonNull(StackArg(ctx, 8), 0); // first button
    FinishUi(ctx, StackArg(ctx, 9));
}

GUEST_EXPORT(XamShowMessageBoxUIEx)
{
    StoreIfNonNull(StackArg(ctx, 8), 0);
    FinishUi(ctx, StackArg(ctx, 9));
}

GUEST_EXPORT(XamShowKeyboardUI)
{
    // (user, flags, default text, title, description, result buffer, max length, overlapped)
    const uint32_t def = ctx.r5.u32, out = ctx.r8.u32, max = ctx.r9.u32;
    if (out && max)
    {
        uint32_t i = 0;
        for (; def && i + 1 < max; i++)
        {
            const uint16_t c = GuestPtr<uint16_t>(def)[i];
            if (!c)
                break;
            GuestPtr<uint16_t>(out)[i] = c;
        }
        GuestPtr<uint16_t>(out)[i] = 0;
    }
    FinishUi(ctx, ctx.r10.u32);
}

// Input (controllers arrive in phase 4) -----------------------------------------------

GUEST_EXPORT(XamInputGetCapabilities) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }
GUEST_EXPORT(XamInputGetState) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }
GUEST_EXPORT(XamInputSetState) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }

// Content packages (saves) --------------------------------------------------------------

GUEST_EXPORT(XamContentCreateEx)
{
    // (user, rootName, XCONTENT_DATA*, flags, disposition*, license*, cacheSize, contentSize (u64), overlapped)
    const std::string root = GuestString(ctx.r4.u32);
    const uint32_t data = ctx.r5.u32;
    // XCONTENT_DATA: deviceId, contentType, displayName[128] (wide), fileName[42].
    const std::string fileName(GuestPtr<char>(data + 8 + 256), strnlen(GuestPtr<char>(data + 8 + 256), 42));
    const uint32_t contentType = LoadBE32(data + 4);
    const std::filesystem::path dir = UserDir() / "content" / ([&] {
        char t[16];
        snprintf(t, sizeof(t), "%08X", contentType);
        return std::string(t);
    }()) / fileName;
    std::error_code ec;
    const bool existed = std::filesystem::exists(dir, ec);
    const uint32_t flags = ctx.r6.u32;
    const uint32_t mode = flags & 0xF; // 1 create new, 2 create always, 3 open, 4 open always
    if (!existed && mode == 3)
    {
        FinishUi(ctx, StackArg(ctx, 8), X_ERROR_FILE_NOT_FOUND);
        return;
    }
    std::filesystem::create_directories(dir, ec);
    const std::string device = "\\Device\\Content\\" + dir.parent_path().filename().string() + "\\" + fileName;
    VfsAddLink(root + ":", device);
    Log("content: %s: -> %s", root.c_str(), dir.string().c_str());
    StoreIfNonNull(ctx.r7.u32, existed ? 2 : 1); // XCONTENT_OPENED_EXISTING : XCONTENT_CREATED_NEW
    StoreIfNonNull(ctx.r8.u32, 0);
    FinishUi(ctx, StackArg(ctx, 8));
}

GUEST_EXPORT(XamContentClose)
{
    VfsRemoveLink(GuestString(ctx.r3.u32) + ":");
    FinishUi(ctx, ctx.r4.u32);
}

GUEST_EXPORT(XamContentFlush) { FinishUi(ctx, ctx.r4.u32); }
GUEST_EXPORT(XamContentDelete) { FinishUi(ctx, ctx.r5.u32); }

GUEST_EXPORT(XamContentGetCreator)
{
    // (user, data*, isCreator*, xuid*, overlapped)
    StoreIfNonNull(ctx.r5.u32, 1);
    if (ctx.r6.u32)
        StoreBE64(ctx.r6.u32, kXuid);
    FinishUi(ctx, ctx.r7.u32);
}

GUEST_EXPORT(XamContentGetDeviceState)
{
    FinishUi(ctx, ctx.r4.u32);
}

GUEST_EXPORT(XamContentGetDeviceData)
{
    // (deviceId, XDEVICE_DATA*)
    const uint32_t data = ctx.r4.u32;
    memset(GuestPtr(data), 0, 0x50);
    StoreBE32(data + 0, ctx.r3.u32);
    StoreBE32(data + 4, 1);                 // device type: HDD
    StoreBE64(data + 8, 20ull << 30);        // total bytes
    StoreBE64(data + 16, 16ull << 30);       // free bytes
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamContentCreateEnumerator)
{
    // (user, deviceId, contentType, flags, itemCount, bufferSize*, handle*)
    StoreIfNonNull(ctx.r8.u32, ctx.r7.u32 * 0x134);
    StoreIfNonNull(ctx.r9.u32, CreateHandle(std::make_shared<EnumeratorObject>()));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamCreateEnumeratorHandle)
{
    StoreIfNonNull(StackArg(ctx, 8), CreateHandle(std::make_shared<EnumeratorObject>()));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamEnumerate)
{
    // (handle, flags, buffer, size, itemsReturned*, overlapped)
    StoreIfNonNull(ctx.r7.u32, 0);
    FinishUi(ctx, ctx.r8.u32, X_ERROR_NO_MORE_FILES);
}

// Tasks ----------------------------------------------------------------------------

GUEST_EXPORT(XamTaskSchedule)
{
    // (callback, message, unknown, handle*)
    auto t = CreateGuestThread(ctx.r3.u32, ctx.r4.u32, 0, 0x10000, false, "xam task");
    StoreIfNonNull(ctx.r6.u32, t->handle);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XamTaskShouldExit) { Return(ctx, 0); }
GUEST_EXPORT(XamTaskCloseHandle) { CloseHandle(ctx.r3.u32); Return(ctx, X_STATUS_SUCCESS); }

// Live, sessions, voice: offline ------------------------------------------------------

GUEST_EXPORT(XMsgInProcessCall) { Return(ctx, X_ERROR_FUNCTION_FAILED); }
GUEST_EXPORT(XMsgStartIORequest) { FinishUi(ctx, ctx.r5.u32, X_ERROR_FUNCTION_FAILED); }
GUEST_EXPORT(XMsgStartIORequestEx) { FinishUi(ctx, ctx.r5.u32, X_ERROR_FUNCTION_FAILED); }
GUEST_EXPORT(XMsgCancelIORequest) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamSessionCreateHandle) { StoreIfNonNull(ctx.r3.u32, CreateHandle(std::make_shared<EnumeratorObject>())); Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamSessionRefObjByHandle) { StoreIfNonNull(ctx.r4.u32, 0); Return(ctx, X_ERROR_FUNCTION_FAILED); }
GUEST_EXPORT(XamVoiceCreate) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }
GUEST_EXPORT(XamVoiceClose) { Return(ctx, 0); }
GUEST_EXPORT(XamVoiceHeadsetPresent) { Return(ctx, 0); }
GUEST_EXPORT(XamVoiceSubmitPacket) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }

// Keys and signing: report every signature as valid ---------------------------------------

GUEST_EXPORT(XeKeysConsolePrivateKeySign)
{
    memset(GuestPtr(ctx.r4.u32), 0, 0x228);
    Return(ctx, 1);
}

GUEST_EXPORT(XeKeysConsoleSignatureVerification)
{
    StoreIfNonNull(ctx.r5.u32, 0);
    Return(ctx, 1);
}

// Network: no link. Every call fails cleanly ------------------------------------------

GUEST_EXPORT(NetDll_XNetStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetCleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSACleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAGetLastError) { Return(ctx, 10050); } // WSAENETDOWN
GUEST_EXPORT(NetDll_XNetGetEthernetLinkStatus) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetGetTitleXnAddr) { Return(ctx, 1); } // XNET_GET_XNADDR_NONE
GUEST_EXPORT(NetDll_XNetGetConnectStatus) { Return(ctx, 3); } // lost
GUEST_EXPORT(NetDll_socket) { Return(ctx, 0xFFFFFFFF); }
GUEST_EXPORT(NetDll_closesocket) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetRandom)
{
    // (caller, buffer, length)
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        GuestPtr<uint8_t>(ctx.r4.u32)[i] = uint8_t(rand());
    Return(ctx, 0);
}
