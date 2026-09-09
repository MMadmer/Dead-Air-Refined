#pragma once

#include "Include/xrRender/RainRender.h"

namespace xray::render::RENDER_NAMESPACE
{
class dxRainRender : public IRainRender
{
public:
    dxRainRender();
    virtual ~dxRainRender();
    virtual void Copy(IRainRender& _in);

    virtual void Render(CEffect_Rain& owner);

    virtual const Fsphere& GetDropBounds() const;

private:
    // Visualization	(rain)
    ref_shader SH_Rain;
    ref_geom hGeom_Rain;

    // Visualization	(drops)
    IRender_DetailModel* DM_Drop;
    ref_geom hGeom_Drops;
    // Splashes used to be drawn with the streaks' own blender, so a crown lying in the mud was
    // shaded exactly like a drop in the air - and the soft depth fade a streak needs would eat
    // the crown's base, which is flush with the ground it stands on. Optional: if the loose
    // effects_rain_splash.s is not deployed this stays empty and the detail model's shader
    // takes over, exactly as before.
    ref_shader SH_Splash;
};
} // namespace xray::render::RENDER_NAMESPACE
