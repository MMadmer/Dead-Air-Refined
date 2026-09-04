#pragma once

// The ledge climb moves the actor by hand; a ladder next to it would grab him mid-climb. Read
// by the physics side, set from game scripts (game.set_actor_allow_ladder).
extern XRPHYSICS_API bool g_da_actor_allow_ladder;

struct XRPHYSICS_API ph_console
{
    static BOOL g_bDebugDumpPhysicsStep; //= 0;
    static float ph_tri_query_ex_aabb_rate; //= 1.3f;
    static int ph_tri_clear_disable_count; //= 10;
    static float phBreakCommonFactor; //= 0.01f;
    static float phRigidBreakWeaponFactor; //= 1.f;
    static float ph_step_time; //=fixed_step;
};
