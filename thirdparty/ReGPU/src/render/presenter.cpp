// Native output: a window with a D3D12 flip-model swap chain. The guest's
// VdSwap hands a frame over; a render thread presents it without blocking
// the guest. Until the D3D hooks draw real content the frame is a clear
// whose shade pulses with the frame count, proving the path end to end.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include "log.h"
#include "render/presenter.h"
#include "settings.h"

namespace regpu
{
using Microsoft::WRL::ComPtr;


namespace
{
constexpr UINT kBuffers = 3;
constexpr UINT kWidth = 1280, kHeight = 720;

struct Presenter
{
    HWND window = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvStride = 0;
    ComPtr<ID3D12Resource> backBuffers[kBuffers];
    ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValues[kBuffers] = {};
    UINT64 nextFence = 1;
    HANDLE fenceEvent = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    uint64_t pendingFrame = 0;
    std::vector<uint8_t> image; // latest submitted RGBA8, kWidth x kHeight
    ComPtr<ID3D12Resource> upload[kBuffers];
    uint64_t presentedFrame = 0;
    std::atomic<bool> ready{ false };
};

Presenter g_p;
}

// The size the game renders at (settings: resolution and forced aspect ratio).
void RenderSize(uint32_t& width, uint32_t& height)
{
    GetSettings().RenderSize(width, height);
}

uint64_t GuestSwapCount(); // gpu.cpp

namespace
{

// The window title's frame rate line (TitleFrameStats), handed to the window's own thread.
constexpr UINT kTitleMessage = WM_APP + 1;
std::mutex g_titleMutex;
std::wstring g_titleText;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == kTitleMessage)
    {
        std::wstring text;
        {
            std::lock_guard lock(g_titleMutex);
            text = g_titleText;
        }
        SetWindowTextW(hwnd, text.c_str());
        return 0;
    }
    if (msg == WM_CLOSE)
    {
        // Who asked: the close button and Alt+F4 come through the window's own message loop; a message
        // sent from inside the process shows its sender in the stack.
        const bool altF4 = (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F4) & 0x8000);
        const auto stack = regpu::GetHost().currentStack ? regpu::GetHost().currentStack(16) : std::string();
        Log("presenter: WM_CLOSE (Alt+F4 held: %d, sent from another thread: %d): %s", int(altF4), int(InSendMessage()), stack.c_str());
        Log("presenter: window closed, exiting");
        std::_Exit(0);
    }
    if (msg == WM_CHAR)
    {
        if (regpu::GetHost().onChar)
            regpu::GetHost().onChar(wchar_t(wp));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fatal("presenter: %s failed (%08lX)", what, hr);
}

std::mutex g_deviceMutex;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
std::atomic<bool> g_rendererActive{ false };

void EnsureDevice()
{
    std::lock_guard lock(g_deviceMutex);
    if (g_device)
        return;
    if (getenv("REGPU_D3D12_DEBUG"))
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            // REGPU_D3D12_GBV=1 (with REGPU_D3D12_DEBUG): GPU-based validation and synchronized command
            // queue validation, which report resource state and barrier hazards as the GPU meets them (slow).
            ComPtr<ID3D12Debug1> debug1;
            if (getenv("REGPU_D3D12_GBV") && SUCCEEDED(debug.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
                debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
                Log("presenter: D3D12 GPU-based validation on (REGPU_D3D12_GBV)");
            }
        }
    }
    // REGPU_DRED=1: Device Removed Extended Data, so that a lost device reports the command it stopped at
    // (RendererLogDred). Costs a little GPU time per command.
    if (getenv("REGPU_DRED"))
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Log("presenter: DRED on (REGPU_DRED)");
        }
        else
            Log("presenter: DRED is not available on this system");
    }
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice");
    // With the debug layer: its warnings, errors and corruption reports go to the log as they happen
    // (each distinct message id 20 times at most), so they are there even if the process dies.
    if (getenv("REGPU_D3D12_DEBUG"))
    {
        ComPtr<ID3D12InfoQueue1> info;
        DWORD cookie = 0;
        if (SUCCEEDED(g_device.As(&info)) &&
            SUCCEEDED(info->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*) {
                    if (severity > D3D12_MESSAGE_SEVERITY_WARNING)
                        return;
                    static std::mutex mutex;
                    static std::unordered_map<int, int> seen;
                    std::lock_guard lock(mutex);
                    if (seen[int(id)]++ < 20)
                        Log("d3d12 %s (id %d): %s", severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warning",
                            int(id), description);
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie)))
            Log("presenter: D3D12 debug layer messages go to the log");
        else
            Log("presenter: D3D12 debug layer on, but its message callback is unavailable (messages only at device removal)");
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");
}

