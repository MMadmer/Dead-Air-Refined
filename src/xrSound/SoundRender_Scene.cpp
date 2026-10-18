#include "stdafx.h"

#include "Common/LevelStructure.hpp"
#include "xrCDB/Intersect.hpp"
#include "xrMaterialSystem/GameMtlLib.h"

#include "SoundRender_Core.h"
#include "SoundRender_Scene.h"
#include "SoundRender_Emitter.h"

// A tree crown, a bush, an occluder or an invisible wall is passable in the game's own material terms (a bullet does
// not stop there): no wall for a sound either.
static bool snd_passes(const CDB::RESULT& r)
{
    if (r.material >= GMLib.CountMaterial())
        return false;
    const SGameMtl* m = GMLib.GetMaterialByIdx(u16(r.material));
    return m && m->Flags.test(SGameMtl::flPassable);
}

// The nearest hit of the last query that is no passable one, nullptr when there is none.
static const CDB::RESULT* snd_solid_hit(CDB::COLLIDER& db)
{
    const CDB::RESULT* best = nullptr;
    for (const CDB::RESULT *it = db.r_begin(), *end = it + db.r_count(); it != end; ++it)
        if (!snd_passes(*it) && (!best || it->range < best->range))
            best = it;
    return best;
}

CSoundRender_Scene::~CSoundRender_Scene()
{
    ZoneScoped;

    stop_emitters();

    set_geometry_occ(nullptr, {});
    set_geometry_som(nullptr);
    set_geometry_env(nullptr);

    // remove emitters
    for (auto& emit : s_emitters)
        xr_delete(emit);
    s_emitters.clear();

    xr_delete(geom_ENV);
    xr_delete(geom_SOM);
}

void CSoundRender_Scene::stop_emitters() const
{
    for (const auto& emit : s_emitters)
        emit->stop(false);
}

int CSoundRender_Scene::pause_emitters(bool pauseState)
{
    m_iPauseCounter += pauseState ? +1 : -1;
    VERIFY(m_iPauseCounter >= 0);

    for (const auto& emit : s_emitters)
        emit->pause(pauseState, pauseState ? m_iPauseCounter : m_iPauseCounter + 1);

    return m_iPauseCounter;
}

void CSoundRender_Scene::set_handler(sound_event* E) { sound_event_handler = E; }

void CSoundRender_Scene::set_geometry_occ(CDB::MODEL* M, const Fbox& /*aabb*/)
{
    geom_MODEL = M;
}

void CSoundRender_Scene::set_geometry_som(IReader* I)
{
    ZoneScoped;

    xr_delete(geom_SOM);
    if (nullptr == I)
        return;

    // check version
    R_ASSERT(I->find_chunk(0));
    [[maybe_unused]] const u32 version = I->r_u32();
    VERIFY2(version == 0, "Invalid SOM version");

    struct SOM_poly
    {
        Fvector3 v1;
        Fvector3 v2;
        Fvector3 v3;
        u32 b2sided;
        float occ;
    };

    CDB::Collector CL;
    {
        // load geometry
        IReader* geom = I->open_chunk(1);
        VERIFY2(geom, "Corrupted SOM file");
        if (!geom)
            return;

        // Load tris and merge them
        const auto begin = static_cast<SOM_poly*>(geom->pointer());
        const auto end = static_cast<SOM_poly*>(geom->end());
        for (SOM_poly* poly = begin; poly != end; ++poly)
        {
            CL.add_face_packed_D(poly->v1, poly->v2, poly->v3, *(u32*)&poly->occ, 0.01f);
            if (poly->b2sided)
                CL.add_face_packed_D(poly->v3, poly->v2, poly->v1, *(u32*)&poly->occ, 0.01f);
        }
        geom->close();
    }

    // Create AABB-tree
    geom_SOM = xr_new<CDB::MODEL>();
    geom_SOM->build(CL.getV(), CL.getVS(), CL.getT(), CL.getTS());
}

