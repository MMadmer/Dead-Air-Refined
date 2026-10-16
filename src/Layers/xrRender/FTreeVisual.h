#pragma once

#include <atomic>

#include "FBasicVisual.h"

struct FSlideWindowItem;

namespace xray::render::RENDER_NAMESPACE
{
struct TreeWindShared;

#ifdef USE_DX11
// Keep in step with TREE_INSTANCE_VECTOR_COUNT (shaders/r3/tree_instance.h).
constexpr u32 FTreeVisualInstanceVectorCount = 11;

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

// The static lighting of one tree visual as loaded (before ps_r__Tree_SBC): rgb + hemi, and sun.
struct FTreeVisualColors
{
    Fvector4 scale;
    Fvector4 bias;
    float sun_scale;
    float sun_bias;
};

// One root's wind as the draw paths feed it this frame (the GPU-driven trees upload it).
struct FTreeVisualWind
{
    Fvector4 now; // state, frequency factor, root gust, root deviation
    Fvector4 previous; // the same as the previous frame drew with (motion vectors)
    float height; // tallest visual of the root above it
    float gate; // 1: the root carries foliage, the wind moves it
};

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
    u32 m_vb_id{}, m_ib_id{};

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
    // Row 10 / c_tree_prev: the tree's wind as the previous frame drew it, for the motion
    // vectors - (state, frequency factor, root gust, root deviation). Call after
    // wind_state_row: that is what integrates this frame.
    Fvector4 wind_prev_row() const;

public:
    bool NeedsWindUpdate() const;
    static void PrepareWind(const xr_vector<FTreeVisual*>& visuals);
    // The frame-wide sway every tree shader takes - the wave (its phase in w) and the wind's heading times its amplitude
    // - now and as the previous frame drew with it: the upgraded trees' trunk sway on the stock roots (da_trees.cpp).
    static void FrameWind(Fvector4& wave, Fvector4& wind, Fvector4& wave_prev, Fvector4& wind_prev);

    // The vegetation-audio layer harvests tree world positions once per level load.
    const Fvector& root_position() const { return xform.c; }
    // No foliage on this root - a stump, a log, a snag: the wind does not move it.
    bool rigid() const;

    // Read accessors for the GPU-driven registry and the ray tracing scene (gpudriven.md 3.2).
    const Fmatrix& world() const { return xform; }
    FTreeVisualColors colors() const;
    // The wind root shared by every visual of this tree (null: none); its height above the root.
    const TreeWindShared* wind_root() const { return m_shared; }
    float wind_height() const;
    // The root's natural angular frequency (rad/s, from its height); 0 without a root.
    float wind_omega() const;
    // Integrates the root for this frame when nobody has yet, then reads what a draw would use.
    FTreeVisualWind wind_state() const;
#ifdef USE_DX11
    // Index windows of the geometry: 1 for a stripified tree, the slide windows of a progressive
    // one (0 = full detail); false for a window the visual does not have.
    virtual u32 window_count() const { return 1; }
    virtual bool window(u32 index, FTreeVisualInstancedDraw& draw) const;
#endif
    // The registry's instance index of this visual, u32(-1) while it is not registered.
    u32 gd_instance{u32(-1)};
    // The level vertex and index buffers it draws from (level.geom chunks fsL_VB / fsL_IB): the procedural shrubs
    // read the stock bushes' geometry back from the file to key them (da_shrubs.cpp).
    u32 vb_id() const { return m_vb_id; }
    u32 ib_id() const { return m_ib_id; }

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
    u32 window_count() const override;
    bool window(u32 index, FTreeVisualInstancedDraw& draw) const override;
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
