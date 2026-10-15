#include "stdafx.h"
#include "GameMtlLib.h"

namespace
{
void DestroySounds(xr_vector<ref_sound>& lst)
{
    for (auto it : lst)
        it.destroy();
}

void DestroyPSs(xr_vector<shared_str>& lst) {}
void CreateSounds(xr_vector<ref_sound>& lst, pcstr buf)
{
    string128 tmp;
    const int cnt = _GetItemCount(buf);
    R_ASSERT(cnt <= GAMEMTL_SUBITEM_COUNT + 2);
    lst.resize(cnt);
    for (int k = 0; k < cnt; ++k)
        lst[k].create(_GetItem(buf, k, tmp), st_Effect, sg_SourceType);
}

void CreateMarks(IWallMarkArray* pMarks, pcstr buf)
{
    string256 tmp;
    const int cnt = _GetItemCount(buf);
    R_ASSERT(cnt <= GAMEMTL_SUBITEM_COUNT);
    for (int k = 0; k < cnt; ++k)
        pMarks->AppendMark(_GetItem(buf, k, tmp));
}

void CreatePSs(xr_vector<shared_str>& lst, pcstr buf)
{
    string256 tmp;
    const int cnt = _GetItemCount(buf);
    R_ASSERT(cnt <= GAMEMTL_SUBITEM_COUNT);
    for (int k = 0; k < cnt; ++k)
        lst.emplace_back(_GetItem(buf, k, tmp));
}
}

SGameMtlPair::~SGameMtlPair()
{
    // destroy all media
    DestroySounds(BreakingSounds);
    DestroySounds(StepSounds);
    DestroySounds(CollideSounds);
    DestroyPSs(CollideParticles);
}

void SGameMtlPair::Load(IReader& fs)
{
    shared_str buf;

    R_ASSERT(fs.find_chunk(GAMEMTLPAIR_CHUNK_PAIR));
    mtl0 = fs.r_u32();
    mtl1 = fs.r_u32();
    ID = fs.r_u32();
    ID_parent = fs.r_u32();
    u32 own_mask = fs.r_u32();
    // XXX: recheck below when we will be integrating EDITOR
#ifdef GM_NON_GAME
    if (GAMEMTL_NONE_ID == ID_parent)
        OwnProps.one();
    else
        OwnProps.assign(own_mask);
#else
    OwnProps.assign(own_mask);
#endif

    R_ASSERT(fs.find_chunk(GAMEMTLPAIR_CHUNK_BREAKING));
    fs.r_stringZ(src_breaking);
    CreateSounds(BreakingSounds, src_breaking.c_str());

    R_ASSERT(fs.find_chunk(GAMEMTLPAIR_CHUNK_STEP));
    fs.r_stringZ(src_step);
    CreateSounds(StepSounds, src_step.c_str());

    R_ASSERT(fs.find_chunk(GAMEMTLPAIR_CHUNK_COLLIDE));
    fs.r_stringZ(src_collide);
    CreateSounds(CollideSounds, src_collide.c_str());

    fs.r_stringZ(src_particles);
    CreatePSs(CollideParticles, src_particles.c_str());

    fs.r_stringZ(src_marks);
    CreateMarks(&*CollideMarks, src_marks.c_str());
}

