// The main menu's X button opened the Xbox Live Marketplace, which never answers here: the game sat on a
// loading screen for good. It now asks whether to quit the game instead. The Lua binding ShowMarketplace
// (App::Forza, handler sub_822BBF98) reads its argument and calls the native sub_822B9990, whose result
// is unused; that native call is replaced. Both exist only in the title-updated executable, so their
// names are pasted together (tools/xexremap.py translates sub_/0x literals from the disc executable).
#include <windows.h>

#include <cstdio>

#include "gpu/renderer.h"
#include "kernel/imports.h"
#include "log.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address

PPC_FUNC(TU_SUB(822B9990))
{
    // A dialog on the game window: the game waits on this thread until it closes.
    HWND window = FindWindowW(L"fm2", nullptr);
    const int choice = MessageBoxW(window, L"Quit Forza Motorsport 2?", L"Forza Motorsport 2", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST);
    if (choice == IDYES)
    {
        Log("quit: chosen from the main menu");
        RendererSaveCaches();
        fflush(nullptr);
        ExitProcess(0);
    }
    (void)ctx;
    (void)base;
}
#endif
