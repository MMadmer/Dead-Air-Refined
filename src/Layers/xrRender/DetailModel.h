#pragma once

#include "IRenderDetailModel.h"

namespace xray::render::RENDER_NAMESPACE
{
// What a detail model is (docs/dead-air/GRASS.md), bound by its content key through
// dead_air_x64_seasons.ltx [detail_types]. The blade classes are drawn as procedural blades
// (da_grass_blades.cpp) while blades run; every other class stays a stock card whose keep, height
// and burial under snow follow the season on the GPU.
enum EDetailClass : u8
{
    dcUnknown = 0, // a key the data does not list (mod content): stock card, generic grass season
    dcForb,
    dcHerb,
    dcFern,
    dcHorsetail,
    dcShrub,
    dcLitter,
    dcDecal,
    dcBladeFirst,
    dcBunch = dcBladeFirst,
    dcFine,
    dcLeafy,
    dcPlume,
    dcTuft,
    dcRush,
    dcDry,
    dcCount
};

inline bool detail_class_is_blade(u32 cls) { return cls >= dcBladeFirst && cls < dcCount; }

class ECORE_API CDetail : public IRender_DetailModel
{
private:
    void transfer_indices(u16* iDest, u32 iOffset);
public:
    // Content key: K1 (names, flags, scale range, vertices and indices as the file stores them, before
    // Optimize) after Load; CDetailManager::Load folds the sprite's atlas blocks in (KSPR).
    u64 m_key{};
    EDetailClass m_class{dcUnknown};
    // The uv rectangle of the model, for the sprite fold.
    float m_uv_min[2]{}, m_uv_max[2]{};

    void Load(IReader* S);
    void Optimize();
    virtual void Unload();

    virtual void transfer(Fmatrix& mXform, fvfVertexOut* vDest, u32 C, u16* iDest, u32 iOffset);
    virtual void transfer(Fmatrix& mXform, fvfVertexOut* vDest, u32 C, u16* iDest, u32 iOffset, float du, float dv);
    virtual ~CDetail();
};

// FNV-1a 64 of the content keys (DetailModel.cpp, DetailManager.cpp; tools/seasons/gen_season_data.py).
u64 detail_key_fnv(const void* data, size_t size, u64 hash);
} // namespace xray::render::RENDER_NAMESPACE
