// Ultrawide: the game's own camera aspect. sub_8257E0D0 is the camera's aspect setter
// (stfs f1,268(r3)), called every frame with 16/9. On an output wider than 16:9 it gets
// the wider aspect instead, so the game's projection and its frustum culling both cover
// the wider view (nothing at the sides is culled); the renderer then stops narrowing
// perspective geometry itself (see vwide in renderer.cpp).
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "kernel/imports.h"

float GameAspect();                          // renderer.cpp: the aspect to give the game (16/9 when not wider)
std::atomic<bool> g_cameraAspectWidened{ false };

PPC_FUNC_IMPL(__imp__sub_8257E0D0);
PPC_FUNC(sub_8257E0D0)
{
    const double sixteenNine = 16.0 / 9.0;
    static const bool off = getenv("FM2_NO_CAMERA_ASPECT") != nullptr; // A/B: the renderer alone widens the view
    if (!off && std::fabs(ctx.f1.f64 - sixteenNine) < 1e-3)
    {
        const float wide = GameAspect();
        if (wide > float(sixteenNine) + 1e-3f)
        {
            ctx.f1.f64 = wide;
            g_cameraAspectWidened.store(true, std::memory_order_relaxed);
        }
    }
    __imp__sub_8257E0D0(ctx, base);
}