void CreateWindowAndDevice()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"regpu";
    wc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
    wc.hIcon = HICON(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    RegisterClassW(&wc);
    // The swap chain is the chosen resolution (the game's image is centred in it with black
    // bars when an aspect ratio is forced). Windowed: the client area starts at that size,
    // shrunk (aspect kept) to fit the screen. Fullscreen: a borderless window over the
    // primary display. Either way the swap chain stretches to the window.
    const Settings& settings = GetSettings();
    const uint32_t outW = settings.width, outH = settings.height;
    HWND surface = nullptr;
    if (settings.fullscreen)
    {
        // Borderless window over the whole display; its black background shows around the picture.
        // The picture (the swap chain) is a child window: the largest rectangle of the chosen
        // aspect ratio, centred. Other windows cannot cover the black bars this way.
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        g_p.window = CreateWindowExW(0, L"regpu", GetSettings().windowTitle.c_str(), WS_POPUP | WS_VISIBLE, 0, 0, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);
        LONG w = sw, h = sh;
        if (int64_t(sw) * outH > int64_t(sh) * outW)
            w = LONG(int64_t(sh) * outW / outH);
        else
            h = LONG(int64_t(sw) * outH / outW);
        surface = CreateWindowExW(0, L"regpu", nullptr, WS_CHILD | WS_VISIBLE, (sw - w) / 2, (sh - h) / 2, w, h,
            g_p.window, nullptr, wc.hInstance, nullptr);
    }
    else
    {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        LONG cw = LONG(outW), ch = LONG(outH);
        const LONG maxW = (work.right - work.left) * 9 / 10, maxH = (work.bottom - work.top) * 9 / 10;
        if (cw > maxW || ch > maxH)
        {
            const double f = std::min(double(maxW) / cw, double(maxH) / ch);
            cw = LONG(cw * f);
            ch = LONG(ch * f);
        }
        RECT r{ 0, 0, cw, ch };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_p.window = CreateWindowW(L"regpu", GetSettings().windowTitle.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }

    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    EnsureDevice();
    g_p.device = g_device;
    g_p.queue = g_queue;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = outW;
    sd.Height = outH;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    Check(factory->CreateSwapChainForHwnd(g_p.queue.Get(), surface ? surface : g_p.window, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
    Check(sc1.As(&g_p.swapChain), "IDXGISwapChain3");
    factory->MakeWindowAssociation(g_p.window, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    Check(g_p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_p.rtvHeap)), "CreateDescriptorHeap");
    g_p.rtvStride = g_p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; i++)
    {
        Check(g_p.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_p.backBuffers[i])), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * g_p.rtvStride;
        g_p.device->CreateRenderTargetView(g_p.backBuffers[i].Get(), nullptr, h);
        Check(g_p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_p.allocators[i])), "CreateCommandAllocator");
    }
    Check(g_p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_p.allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_p.list)),
        "CreateCommandList");
    g_p.list->Close();
    Check(g_p.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_p.fence)), "CreateFence");
    g_p.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = UINT64(kWidth) * kHeight * 4;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < kBuffers; i++)
        Check(g_p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                  IID_PPV_ARGS(&g_p.upload[i])),
            "CreateCommittedResource(upload)");
}

void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g_p.list->ResourceBarrier(1, &b);
}

void DrawFrame(uint64_t frame)
{
    const UINT i = g_p.swapChain->GetCurrentBackBufferIndex();
    if (g_p.fence->GetCompletedValue() < g_p.fenceValues[i])
    {
        g_p.fence->SetEventOnCompletion(g_p.fenceValues[i], g_p.fenceEvent);
        WaitForSingleObject(g_p.fenceEvent, INFINITE);
    }
    g_p.allocators[i]->Reset();
    g_p.list->Reset(g_p.allocators[i].Get(), nullptr);
    ID3D12Resource* backBuffer = g_p.backBuffers[i].Get();

    bool copied = false;
    {
        std::lock_guard lock(g_p.mutex);
        if (g_p.image.size() == size_t(kWidth) * kHeight * 4)
        {
            void* mapped = nullptr;
            g_p.upload[i]->Map(0, nullptr, &mapped);
            memcpy(mapped, g_p.image.data(), g_p.image.size());
            g_p.upload[i]->Unmap(0, nullptr);
            copied = true;
        }
    }

    if (copied)
    {
        Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_p.upload[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, kWidth * 4 };
        g_p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    else
    {
        Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(i) * g_p.rtvStride;
        const float pulse = 0.15f + 0.1f * float((frame % 120) < 60 ? frame % 60 : 60 - frame % 60) / 60.0f;
        const float color[4] = { 0.05f, pulse, 0.12f, 1.0f };
        g_p.list->ClearRenderTargetView(rtv, color, 0, nullptr);
        Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    g_p.list->Close();
    ID3D12CommandList* lists[] = { g_p.list.Get() };
    g_p.queue->ExecuteCommandLists(1, lists);
    g_p.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    g_p.fenceValues[i] = g_p.nextFence++;
    g_p.queue->Signal(g_p.fence.Get(), g_p.fenceValues[i]);
}

void RenderThread()
{
    SetThreadDescription(GetCurrentThread(), L"presenter");
    CreateWindowAndDevice();
    g_p.ready = true;
    Log("presenter: window and D3D12 swap chain ready");
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        uint64_t frame;
        {
            std::unique_lock lock(g_p.mutex);
            g_p.cv.wait_for(lock, std::chrono::milliseconds(16), [] { return g_p.pendingFrame != g_p.presentedFrame; });
            frame = g_p.pendingFrame;
            if (frame == g_p.presentedFrame || g_rendererActive)
                continue;
            g_p.presentedFrame = frame;
        }
        DrawFrame(frame);
    }
}
}