void CSoundRender_Scene::set_geometry_env(IReader* I)
{
    ZoneScoped;

    xr_delete(geom_ENV);
    if (nullptr == I)
        return;
    const auto envLib = SoundRender->Parent.get_env_library();
    if (!envLib)
        return;

    // Associate names
    xr_vector<u16> ids;
    IReader* names = I->open_chunk(0);
    while (!names->eof())
    {
        string256 n;
        names->r_stringZ(n, sizeof(n));
        const int id = envLib->GetID(n);
        R_ASSERT(id >= 0);
        ids.push_back((u16)id);
    }
    names->close();

    // Load geometry
    IReader* geom_ch = I->open_chunk(1);

    u8* _data = (u8*)xr_malloc(geom_ch->length());

    memcpy(_data, geom_ch->pointer(), geom_ch->length());

    IReader* geom = xr_new<IReader>(_data, geom_ch->length(), 0);

    hdrCFORM H;
    geom->r(&H, sizeof(hdrCFORM));
    Fvector* verts = (Fvector*)geom->pointer();
    CDB::TRI* tris = (CDB::TRI*)(verts + H.vertcount);
    for (u32 it = 0; it < H.facecount; it++)
    {
        CDB::TRI* T = tris + it;
        const u16 id_front = (u16)((T->dummy & 0x0000ffff) >> 0); //	front face
        const u16 id_back = (u16)((T->dummy & 0xffff0000) >> 16); //	back face
        R_ASSERT(id_front < (u16)ids.size());
        R_ASSERT(id_back < (u16)ids.size());
        T->dummy = u32(ids[id_back] << 16) | u32(ids[id_front]);
    }
    geom_ENV = xr_new<CDB::MODEL>();
    geom_ENV->build(verts, H.vertcount, tris, H.facecount);
#ifdef _EDITOR // XXX: may be we are interested in applying env in the game build too?
    env_apply();
#endif
    geom_ch->close();
    geom->close();
    xr_free(_data);
}

void CSoundRender_Scene::play(ref_sound& S, IGameObject* O, u32 flags, float delay)
{
    if (!SoundRender->bPresent || !S._handle())
        return;
    S->g_object = O;
    if (S._feedback())
        ((CSoundRender_Emitter*)S._feedback())->rewind();
    else
        i_play(S, flags, delay);

    if (flags & sm_2D || S._handle()->channels_num() == 2)
        S._feedback()->switch_to_2D();

    S._feedback()->set_ignore_time_factor(flags & sm_IgnoreTimeFactor);
}

void CSoundRender_Scene::play_no_feedback(
    ref_sound& S, IGameObject* O, u32 flags, float delay, Fvector* pos, float* vol, float* freq, Fvector2* range)
{
    if (!SoundRender->bPresent || !S._handle())
        return;
    const ref_sound orig = S;
    S._set(xr_new<CSound>(orig->handle));
    S->g_type = orig->g_type;
    S->g_object = O;
    S->dwBytesTotal = orig->dwBytesTotal;
    S->fTimeTotal = orig->fTimeTotal;
    S->fn_attached[0] = orig->fn_attached[0];
    S->fn_attached[1] = orig->fn_attached[1];

    i_play(S, flags, delay);

    if (flags & sm_2D || S._handle()->channels_num() == 2)
        S._feedback()->switch_to_2D();

    if (pos)
        S._feedback()->set_position(*pos);
    if (freq)
        S._feedback()->set_frequency(*freq);
    if (range)
        S._feedback()->set_range((*range)[0], (*range)[1]);
    if (vol)
        S._feedback()->set_volume(*vol);
    S = orig;
}

void CSoundRender_Scene::play_at_pos(ref_sound& S, IGameObject* O, const Fvector& pos, u32 flags, float delay)
{
    if (!SoundRender->bPresent || !S._handle())
        return;
    S->g_object = O;
    if (S._feedback())
        ((CSoundRender_Emitter*)S._feedback())->rewind();
    else
        i_play(S, flags, delay);

    S._feedback()->set_position(pos);

    if (flags & sm_2D || S._handle()->channels_num() == 2)
        S._feedback()->switch_to_2D();

    S._feedback()->set_ignore_time_factor(flags & sm_IgnoreTimeFactor);
}

CSoundRender_Emitter* CSoundRender_Scene::i_play(ref_sound& S, u32 flags, float delay)
{
    VERIFY(!S->feedback);
    CSoundRender_Emitter* E = xr_new<CSoundRender_Emitter>(this);
    {
        std::lock_guard lock(s_emitters_lock);
        s_emitters.emplace_back(E);
    }
    S->feedback = E;
    E->start(S, flags, delay);
    return E;
}

void CSoundRender_Scene::update()
{
    ZoneScoped;

    s_events_prev_count = s_events.size();

    for (auto& [sound, range] : s_events)
        sound_event_handler(sound, range);

    s_events.clear();
}

void CSoundRender_Scene::object_relcase(IGameObject* obj)
{
    ZoneScoped;

    if (obj)
    {
        for (const auto& emit : s_emitters)
        {
            if (emit)
                if (emit->owner_data)
                    if (obj == emit->owner_data->g_object)
                        emit->owner_data->g_object = 0;
        }
    }
}

