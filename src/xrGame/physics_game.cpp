#include "StdAfx.h"
#include "ParticlesObject.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Level.h"
#include "GamePersistent.h"
#include "xrPhysics/ExtendedGeom.h"
#include "PhysicsGamePars.h"

#include "xrPhysics/PhysicsExternalCommon.h"
#include "PHSoundPlayer.h"
#include "PhysicsShellHolder.h"
#include "xrPhysics/PHCommander.h"
#include "xrPhysics/MathUtils.h"
#include "da_water_impact.h"
#include "da_water_actor.h"
#include "xrPhysics/IPHWorld.h"

#include "xrPhysics/PHReqComparer.h"

#include "Include/xrRender/FactoryPtr.h"
#include "Include/xrRender/WallMarkArray.h"
//#ifdef	DEBUG

//#endif
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
static const float PARTICLE_EFFECT_DIST = 70.f;
static const float SOUND_EFFECT_DIST = 70.f;
const float mass_limit =
    10000.f; // some conventional value used as evaluative param (there is no code restriction on mass)
//////////////////////////////////////////////////////////////////////////////////
static const float SQUARE_PARTICLE_EFFECT_DIST = PARTICLE_EFFECT_DIST * PARTICLE_EFFECT_DIST;
static const float SQUARE_SOUND_EFFECT_DIST = SOUND_EFFECT_DIST * SOUND_EFFECT_DIST;
class CPHParticlesPlayCall : public CPHAction
{
    LPCSTR ps_name;

protected:
    dContactGeom c;

public:
    CPHParticlesPlayCall(const dContactGeom& contact, bool invert_n, LPCSTR psn)
    {
        ps_name = psn;
        c = contact;
        if (invert_n)
        {
            c.normal[0] = -c.normal[0];
            c.normal[1] = -c.normal[1];
            c.normal[2] = -c.normal[2];
        }
    }
    virtual void run()
    {
        CParticlesObject* ps = CParticlesObject::Create(ps_name, TRUE);

        Fmatrix pos;
        Fvector zero_vel = {0.f, 0.f, 0.f};
        pos.k.set(*((Fvector*)c.normal));
        Fvector::generate_orthonormal_basis(pos.k, pos.j, pos.i);
        pos.c.set(*((Fvector*)c.pos));

        ps->UpdateParent(pos, zero_vel);
        GamePersistent().ps_schedule_play(ps);
    };
    virtual bool obsolete() const { return false; }
};
static const float minimal_plane_distance_between_liquid_particles = 0.2f;
class CPHLiquidParticlesPlayCall : public CPHParticlesPlayCall, public CPHReqComparerV
{
    u32 remove_time;
    bool b_called;

public:
    CPHLiquidParticlesPlayCall(const dContactGeom& contact, bool invert_n, LPCSTR psn)
        : CPHParticlesPlayCall(contact, invert_n, psn), b_called(false)
    {
        static const u32 time_to_call_remove = 3000;
        remove_time = Device.dwTimeGlobal + time_to_call_remove;
    }
    const Fvector& position() const { return cast_fv(c.pos); }
private:
    virtual bool compare(const CPHReqComparerV* v) const { return v->compare(this); }
    virtual void run()
    {
        if (b_called)
            return;
        b_called = true;
        CPHParticlesPlayCall::run();
    }
    virtual bool obsolete() const { return Device.dwTimeGlobal > remove_time; }
};

class CPHLiquidParticlesCondition : public CPHCondition, public CPHReqComparerV
{
private:
    virtual bool compare(const CPHReqComparerV* v) const { return v->compare(this); }
    virtual bool is_true() { return true; }
    virtual bool obsolete() const { return false; }
};
class CPHFindLiquidParticlesComparer : public CPHReqComparerV
{
    Fvector m_position;

public:
    CPHFindLiquidParticlesComparer(const Fvector& position) : m_position(position) {}
private:
    virtual bool compare(const CPHReqComparerV* v) const { return v->compare(this); }
    virtual bool compare(const CPHLiquidParticlesCondition* v) const { return true; }
    virtual bool compare(const CPHLiquidParticlesPlayCall* v) const
    {
        VERIFY(v);

        Fvector disp = Fvector().sub(m_position, v->position());
        return disp.x * disp.x + disp.z * disp.z <
            (minimal_plane_distance_between_liquid_particles * minimal_plane_distance_between_liquid_particles);
    }
};

