#pragma once

#include "FBasicVisual.h"

struct FSlideWindowItem;

namespace xray::render::RENDER_NAMESPACE
{
#ifdef USE_DX11
constexpr u32 FTreeVisualInstanceVectorCount = 9;

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

public:
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