float CSoundRender_Scene::get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion)
{
    ZoneScoped;

    float occ_value = 1.f;

    if (nullptr != geom_SOM)
    {
        // Calculate RAY params
        Fvector pos, dir;
        pos.random_dir();
        pos.mul(dispersion);
        pos.add(snd_pt);
        dir.sub(pos, hear_pt);
        const float range = dir.magnitude();
        dir.div(range);

        geom_DB.ray_query(CDB::OPT_CULL, geom_SOM, hear_pt, dir, range);
        const auto r_cnt = geom_DB.r_count();
        CDB::RESULT* begin = geom_DB.r_begin();
        if (0 != r_cnt)
        {
            for (size_t k = 0; k < r_cnt; k++)
            {
                CDB::RESULT* R = begin + k;
                occ_value *= *reinterpret_cast<float*>(&R->dummy);
            }
        }
    }
    return occ_value;
}

float CSoundRender_Scene::get_occlusion(const Fvector& P, float R, Fvector* occ)
{
    ZoneScoped;

    float occ_value = 1.f;

    // Calculate RAY params
    const Fvector base = SoundRender->listener_position();
    Fvector pos, dir;
    pos.random_dir();
    pos.mul(R);
    pos.add(P);
    dir.sub(pos, base);
    const float range = dir.magnitude();
    dir.div(range);

    if (nullptr != geom_MODEL)
    {
        bool bNeedFullTest = true;
        // 1. Check cached polygon
        float u, v, test_range;
        if (CDB::TestRayTri(base, dir, occ, u, v, test_range, true))
            if (test_range > 0 && test_range < range)
            {
                occ_value = psSoundOcclusionScale;
                bNeedFullTest = false;
            }
        // 2. Polygon doesn't picked up - real database query
        if (bNeedFullTest)
        {
            geom_DB.ray_query(0, geom_MODEL, base, dir, range);
            if (const CDB::RESULT* R2 = snd_solid_hit(geom_DB))
            {
                // cache polygon
                const CDB::TRI& T = geom_MODEL->get_tris()[R2->id];
                const Fvector* V = geom_MODEL->get_verts();
                occ[0].set(V[T.verts[0]]);
                occ[1].set(V[T.verts[1]]);
                occ[2].set(V[T.verts[2]]);
                occ_value = psSoundOcclusionScale;
            }
        }
    }
    if (nullptr != geom_SOM)
    {
        geom_DB.ray_query(CDB::OPT_CULL, geom_SOM, base, dir, range);
        const auto r_cnt = geom_DB.r_count();
        CDB::RESULT* begin = geom_DB.r_begin();
        if (0 != r_cnt)
        {
            for (size_t k = 0; k < r_cnt; k++)
            {
                CDB::RESULT* R2 = begin + k;
                occ_value *= *reinterpret_cast<float*>(&R2->dummy);
            }
        }
    }
    return occ_value;
}

void CSoundRender_Scene::set_user_env(CSound_environment* E)
{
    if (0 == E && !bUserEnvironment)
        return;

    if (E)
    {
        s_user_environment = *((CSoundRender_Environment*)E);
        bUserEnvironment = true;
    }
    else
    {
        bUserEnvironment = false;
    }
    SoundRender->env_apply();
}

CSound_environment* CSoundRender_Scene::get_environment(const Fvector& P)
{
    static CSoundRender_Environment identity;

    if (auto* env = authored_environment(P))
        return env;
    identity.set_identity();
    return &identity;
}

CSoundRender_Environment* CSoundRender_Scene::authored_environment(const Fvector& P)
{
    if (bUserEnvironment)
    {
        return &s_user_environment;
    }
    if (geom_ENV)
    {
        constexpr Fvector dir = { 0, -1, 0 };
        geom_DB.ray_query(CDB::OPT_ONLYNEAREST, geom_ENV, P, dir, 1000.f);
        if (geom_DB.r_count())
        {
            const auto envLib = SoundRender->Parent.get_env_library();

            const CDB::RESULT* r = geom_DB.r_begin();
            const CDB::TRI* T = geom_ENV->get_tris() + r->id;
            const Fvector* V = geom_ENV->get_verts();
            Fvector tri_norm;
            tri_norm.mknormal(V[T->verts[0]], V[T->verts[1]], V[T->verts[2]]);
            const float dot = dir.dotproduct(tri_norm);
            if (dot < 0)
            {
                const u16 id_front = (u16)((T->dummy & 0x0000ffff) >> 0); //	front face
                return envLib->Get(id_front);
            }
            const u16 id_back = (u16)((T->dummy & 0xffff0000) >> 16); //	back face
            return envLib->Get(id_back);
        }
    }
    return nullptr;
}

