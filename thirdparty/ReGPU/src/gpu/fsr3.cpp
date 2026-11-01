// AMD FSR 3 and FSR 4 upscaling (FidelityFX API, MIT): the same inputs as DLSS (see dlss.h). Needs
// amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll next to the executable
// (copied there by the build). FSR 3 runs on any D3D12 GPU. FSR 4 is the same upscaler DLL's ML
// version: the FidelityFX API lists the versions the GPU can run and ReGPU picks one by name
// (REGPU_FSR_VERSION=<text> picks the first whose name contains the text, for testing).
#include "gpu/fsr3.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/renderer.h"
#include "log.h"

namespace regpu
{
namespace
{
struct Fsr3
{
    bool failed = false;
    ffxContext context = nullptr;
    DlssSetup setup{};
    bool ml = false;
    bool versionsLogged = false;
    // The context is rebuilt whenever its setup changes. A replaced context is destroyed only kRetireFrames
    // evaluations later, when the frames still in flight that used it are done (the destroy used to run
    // at once, with the GPU possibly still using it).
    struct Retired
    {
        ffxContext context;
        int left;
    };
    std::vector<Retired> retired;
    bool hdrSeen = false; // sticky: once the scene has been HDR the context stays HDR (the format flips in menus)
} g_fsr3;
constexpr int kRetireFrames = 8;

bool Contains(const char* name, const char* text)
{
    return name && std::strstr(name, text) != nullptr;
}

// A name from the list of upscaler versions the API offers: "FSR 4" ones are the ML models. The
// names' exact wording is the SDK's, so REGPU_FSR_VERSION can override the match.
bool IsMlVersion(const char* name)
{
    return Contains(name, "FSR 4") || Contains(name, "FSR4") || Contains(name, "4.");
}

// The version id to ask for, or false to leave the choice to the SDK (which takes the newest one
// the GPU runs, so FSR 4 where supported and FSR 3.1 elsewhere).
bool PickVersion(ID3D12Device* device, bool ml, uint64_t& id)
{
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device;
    q.outputCount = &count;
    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0)
        return false;
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.outputCount = &count;
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK)
        return false;
    if (!g_fsr3.versionsLogged)
    {
        g_fsr3.versionsLogged = true;
        for (uint64_t i = 0; i < count; ++i)
            Log("fsr: upscaler version %llu: %s", (unsigned long long)ids[i], names[i] ? names[i] : "?");
    }
    const char* want = std::getenv("REGPU_FSR_VERSION");
    for (uint64_t i = 0; i < count; ++i)
    {
        const bool match = (want && *want) ? Contains(names[i], want) : IsMlVersion(names[i]) == ml;
        if (match)
        {
            id = ids[i];
            Log("fsr: using %s", names[i]);
            return true;
        }
    }
    return false;
}

bool FsrEvaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness, bool ml);
}

bool Fsr3Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness)
{
    return FsrEvaluate(device, list, setup, frame, sharpness, false);
}

bool Fsr4Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness)
{
    return FsrEvaluate(device, list, setup, frame, sharpness, true);
}

namespace
{

bool FsrEvaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness, bool ml)
{
    if (g_fsr3.failed)
        return false;
    for (size_t i = 0; i < g_fsr3.retired.size();)
    {
        if (--g_fsr3.retired[i].left <= 0)
        {
            ffxDestroyContext(&g_fsr3.retired[i].context, nullptr);
            g_fsr3.retired.erase(g_fsr3.retired.begin() + ptrdiff_t(i));
        }
        else
            ++i;
    }
    g_fsr3.hdrSeen = g_fsr3.hdrSeen || setup.hdr;
    DlssSetup want = setup;
    want.hdr = g_fsr3.hdrSeen;
    if (!g_fsr3.context || !(want == g_fsr3.setup) || ml != g_fsr3.ml)
    {
        const char* why = !g_fsr3.context ? "first context" : ml != g_fsr3.ml ? "FSR 3/4 switched"
            : (want.renderWidth != g_fsr3.setup.renderWidth || want.renderHeight != g_fsr3.setup.renderHeight) ? "render size changed"
            : (want.outputWidth != g_fsr3.setup.outputWidth || want.outputHeight != g_fsr3.setup.outputHeight) ? "output size changed"
            : want.hdr != g_fsr3.setup.hdr ? "scene became HDR" : "quality or depth flags changed";
        Log("%s: new context (%s): %ux%u -> %ux%u", ml ? "fsr4" : "fsr3", why, want.renderWidth, want.renderHeight, want.outputWidth, want.outputHeight);
        if (g_fsr3.context)
        {
            g_fsr3.retired.push_back({ g_fsr3.context, kRetireFrames });
            g_fsr3.context = nullptr;
        }
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        ffxOverrideVersion version{};
        uint64_t versionId = 0;
        const bool pinned = PickVersion(device, ml, versionId);
        if (pinned)
        {
            version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            version.versionId = versionId;
            backend.header.pNext = &version.header;
        }
        ffxCreateContextDescUpscale create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext = &backend.header;
        create.maxRenderSize = { want.renderWidth, want.renderHeight };
        create.maxUpscaleSize = { want.outputWidth, want.outputHeight };
        create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | (want.depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
            (want.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0);
        ffxReturnCode_t r = FFX_API_RETURN_ERROR;
        try
        {
            r = ffxCreateContext(&g_fsr3.context, &create.header, nullptr);
        }
        catch (...)
        {
            // (A C++ exception out of the FidelityFX DLL: seen when the device was lost.)
            Log("%s: ffxCreateContext threw; the device is probably lost", ml ? "fsr4" : "fsr3");
            RendererLogDred();
        }
        if (r != FFX_API_RETURN_OK)
        {
            Log("fsr3: create failed (%d): amd_fidelityfx_*_dx12.dll missing?", int(r));
            g_fsr3.context = nullptr;
            g_fsr3.failed = true;
            return false;
        }
        g_fsr3.setup = want;
        g_fsr3.ml = ml;
        Log("%s: context ready%s", ml ? "fsr4" : "fsr3", want.hdr ? ", HDR" : "");
    }
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list;
    d.color = ffxApiGetResourceDX12(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(frame.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = { frame.jitterX, frame.jitterY };
    d.motionVectorScale = { frame.motionScaleX, frame.motionScaleY };
    d.renderSize = { setup.renderWidth, setup.renderHeight };
    d.upscaleSize = { setup.outputWidth, setup.outputHeight };
    d.enableSharpening = sharpness > 0.0f;
    d.sharpness = sharpness;
    d.frameTimeDelta = frame.frameTimeMs;
    d.preExposure = 1.0f;
    d.reset = frame.reset;
    // The game's camera is unknown here; typical values (used for depth-based heuristics).
    d.cameraNear = 0.1f;
    d.cameraFar = 10000.0f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    try
    {
        r = ffxDispatch(&g_fsr3.context, &d.header);
    }
    catch (...)
    {
        Log("%s: ffxDispatch threw; the device is probably lost", ml ? "fsr4" : "fsr3");
        RendererLogDred();
        g_fsr3.failed = true;
    }
    if (r != FFX_API_RETURN_OK)
    {
        static int logged = 0;
        if (logged++ < 3)
            Log("fsr3: dispatch failed (%d)", int(r));
        return false;
    }
    return true;
}
} // namespace
} // namespace regpu