// class CPHLiquidParticlesComparer :
//	public CPHReqComparerV
//{
//	virtual bool			compare							(const	CPHReqComparerV* v)					const	{return
// v->compare(this);}
//	virtual bool			compare							(const	CPHOnesConditionSelfCmpTrue* v)		const	{return
// true;}
//
//};

class CPHWallMarksCall : public CPHAction
{
    // ref_shader pWallmarkShader;
    wm_shader pWallmarkShader;
    Fvector pos;
    CDB::TRI* T;

public:
    // CPHWallMarksCall(const Fvector &p,CDB::TRI* Tri,ref_shader s)
    CPHWallMarksCall(const Fvector& p, CDB::TRI* Tri, const wm_shader& s)
    {
        pWallmarkShader = s;
        pos.set(p);
        T = Tri;
    }
    virtual void run()
    {
        //добавить отметку на материале
        GEnv.Render->add_StaticWallmark(pWallmarkShader, pos, 0.09f, T, Level().ObjectSpace.GetStaticVerts());
    };
    virtual bool obsolete() const { return false; }
};

static void play_object(dxGeomUserData* data, SGameMtlPair* mtl_pair, const dContactGeom* c)
{
    VERIFY(data);
    VERIFY(mtl_pair);
    VERIFY(c);

    auto* phShell = static_cast<CPhysicsShellHolder*>(data->ph_ref_object);
    if (!phShell->m_pPhysicsShell && !phShell->character_physics_support() || phShell->getDestroy())
        return;

    if (CPHSoundPlayer* sp = phShell->ph_sound_player())
        sp->Play(mtl_pair, *(Fvector*)c->pos);
}

template <class Pars>
IC bool play_liquid_particle_criteria(dxGeomUserData& data, float vel_cret)
{
    if (vel_cret > Pars::vel_cret_particles)
        return true;

    bool controller = !!data.ph_object && data.ph_object->CastType() == CPHObject::tpCharacter;

    return !controller && vel_cret > Pars::vel_cret_particles / 4.f;

    // return false;
    // if( !data.ph_ref_object || !data.ph_ref_object->ObjectPPhysicsShell() )
    //	return false;
    // if( data.ph_ref_object->ObjectPPhysicsShell()->HasTracedGeoms() )
    //	return false;
}

template <class Pars>
void play_particles(float vel_cret, dxGeomUserData* data, const dContactGeom* c, bool b_invert_normal,
    const SGameMtl* static_mtl, LPCSTR ps_name)
{
    VERIFY(c);
    VERIFY(static_mtl);
    bool liquid = !!static_mtl->Flags.test(SGameMtl::flLiquid);

    bool play_liquid = liquid && play_liquid_particle_criteria<Pars>(*data, vel_cret);
    bool play_not_liquid = !liquid && vel_cret > Pars::vel_cret_particles;

    if (play_not_liquid)
        Level().ph_commander().add_call(xr_new<CPHOnesCondition>(), xr_new<CPHParticlesPlayCall>(*c, b_invert_normal, ps_name));
    else if (play_liquid)
    {
        CPHFindLiquidParticlesComparer find(cast_fv(c->pos));
        if (!Level().ph_commander().has_call(&find, &find))
            Level().ph_commander().add_call(
                xr_new<CPHLiquidParticlesCondition>(), xr_new<CPHLiquidParticlesPlayCall>(*c, b_invert_normal, ps_name));
    }
}

