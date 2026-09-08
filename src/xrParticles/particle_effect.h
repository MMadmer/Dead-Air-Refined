#pragma once

namespace PAPI
{
// A effect of particles - Info and an array of Particles
struct PARTICLES_API ParticleEffect
{
    u32 p_count; // Number of particles currently existing.
    u32 max_particles; // Max particles allowed in effect.
    u32 particles_allocated; // Actual allocated size.
    Particle* particles; // Actually, num_particles in size
    OnBirthParticleCB b_cb;
    OnDeadParticleCB d_cb;
    void* owner;
    u32 param;
    // The wind this effect drifts in (m/s, world), sampled by the engine's wind service at
    // the effect's position a few times a second (PAMove). Not serialised - it is weather.
    pVector wind{};
    float wind_stamp{-1.f};
    // How much of that wind the particles take (0 = none): smoke and dust drift, a flame
    // sprite, a muzzle flash or a ring on water stay where they were authored. Set from the
    // effect definition (see CPEDef::m_WindScale); an effect no rule names gets zero.
    float wind_scale{};

    ParticleEffect(int mp);

    ~ParticleEffect();

    int Resize(u32 max_count);

    void Remove(int i);

    bool Add(const pVector& pos, const pVector& posB, const pVector& size, const pVector& rot, const pVector& vel,
             u32 color, const float age = 0.0f, u16 frame = 0, u16 flags = 0);
};
} // namespace PAPI