bool CSoundRender_Scene::probe_environment(const Fvector& P, CSoundRender_Environment& env, bool log)
{
    if (!geom_MODEL)
        return false;

    // Fibonacci directions from the top down to just below the horizon (lower ones only find the floor). A hit encloses
    // when it is a cover above or, around, a surface steeper than a slope - ground seen at a grazing angle does not.
    constexpr int rays = 40;
    constexpr float range = 50.f;
    int up = 0, up_hits = 0, side = 0, side_hits = 0;
    float sum = 0.f, nearest = range;
    for (int i = 0; i < rays; ++i)
    {
        const float y = 1.f - (i + .5f) * (2.f / rays);
        if (y < -.35f)
            break;
        const float r = _sqrt(1.f - y * y);
        const float a = float(i) * 2.3999632f;
        const Fvector dir = { r * _cos(a), y, r * _sin(a) };
        const bool above = y > .35f;
        (above ? up : side)++;

        geom_probe.ray_query(0, geom_MODEL, P, dir, range);
        const CDB::RESULT* hit = snd_solid_hit(geom_probe);
        if (!hit)
            continue;
        const CDB::TRI& T = geom_MODEL->get_tris()[hit->id];
        const Fvector* V = geom_MODEL->get_verts();
        Fvector n;
        n.mknormal(V[T.verts[0]], V[T.verts[1]], V[T.verts[2]]);
        if (!above && _abs(n.y) > .7f)
            continue;
        (above ? up_hits : side_hits)++;
        sum += hit->range;
        nearest = std::min(nearest, hit->range);
    }

    // Cover, walls, the share of the openings and the mean path to a surface (Sabine with the openings as absorbers).
    const int count = up + side;
    const int hits = up_hits + side_hits;
    const float cover = up ? float(up_hits) / up : 0.f;
    const float walls = side ? float(side_hits) / side : 0.f;
    const float w = .55f * cover + .45f * walls;
    const float open = 1.f - float(hits) / count;
    const float mean = hits ? sum / hits : range;
    const float absorb = .08f + .92f * open;
    const float near_k = clampr(1.f - nearest / 20.f, 0.f, 1.f);

    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    env.set_default();
    env.Room = -600.f + 1000.f * std::log10(std::max(w, .004f));
    env.RoomHF = mix(-1500.f, -250.f, w);
    env.RoomRolloffFactor = 0.f;
    env.DecayTime = clampr(.053f * mean / absorb, .2f, 4.f);
    env.DecayHFRatio = mix(.5f, .85f, w);
    env.Reflections = mix(-2600.f, -400.f, near_k);
    env.ReflectionsDelay = clampr(2.f * nearest / 343.f, .002f, .3f);
    env.Reverb = mix(-1800.f, 200.f, w);
    env.ReverbDelay = clampr(mean / 343.f, .005f, .1f);
    env.EnvironmentSize = clampr(2.f * mean, 1.f, 100.f);
    env.EnvironmentDiffusion = mix(.3f, 1.f, w);
    env.clamp();

    if (log)
        Msg("* [snd_env] cover=%.2f walls=%.2f open=%.2f mean=%.1f near=%.1f -> room=%.0f decay=%.2f refl=%.0f@%.3f reverb=%.0f",
            cover, walls, open, mean, nearest, env.Room, env.DecayTime, env.Reflections, env.ReflectionsDelay, env.Reverb);
    return true;
}

void CSoundRender_Scene::set_environment_size(CSound_environment* src_env, CSound_environment** dst_env)
{
    // XXX: old SDK functionality
    /*if (bEAX)
    {
        CSoundRender_Environment* SE = static_cast<CSoundRender_Environment*>(src_env);
        CSoundRender_Environment* DE = static_cast<CSoundRender_Environment*>(*dst_env);
#if defined(XR_PLATFORM_WINDOWS)
        // set environment
        i_eax_set(&DSPROPSETID_EAX_ListenerProperties,
            DSPROPERTY_EAXLISTENER_IMMEDIATE | DSPROPERTY_EAXLISTENER_ENVIRONMENTSIZE, &SE->EnvironmentSize,
            sizeof(SE->EnvironmentSize));
        i_eax_listener_set(SE);
        i_eax_commit_setting();
        i_eax_set(&DSPROPSETID_EAX_ListenerProperties,
            DSPROPERTY_EAXLISTENER_IMMEDIATE | DSPROPERTY_EAXLISTENER_ENVIRONMENTSIZE, &DE->EnvironmentSize,
            sizeof(DE->EnvironmentSize));
        i_eax_listener_get(DE);
#endif
    }*/
}


void CSoundRender_Scene::set_environment(u32 id, CSound_environment** dst_env)
{
    // XXX: old SDK functionality
    /*if (bEAX)
    {
        CSoundRender_Environment* DE = static_cast<CSoundRender_Environment*>(*dst_env);
#if defined(XR_PLATFORM_WINDOWS)
        // set environment
        i_eax_set(&DSPROPSETID_EAX_ListenerProperties,
            DSPROPERTY_EAXLISTENER_IMMEDIATE | DSPROPERTY_EAXLISTENER_ENVIRONMENTSIZE, &id, sizeof(id));
        i_eax_listener_get(DE);
#endif
    }*/
}