void PresenterInit()
{
    if (getenv("REGPU_HEADLESS"))
        return;
    std::thread(RenderThread).detach();
}

void PresenterSubmitFrame(uint64_t frame)
{
    {
        std::lock_guard lock(g_p.mutex);
        g_p.pendingFrame = frame;
    }
    g_p.cv.notify_one();
}

bool PresenterHasFocus()
{
    return !g_p.window || GetForegroundWindow() == g_p.window;
}

void PresenterSubmitImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba)
{
    if (width != kWidth || height != kHeight)
        return;
    {
        std::lock_guard lock(g_p.mutex);
        g_p.image = std::move(rgba);
        g_p.pendingFrame++;
    }
    g_p.cv.notify_one();
}

ID3D12Device* RenderDevice()
{
    EnsureDevice();
    return g_device.Get();
}

ID3D12CommandQueue* RenderQueue()
{
    EnsureDevice();
    return g_queue.Get();
}

void PresenterUseRenderer()
{
    g_rendererActive = true;
}

bool PresenterRecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* overlay, uint32_t overlayW, uint32_t overlayH)
{
    if (!g_p.ready)
        return false;
    const UINT i = g_p.swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = g_p.backBuffers[i].Get();
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backBuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    // The image is the render size; with a forced aspect ratio it is smaller than the back
    // buffer and goes in the middle, on black.
    const D3D12_RESOURCE_DESC sd = src->GetDesc(), bd = backBuffer->GetDesc();
    const UINT offsetX = UINT((bd.Width - std::min(sd.Width, bd.Width)) / 2), offsetY = (bd.Height - std::min(sd.Height, bd.Height)) / 2;
    if (offsetX || offsetY)
    {
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_p.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(i) * g_p.rtvStride;
        const float black[4] = { 0, 0, 0, 1 };
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    list->ResourceBarrier(1, &b);
    if (!offsetX && !offsetY && sd.Width == bd.Width && sd.Height == bd.Height)
        list->CopyResource(backBuffer, src);
    else
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = src;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{ 0, 0, 0, UINT(std::min(sd.Width, bd.Width)), std::min(sd.Height, bd.Height), 1 };
        list->CopyTextureRegion(&dst, offsetX, offsetY, 0, &from, &box);
    }
    if (overlay)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = backBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = overlay;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, offsetX + 24, offsetY + 24, 0, &from, nullptr);
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    return true;
}

// Every half second the window title shows the presented frame rate and frame time over that
// half second (average, and the longest frame), and the title's own frame rate (its swaps) when
// that differs. The text goes to the window's thread by a posted message, so a present never
// waits on the window. REGPU_NO_TITLE_FPS=1: the plain title.
void TitleFrameStats()
{
    static const bool off = getenv("REGPU_NO_TITLE_FPS") != nullptr;
    if (off || !g_p.window)
        return;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point windowStart = Clock::now(), last = windowStart;
    static uint32_t frames = 0;
    static double longest = 0;
    static uint64_t swapsAtStart = GuestSwapCount();
    const Clock::time_point now = Clock::now();
    longest = std::max(longest, std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
    frames++;
    const double elapsed = std::chrono::duration<double>(now - windowStart).count();
    if (elapsed < 0.5)
        return;
    const uint64_t swaps = GuestSwapCount();
    const double fps = frames / elapsed, gameFps = double(swaps - swapsAtStart) / elapsed;
    wchar_t line[160];
    int n = swprintf(line, std::size(line), L" | %.1f fps | %.1f ms (max %.1f)", fps, 1000.0 / fps, longest);
    if (n > 0 && std::abs(gameFps - fps) >= 1.0)
        swprintf(line + n, std::size(line) - n, L" | game %.1f fps", gameFps);
    {
        std::lock_guard lock(g_titleMutex);
        g_titleText = GetSettings().windowTitle + line;
    }
    PostMessageW(g_p.window, kTitleMessage, 0, 0);
    windowStart = now;
    frames = 0;
    longest = 0;
    swapsAtStart = swaps;
}

void PresenterPresent()
{
    // Vsync: wait for the display's refresh; off: present at once (tearing allowed).
    if (g_p.ready)
    {
        if (GetSettings().vsync)
            g_p.swapChain->Present(1, 0);
        else
            g_p.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        TitleFrameStats();
    }
}

uint32_t DisplayRefreshHz()
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return mode.dmDisplayFrequency;
    return 60;
}
} // namespace regpu
