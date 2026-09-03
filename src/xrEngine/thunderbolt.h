// Rain.h: interface for the CRain class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "Include/xrRender/FactoryPtr.h"
#include "Include/xrRender/LensFlareRender.h"
#include "Include/xrRender/ThunderboltDescRender.h"
#include "Include/xrRender/ThunderboltRender.h"

// refs
class ENGINE_API IRender_DetailModel;
class ENGINE_API CLAItem;
class ENGINE_API CEnvDescriptorMixer;

namespace xray::render
{
namespace render_r4
{
class dxThunderboltRender;
}
namespace render_gl
{
class dxThunderboltRender;
}
} // namespace xray::render

struct ENGINE_API SThunderboltDesc
{
    // geom
    // IRender_DetailModel* l_model;
    FactoryPtr<IThunderboltDescRender> m_pRender;
    // sound
    ref_sound snd;
    // gradient
    struct SFlare
    {
        float fOpacity{};
        Fvector2 fRadius{};
        shared_str shader;
        shared_str texture;

        FactoryPtr<IFlareRender> m_pFlare;

        SFlare() = default;
        SFlare(float opacity, Fvector2 radius, pcstr sh, pcstr tex)
            : fOpacity(opacity), fRadius(radius), shader(sh), texture(tex)
        {
            m_pFlare->CreateShader(shader.c_str(), texture.c_str());
        }
        ~SFlare()
        {
            m_pFlare->DestroyShader();
        }

        void ed_show_params(); // ImGui editor
        void save(CInifile* config) const;
    };
    SFlare* m_GradientTop;
    SFlare* m_GradientCenter;
    shared_str name;
    CLAItem* color_anim;

public:
    SThunderboltDesc(const CInifile& pIni, shared_str const& sect);
    ~SThunderboltDesc();
    static SFlare* create_gradient(pcstr gradient_name, const CInifile& config, shared_str const& sect);
    void ed_show_params(); // ImGui editor
    void save(CInifile* config) const;
};

struct ENGINE_API SThunderboltCollection
{
    xr_vector<SThunderboltDesc*> palette;
    shared_str section;

    SThunderboltCollection(const shared_str& sect, CInifile const* pIni, CInifile const* thunderbolts);
    ~SThunderboltCollection();

    SThunderboltDesc* GetRandomDesc()
    {
        VERIFY(palette.size() > 0);
        return palette[Random.randI(palette.size())];
    }

    void ed_show_params(); // ImGui editor
    void save(CInifile* config) const;
};

#define THUNDERBOLT_CACHE_SIZE 8
//
class ENGINE_API CEffect_Thunderbolt
{
    friend class xray::render::render_r4::dxThunderboltRender;
    friend class xray::render::render_gl::dxThunderboltRender;

protected:
    xr_vector<SThunderboltCollection*> collections;
    SThunderboltDesc* current;

private:
    Fmatrix current_xform;
    Fvector3 current_direction;

    FactoryPtr<IThunderboltRender> m_pRender;
    // ref_geom hGeom_model;
    // states
    enum EState
    {
        stIdle,
        stWorking
    };
    EState state;

    // ref_geom hGeom_gradient;

    Fvector lightning_center;
    float lightning_size;
    float lightning_phase;
    // The flash as the clouds see it: the colour of this instant, the amount the flash
    // added to the fog colour (so a reader can take it back out), and whether the bolt is
    // hidden inside the cloud - sheet lightning, a glow with no channel to look at.
    Fvector lightning_color{};
    Fvector lightning_fog_add{};
    Fvector lightning_sun_add{};
    Fvector sun_dir_real{0.f, -1.f, 0.f};
    bool bolt_hidden{};
    // The bolt as the eye sees it: the unit direction from the camera to the discharge
    // (current_direction is inverted after the roll so it can stand in for the sun), and
    // what the flash added to the sky colour this frame, so the dome can take it back out.
    Fvector bolt_dir{0.f, 1.f, 0.f};
    Fvector lightning_sky_add{};
    // The channel inside the cloud: a bent line a few kilometres long, its heading, length
    // and bend rolled per discharge - the glow is that shape, not a ball.
    float channel_heading{};
    float channel_length{3000.f};
    float channel_bend{};
    // The storm cell: strikes cluster around a heading that wanders from bolt to bolt, so
    // they are neither all in one spot nor scattered like dice. Sheet lightning flickers:
    // a few pulses at rolled times inside a longer life.
    float storm_heading{};
    bool storm_heading_set{};
    int pulse_count{};
    float pulse_t[5]{};
    float pulse_w[5]{};

    float life_time;
    float current_time;
    float next_lightning_time;
    bool bEnabled;

    CInifile* m_thunderbolt_collections_config{};
    CInifile* m_thunderbolts_config{};

    // params
    static constexpr float MAX_DIST_FACTOR = 0.95f;
    Fvector2 p_var_alt;
    float p_var_long;
    float p_min_dist;
    float p_tilt;
    float p_second_prop;
    float p_sky_color;
    float p_sun_color;
    float p_fog_color;

private:
    static bool RayPick(const Fvector& s, const Fvector& d, float& range);
    void Bolt(const CEnvDescriptorMixer& currentEnv);

public:
    bool lightning_active() const { return state == stWorking; }
    const Fvector& lightning_direction() const { return bolt_dir; }
    // The flash's brightness lives in its colour (the colour animation, the flicker
    // envelope); lightning_phase only drives the channel texture's frames.
    float lightning_intensity() const { return state == stWorking ? 1.f : 0.f; }
    bool bolt_is_hidden() const { return bolt_hidden; }
    const Fvector& lightning_sky_added() const { return lightning_sky_add; }
    const Fvector& lightning_colour() const { return lightning_color; }
    const Fvector& lightning_fog_added() const { return lightning_fog_add; }
    const Fvector& lightning_sun_added() const { return lightning_sun_add; }
    const Fvector& sun_direction_real() const { return sun_dir_real; }
    float channel_heading_rad() const { return channel_heading; }
    float channel_length_m() const { return channel_length; }
    float channel_bend_k() const { return channel_bend; }

    CEffect_Thunderbolt();
    ~CEffect_Thunderbolt();

    void OnFrame(CEnvDescriptorMixer& currentEnv);
    void Render();

    SThunderboltCollection* AppendDef(const shared_str& sect);

    [[nodiscard]]
    auto& GetCollections() { return collections; }

    void ED_ShowParams(); // ImGui editor
    void save();
};
