// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include <atomic>
#include <mutex>

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/da_wind_history.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
struct wind_history
{
    std::atomic<u32> frame{u32(-1)};
    std::mutex lock;
    da_wind_frame current;
    da_wind_frame previous;
};
wind_history g_da_wind_history;

// What the binders of da_wind_field and da_wm_* hand the shaders this frame.
da_wind_frame da_wind_capture()
{
    da_wind_frame f;
    f.time = Device.fTimeGlobal;
    if (!g_pGamePersistent)
        return f;
    const auto& env = g_pGamePersistent->Environment();
    f.field.set(env.eff_wind_field_ofs.x, env.eff_wind_field_ofs.y, env.eff_wind_norm, env.eff_wind_gust);
    f.wm_pos[0] = env.wind_motor_pos[0];
    f.wm_pos[1] = env.wind_motor_pos[1];
    f.wm_par[0] = env.wind_motor_par[0];
    f.wm_par[1] = env.wind_motor_par[1];
    f.wm_active = env.wind_motor_active;
    return f;
}
} // namespace

const da_wind_frame& da_wind_prev()
{
    wind_history& h = g_da_wind_history;
    const u32 frame = Device.dwFrame;
    // Render threads of one frame only read; the record changes when the next frame starts.
    if (h.frame.load(std::memory_order_acquire) != frame)
    {
        std::lock_guard guard(h.lock);
        const u32 last = h.frame.load(std::memory_order_relaxed);
        if (last != frame)
        {
            const da_wind_frame now = da_wind_capture();
            h.previous = last + 1 == frame && !da_history_reset() ? h.current : now;
            h.current = now;
            h.frame.store(frame, std::memory_order_release);
        }
    }
    return h.previous;
}
} // namespace xray::render::RENDER_NAMESPACE
