// FProgressive.h: interface for the FProgressive class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "FVisual.h"

struct FSlideWindowItem;

namespace xray::render::RENDER_NAMESPACE
{
class FProgressive : public Fvisual
{
protected:
    FSlideWindowItem nSWI;
    FSlideWindowItem* xSWI;
    u32 last_lod;

public:
    FProgressive();
    virtual ~FProgressive();
    virtual void Render(CBackend& cmd_list, float LOD, bool use_fast_geo) override; // LOD - Level Of Detail  [0.0f - min, 1.0f - max], -1 = Ignored
    virtual void Load(const char* N, IReader* data, u32 dwFlags);
    virtual void Copy(dxRender_Visual* pFrom);
    virtual void Release();
    // The sliding windows: the first is the full detail (the cluster hierarchy's source, da_clusters.cpp).
    const FSlideWindowItem& windows() const { return nSWI; }
    // The fast geometry's (positions alone), null without one.
    const FSlideWindowItem* windows_fast() const { return xSWI; }

private:
    FProgressive(const FProgressive& other);
    void operator=(const FProgressive& other);
};
} // namespace xray::render::RENDER_NAMESPACE
