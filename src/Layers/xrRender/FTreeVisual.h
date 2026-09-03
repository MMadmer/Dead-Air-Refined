#pragma once

#include <atomic>

#include "FBasicVisual.h"

struct FSlideWindowItem;

namespace xray::render::RENDER_NAMESPACE
{
#ifdef USE_DX11
constexpr u32 FTreeVisualInstanceVectorCount = 10;

struct FTreeVisualInstanceData
{
    Fvector4 vectors[FTreeVisualInstanceVectorCount];
};
static_assert(sizeof(FTreeVisualInstanceData) == FTreeVisualInstanceVectorCount * sizeof(Fvector4));

struct FTreeVisualInstancedDraw
{
    SGeometry* geometry{};
    u32 base_vertex{};
    u32 vertex_count{};
    u32 start_index{};
    u32 primitive_count{};
};
#endif

class FTreeVisual : public dxRender_Visual, public IRender_Mesh
{
private:
    struct _5color
    {
        Fvector rgb; // - all static lighting
        float hemi; // - hemisphere
        float sun; // - sun
    };

protected:
    _5color c_scale;
    _5color c_bias;
    Fmatrix xform;

    // The crown's memory. A tree is a damped oscillator: it lags a gust, overshoots after it
    // and rings down over several cycles (measured damping ratios sit at 0.04-0.09). The
    // shader can only be a function of "now", so the state lives here and is integrated once
    // per frame for every tree that is drawn; the shader multiplies its bend by it.
    //   m_wind_q      - response, 1 = following the wind exactly
    //   m_wind_qd     - its rate
    //   m_wind_omega  - natural angular frequency from the model's height (rad/s)
    //   m_wind_frame  - frame the state was last integrated in (first caller wins)
    mutable float m_wind_q{1.f};
    mutable float m_wind_qd{};
    float m_wind_omega{};
    // Height of the model: the trunk bend profile in the shaders runs on it (c_tree, row 9).
    float m_tree_height{1.f};
    mutable std::atomic<u32> m_wind_frame{};
    void UpdateWindState() const;
    // Row 8 of the per-instance data / c_sun of the scalar path: (sun scale, sun bias,
    // state, frequency factor for the sway phase).
    Fvector4 wind_state_row(float s) const;

public:
    // The vegetation-audio layer harvests tree world positions once per level load.
    const Fvector& root_position() const { return xform.c; }

    virtual void Render(CBackend& cmd_list, float LOD, bool use_fast_geo) override; // LOD - Level Of Detail  [0.0f - min, 1.0f - max], Ignored
#ifdef USE_DX11
    virtual bool GetInstancedDraw(float LOD, FTreeVisualInstancedDraw& draw);
    void FillInstanceData(CBackend& cmd_list, FTreeVisualInstanceData& data) const;
    static void SetupInstancedGlobals(CBackend& cmd_list);
#endif
    virtual void Load(LPCSTR N, IReader* data, u32 dwFlags);
    virtual void Copy(dxRender_Visual* pFrom);
    virtual void Release();

    FTreeVisual(void);
    virtual ~FTreeVisual(void);
};

class FTreeVisual_ST : public FTreeVisual
{
    typedef FTreeVisual inherited;

public:
    FTreeVisual_ST(void);
    virtual ~FTreeVisual_ST(void);

    virtual void Render(CBackend& cmd_list, float LOD, bool use_fast_geo) override; // LOD - Level Of Detail  [0.0f - min, 1.0f - max], Ignored
#ifdef USE_DX11
    virtual bool GetInstancedDraw(float LOD, FTreeVisualInstancedDraw& draw) override;
#endif
    virtual void Load(LPCSTR N, IReader* data, u32 dwFlags);
    virtual void Copy(dxRender_Visual* pFrom);
    virtual void Release();

private:
    FTreeVisual_ST(const FTreeVisual_ST& other);
    void operator=(const FTreeVisual_ST& other);
};

class FTreeVisual_PM : public FTreeVisual
{
    typedef FTreeVisual inherited;

private:
    FSlideWindowItem* pSWI;
    u32 last_lod;
    u32 SelectLOD(float LOD);

public:
    FTreeVisual_PM(void);
    virtual ~FTreeVisual_PM(void);

    virtual void Render(CBackend& cmd_list, float LOD, bool use_fast_geo) override; // LOD - Level Of Detail  [0.0f - min, 1.0f - max], Ignored
#ifdef USE_DX11
    virtual bool GetInstancedDraw(float LOD, FTreeVisualInstancedDraw& draw) override;
#endif
    virtual void Load(LPCSTR N, IReader* data, u32 dwFlags);
    virtual void Copy(dxRender_Visual* pFrom);
    virtual void Release();

private:
    FTreeVisual_PM(const FTreeVisual_PM& other);
    void operator=(const FTreeVisual_PM& other);
};

const int FTreeVisual_tile = 16;
const int FTreeVisual_quant = 32768 / FTreeVisual_tile;
} // namespace xray::render::RENDER_NAMESPACE
