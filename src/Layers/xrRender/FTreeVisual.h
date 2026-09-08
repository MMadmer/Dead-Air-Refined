#pragma once

#include <atomic>

#include "FBasicVisual.h"

struct FSlideWindowItem;

namespace xray::render::RENDER_NAMESPACE
{
struct TreeWindShared;

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

    // The tree's memory - ONE record per tree, shared by every visual of it. A tree model in
    // a level is several visuals on one root (the trunk, the crown, sometimes more); a state
    // per visual gave each its own natural frequency and its own height from its own box,
    // and the crown visibly swung against the trunk it grows on. The record is keyed by the
    // root position, refcounted by the visuals, integrated once per frame by the first of
    // them drawn (see TreeWindShared in FTreeVisual.cpp).
    TreeWindShared* m_shared{};
    void UpdateWindState() const;
    Fvector4 tree_wind_row() const;
    // Row 8 of the per-instance data / c_sun of the scalar path: (sun scale, sun bias,
    // state, frequency factor for the sway phase).
    Fvector4 wind_state_row(float s) const;

public:
    bool NeedsWindUpdate() const;
    static void PrepareWind(const xr_vector<FTreeVisual*>& visuals);

    // The vegetation-audio layer harvests tree world positions once per level load.
    const Fvector& root_position() const { return xform.c; }
    // No foliage on this root - a stump, a log, a snag: the wind does not move it.
    bool rigid() const;

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