template <class Pars>
void TContactShotMark(CDB::TRI* T, dContactGeom* c)
{
    dxGeomUserData* data = 0;
    float vel_cret = 0;
    bool b_invert_normal = false;
    if (!ContactShotMarkGetEffectPars(c, data, vel_cret, b_invert_normal))
        return;
    Fvector to_camera;
    to_camera.sub(cast_fv(c->pos), Device.vCameraPosition);
    float square_cam_dist = to_camera.square_magnitude();
    if (data)
    {
        SGameMtlPair* mtl_pair = GMLib.GetMaterialPairByIndices(T->material, data->material);
        if (mtl_pair)
        {
            // Ripples: a body landing in water (a liquid material) or on a rain puddle spreads
            // a ring, its radius growing with the impact. Characters make theirs per footstep
            // (step_manager.cpp), not per capsule contact; Environment::water_hit folds the
            // burst of contacts of one landing into one ring.
            {
                const auto& wcfg = da_water_impact_cfg();
                const bool character = data->ph_object && data->ph_object->CastType() == CPHObject::tpCharacter;
                extern ENGINE_API int ps_e_wind_dbg;
                if (wcfg.enabled && !character && vel_cret > wcfg.ring_object_threshold &&
                    square_cam_dist < wcfg.ring_distance * wcfg.ring_distance)
                {
                    Fvector surface;
                    const bool water = da_water_surface(cast_fv(c->pos), int(T->material), 1.f, surface);
                    // wind_dbg: every body impact near the camera, so a missing ring can be
                    // read as "not water here" or "too soft" instead of guessed.
                    if (ps_e_wind_dbg && square_cam_dist < 225.f)
                        Msg("* [water] body contact vel=%.1f water=%d at (%.0f, %.0f, %.0f)", vel_cret, water ? 1 : 0,
                            c->pos[0], c->pos[1], c->pos[2]);
                    if (water)
                    {
                        const float k = clampr(vel_cret / wcfg.ring_object_velocity, 0.f, 1.f);
                        g_pGamePersistent->Environment().water_hit(surface,
                            wcfg.ring_radius_object_min + (wcfg.ring_radius_object_max - wcfg.ring_radius_object_min) * k,
                            CEnvironment::EWaterHit::ring);
                        // Something broke the surface: the ring is the disturbance, this is
                        // the water it threw. Deferred through the commander like every other
                        // particle here - this runs inside the contact callback - and through
                        // the liquid comparer, so one landing's burst of contacts yields one
                        // splash. The contact is copied and moved onto the surface: the real
                        // one is against the bed, which in a deep pond is metres down.
                        const auto& acfg = da_water_actor_cfg();
                        dContactGeom sc = *c;
                        sc.pos[0] = surface.x;
                        sc.pos[1] = surface.y;
                        sc.pos[2] = surface.z;
                        sc.normal[0] = 0.f;
                        sc.normal[1] = 1.f;
                        sc.normal[2] = 0.f;
                        // The name is owned by the static config, so the raw pointer the call
                        // keeps outlives it - the same contract the material pairs rely on.
                        LPCSTR splash = (k > 0.5f ? acfg.ps_entry_big : acfg.ps_entry).c_str();
                        CPHFindLiquidParticlesComparer find(surface);
                        if (!Level().ph_commander().has_call(&find, &find))
                            Level().ph_commander().add_call(xr_new<CPHLiquidParticlesCondition>(),
                                xr_new<CPHLiquidParticlesPlayCall>(sc, false, splash));
                    }
                }
            }
            if (vel_cret > Pars::vel_cret_wallmark && !mtl_pair->CollideMarks->empty())
            {
                wm_shader WallmarkShader = mtl_pair->CollideMarks->GenerateWallmark();
                Level().ph_commander().add_call(
                    xr_new<CPHOnesCondition>(), xr_new<CPHWallMarksCall>(*((Fvector*)c->pos), T, WallmarkShader));
            }
            if (square_cam_dist < SQUARE_SOUND_EFFECT_DIST)
            {
                SGameMtl* static_mtl = GMLib.GetMaterialByIdx(T->material);
                VERIFY(static_mtl);
                if (!static_mtl->Flags.test(SGameMtl::flPassable))
                {
                    if (vel_cret > Pars::vel_cret_sound)
                    {
                        if (!mtl_pair->CollideSounds.empty())
                        {
                            float volume = collide_volume_min +
                                vel_cret * (collide_volume_max - collide_volume_min) /
                                    (_sqrt(mass_limit) * default_l_limit - Pars::vel_cret_sound);
                            ref_sound& randSound =
                                mtl_pair->CollideSounds[Random.randI(mtl_pair->CollideSounds.size())];
                            randSound.play_no_feedback(0, 0, 0, ((Fvector*)c->pos), &volume);
                        }
                    }
                }
                else
                {
                    if (data->ph_ref_object && !mtl_pair->CollideSounds.empty())
                    {
                        play_object(data, mtl_pair, c);
                    }
                }
            }
            if (square_cam_dist < SQUARE_PARTICLE_EFFECT_DIST && !mtl_pair->CollideParticles.empty())
            {
                SGameMtl* static_mtl = GMLib.GetMaterialByIdx(T->material);
                VERIFY(static_mtl);
                LPCSTR ps_name = mtl_pair->CollideParticles[::Random.randI(0, mtl_pair->CollideParticles.size())].c_str();
                play_particles<Pars>(vel_cret, data, c, b_invert_normal, static_mtl, ps_name);
            }
        }
    }
}

ContactCallbackFun* ContactShotMark = &TContactShotMark<EffectPars>;
ContactCallbackFun* CharacterContactShotMark = &TContactShotMark<CharacterEffectPars>;