// ---- Runtime materials ------------------------------------------------------------------------------------------
// The frozen water (docs/dead-air/WEATHER.md, ice): a liquid triangle carries a weight where the season's ice is thick
// enough, and the physics, the bullets and the steps then see this material instead of the water. It starts as a copy
// of a solid material of the library ([ice] source of dead_air_x64_water.ltx) and its pairs, and wears its own steps,
// bullet hits and particles. A library that already has a material of that name keeps it.
void CGameMtlLibrary::AppendRuntimeMaterials()
{
    string_path path;
    if (!FS.exist(path, "$game_config$", "dead_air_x64_water.ltx"))
        return;
    const CInifile ini(path, TRUE);
    constexpr pcstr sect = "ice";
    if (!ini.section_exist(sect))
        return;
    const auto rs = [&](pcstr line, pcstr def) { return ini.line_exist(sect, line) ? ini.r_string(sect, line) : def; };
    const auto rf = [&](pcstr line, float def) { return ini.line_exist(sect, line) ? ini.r_float(sect, line) : def; };

    const pcstr name = rs("material", "materials\\ice");
    if (GetMaterial(name))
        return;
    const pcstr source_name = rs("source", "materials\\flooring_tile");
    const SGameMtl* source = GetMaterial(source_name);
    if (!source)
    {
        Msg("! [ice] no source material '%s' - no ice material", source_name);
        return;
    }

    SGameMtl* ice = materials.emplace_back(xr_new<SGameMtl>(*source));
    ice->ID = material_index++;
    ice->m_Name = name;
    ice->m_Desc = "frozen water (runtime)";
    ice->Flags.set(SGameMtl::flPassable | SGameMtl::flLiquid | SGameMtl::flSlowDown | SGameMtl::flInjurious |
        SGameMtl::flDynamic | SGameMtl::flClimable, FALSE);
    ice->Flags.set(SGameMtl::flBounceable | SGameMtl::flSuppressWallmarks, TRUE);
    ice->fPHFriction = rf("friction", source->fPHFriction * 0.6f);
    ice->fShootFactor = rf("shoot_factor", 0.f);
    ice->fFlotationFactor = 1.f;
    ice->fInjuriousSpeed = 0.f;

    // Pairs: the source's with every material, with the steps of creatures, the bullet's hit and the collisions'
    // particles of the ice.
    const pcstr steps = rs("steps", "");
    const pcstr bullet_sounds = rs("bullet_sounds", "");
    const pcstr bullet_particles = rs("bullet_particles", "");
    const pcstr collide_sounds = rs("collide_sounds", "");
    const pcstr bullet = rs("bullet_material", "objects\\bullet");
    const int source_id = source->ID;
    u32 made = 0;
    const size_t count = materials.size();
    for (size_t i = 0; i < count; ++i)
    {
        SGameMtl* other = materials[i];
        const int other_id = other == ice ? source_id : other->ID;
        const SGameMtlPair* from = nullptr;
        for (const SGameMtlPair* p : material_pairs)
            if (p->IsPair(source_id, other_id))
            {
                from = p;
                break;
            }
        SGameMtlPair* pair = material_pairs.emplace_back(xr_new<SGameMtlPair>(this));
        pair->SetPair(ice->ID, other == ice ? ice->ID : other->ID);
        pair->ID = material_pair_index++;
        pair->ID_parent = GAMEMTL_NONE_ID;
        pair->OwnProps.one();
        const bool creature = !strncmp(other->m_Name.c_str(), "creatures\\", 10);
        const bool is_bullet = !xr_strcmp(other->m_Name, bullet);
        pair->src_breaking = from ? from->src_breaking : shared_str("");
        pair->src_step = creature && steps[0] ? shared_str(steps) : from ? from->src_step : shared_str("");
        pair->src_collide = is_bullet && bullet_sounds[0] ? shared_str(bullet_sounds) :
            !is_bullet && collide_sounds[0]                ? shared_str(collide_sounds) :
            from                                            ? from->src_collide :
                                                              shared_str("");
        pair->src_particles = is_bullet && bullet_particles[0] ? shared_str(bullet_particles) :
            from                                                ? from->src_particles :
                                                                  shared_str("");
        // No marks: a decal on the water's triangles would stay when the ice goes (the ice draws its own).
        pair->src_marks = "";
        CreateSounds(pair->BreakingSounds, pair->src_breaking.c_str());
        CreateSounds(pair->StepSounds, pair->src_step.c_str());
        CreateSounds(pair->CollideSounds, pair->src_collide.c_str());
        CreatePSs(pair->CollideParticles, pair->src_particles.c_str());
        ++made;
    }

    const auto mtlCount = materials.size();
    material_pairs_rt.assign(mtlCount * mtlCount, nullptr);
    for (const auto& mtlPair : material_pairs)
    {
        const int idx0 = GetMaterialIdx(mtlPair->mtl0) * mtlCount + GetMaterialIdx(mtlPair->mtl1);
        const int idx1 = GetMaterialIdx(mtlPair->mtl1) * mtlCount + GetMaterialIdx(mtlPair->mtl0);
        material_pairs_rt[idx0] = mtlPair;
        material_pairs_rt[idx1] = mtlPair;
    }
    Msg("* [ice] runtime material '%s' (from '%s', index %u), %u pairs", name, source_name,
        u32(GetMaterialIdx(name)), made);
}
