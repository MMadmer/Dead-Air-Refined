#include "stdafx.h"

#include "da_fire.h"
#include "ParticleEffectDef.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/Environment.h"
#include "xrCDB/xrCDB.h"
#if defined(USE_DX11)
#include "Layers/xrRenderDX11/3DFluid/dx113DFluidData.h"
#include "Layers/xrRenderDX11/3DFluid/dx113DFluidManager.h"
#endif

#include <algorithm>

namespace xray::render::RENDER_NAMESPACE::PS
{
namespace
{
xr_vector<SDaFirePreset> g_presets;
bool g_presets_loaded = false;

// Every [shader_fire_<name>] and [shader_blast_<name>] section of dead_air_x64_fire.ltx is a
// preset; the blast sections carry the same combustion keys and their own source.
void load_presets()
{
    g_presets_loaded = true;
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_fire.ltx");
    if (!FS.exist(path))
        return;
    CInifile ini(path, TRUE);
    constexpr cpcstr prefix = "shader_fire_";
    constexpr cpcstr prefix_blast = "shader_blast_";
    const size_t prefix_len = xr_strlen(prefix);
    const size_t prefix_blast_len = xr_strlen(prefix_blast);
    for (const CInifile::Sect* sect : ini.sections())
    {
        cpcstr n = sect->Name.c_str();
        const bool is_blast = 0 == strncmp(n, prefix_blast, prefix_blast_len);
        if (!is_blast && strncmp(n, prefix, prefix_len) != 0)
            continue;
        SDaFirePreset p;
        p.blast = is_blast;
        p.name = n + (is_blast ? prefix_blast_len : prefix_len);
        const auto rf = [&](cpcstr key, float def) { return ini.line_exist(n, key) ? ini.r_float(n, key) : def; };
        p.base_height = rf("base_height", p.base_height);
        p.radius = std::max(0.05f, rf("radius", p.radius));
        p.height = std::max(0.1f, rf("height", p.height));
        p.intensity = rf("intensity", p.intensity);
        p.lean = rf("lean", p.lean);
        p.smoke = ini.line_exist(n, "smoke") ? ini.r_bool(n, "smoke") : p.smoke;
        p.smoke_rate = rf("smoke_rate", p.smoke_rate);
        p.smoke_life = std::max(1.f, rf("smoke_life", p.smoke_life));
        p.smoke_alpha = clampr(rf("smoke_alpha", p.smoke_alpha), 0.f, 1.f);
        p.smoke_size = std::max(0.05f, rf("smoke_size", p.smoke_size));
        p.smoke_grey = clampr(rf("smoke_grey", p.smoke_grey), 0.f, 1.f);
        p.heat_kw = std::max(5.f, rf("heat_kw", p.heat_kw));
        p.fluid = ini.line_exist(n, "fluid") ? ini.r_bool(n, "fluid") : p.fluid;
        p.fl_base = rf("fluid_base", p.fl_base);
        p.fl_bed = std::max(0.05f, rf("fluid_bed", p.fl_bed));
        p.fl_radius = rf("fluid_radius", p.fl_radius);
        p.fl_cell = clampr(rf("fluid_cell", p.fl_cell), 0.015f, 0.12f);
        p.fl_ignition = rf("fluid_ignition", p.fl_ignition);
        p.fl_burn = rf("fluid_burn", p.fl_burn);
        p.fl_fuel_per_burn = std::max(0.01f, rf("fluid_fuel_per_burn", p.fl_fuel_per_burn));
        p.fl_t_per_burn = rf("fluid_t_per_burn", p.fl_t_per_burn);
        p.fl_smoke_per_burn = rf("fluid_smoke_per_burn", p.fl_smoke_per_burn);
        p.fl_cooling = rf("fluid_cooling", p.fl_cooling);
        p.fl_fuel = rf("fluid_fuel", p.fl_fuel);
        p.fl_couple = rf("fluid_couple", p.fl_couple);
        p.fl_expansion = rf("fluid_expansion", p.fl_expansion);
        p.fl_buoyancy = rf("fluid_buoyancy", p.fl_buoyancy);
        p.fl_inject = rf("fluid_inject", p.fl_inject);
        p.fl_wind_relax = rf("fluid_wind_relax", p.fl_wind_relax);
        p.fl_vort = rf("fluid_vorticity", p.fl_vort);
        p.fl_smoke_fade = rf("fluid_smoke_fade", p.fl_smoke_fade);
        p.fl_vel_damp = rf("fluid_vel_damp", p.fl_vel_damp);
        p.fl_iterations = int(clampr(rf("fluid_iterations", float(p.fl_iterations)), 2.f, 48.f));
        p.fl_emission = rf("fluid_emission", p.fl_emission);
        p.fl_absorb = rf("fluid_absorb", p.fl_absorb);
        p.fl_albedo = rf("fluid_albedo", p.fl_albedo);
        p.fl_ember = rf("fluid_ember", p.fl_ember);
        p.fl_core_t = std::max(0.1f, rf("fluid_core_t", p.fl_core_t));
        p.fl_smoke_gain = rf("fluid_smoke_gain", p.fl_smoke_gain);
        p.fl_absorb_hot = clampr(rf("fluid_absorb_hot", p.fl_absorb_hot), 0.f, 1.f);
        p.fl_shadow = rf("fluid_shadow", p.fl_shadow);
        p.fl_shadow_step = std::max(0.1f, rf("fluid_shadow_step", p.fl_shadow_step));
        p.fl_lift = rf("fluid_smoke_lift", p.fl_lift);
        p.fl_douse = rf("fluid_douse", p.fl_douse);
        p.fl_smoulder = rf("fluid_smoulder", p.fl_smoulder);
        p.fl_smoulder_soot = rf("fluid_smoulder_soot", p.fl_smoulder_soot);
        p.fl_smoulder_rate = rf("fluid_smoulder_rate", p.fl_smoulder_rate);
        p.fl_emis_pow = std::max(0.5f, rf("fluid_emission_pow", p.fl_emis_pow));
        p.fl_edge_fade = std::max(1.f, rf("fluid_edge_fade", p.fl_edge_fade));
        p.fl_drain_band = std::max(1.f, rf("fluid_drain_band", p.fl_drain_band));
        p.fl_drain = clampr(rf("fluid_drain", p.fl_drain), 0.5f, 1.f);
        p.bl_radius = std::max(0.1f, rf("blast_radius", p.bl_radius));
        p.bl_lift = rf("blast_lift", p.bl_lift);
        p.bl_duration = std::max(0.3f, rf("blast_duration", p.bl_duration));
        p.bl_inject = std::max(0.01f, rf("blast_inject", p.bl_inject));
        p.bl_speed = rf("blast_speed", p.bl_speed);
        p.bl_cell = clampr(rf("blast_cell", p.bl_cell), 0.02f, 0.25f);
        p.bl_ground = rf("blast_ground", p.bl_ground);
        p.bl_pilot = std::max(0.05f, rf("blast_pilot", p.bl_pilot));
        p.bl_ring = rf("blast_ring", p.bl_ring);
        p.bl_dust = rf("blast_dust", p.bl_dust);
        p.bl_div = rf("blast_divergence", p.bl_div);
        p.bl_div_tau = std::max(0.01f, rf("blast_divergence_tau", p.bl_div_tau));
        p.bl_div_neg = rf("blast_divergence_neg", p.bl_div_neg);
        p.bl_div_neg_tau = std::max(0.01f, rf("blast_divergence_neg_tau", p.bl_div_neg_tau));
        p.bl_fade = std::max(0.05f, rf("blast_fade", p.bl_fade));
        p.bl_wind_floor = clampr(rf("blast_wind_floor", p.bl_wind_floor), 0.f, 1.f);
        g_presets.push_back(p);
    }
}

// Smoke billboards carry the puff's noise seed, age, the fire's light on it and its radius
// beside the usual position, colour and corner.
struct SSmokeVertex
{
    Fvector p;
    u32 c;
    Fvector2 t;
    Fvector4 e;
};

VertexElement smoke_decl[] =
{
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
    D3DDECL_END()
};

// Only the fire nearest the camera runs on the grid; the rest fall back to the marched
// flame. The winner of one frame is the owner of the next, which costs a frame at a fire's
// first appearance and saves an ordering problem between effects.
CDaFireEffect* g_fluid_owner = nullptr;
CDaFireEffect* g_fluid_claim = nullptr;
float g_fluid_claim_d2 = flt_max;
u32 g_fluid_frame = u32(-1);

float axis_of(const Fvector& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

// Sutherland-Hodgman against one axis-aligned plane, so a terrain triangle metres across is
// cut down to the piece that is actually inside the box before it is sampled.
void clip_axis(xr_vector<Fvector>& poly, xr_vector<Fvector>& tmp, int axis, float value, bool keepGreater)
{
    tmp.clear();
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i)
    {
        const Fvector& A = poly[i];
        const Fvector& B = poly[(i + 1) % n];
        const float da = keepGreater ? axis_of(A, axis) - value : value - axis_of(A, axis);
        const float db = keepGreater ? axis_of(B, axis) - value : value - axis_of(B, axis);
        if (da >= 0.f)
            tmp.push_back(A);
        if ((da >= 0.f) != (db >= 0.f))
        {
            Fvector P;
            P.lerp(A, B, da / (da - db));
            tmp.push_back(P);
        }
    }
    poly.swap(tmp);
}

// The simulation runs at a fixed rate; every rate and length in a preset is converted with it.
constexpr float g_fluid_step = 1.f / 60.f;

constexpr float g_gravity = 9.81f;
// Wood burns at about 0.025 kg/m2s; the AGA tilt correlation measures the wind against the
// characteristic velocity u_c = (g m'' D / rho_air)^(1/3), ~0.5 m/s for a campfire.
constexpr float g_burn_rate = 0.025f;
constexpr float g_rho_air = 1.2f;

// A puff's quad in the QuadIB corner order (bottom-left, top-left, bottom-right, top-right).
void fill_quad(SSmokeVertex*& pv, const Fvector& c, const Fvector& right, const Fvector& top, u32 clr,
    const Fvector4& extra)
{
    Fvector a, b;
    a.sub(top, right);
    b.add(top, right);
    Fvector p;
    p.sub(c, b);
    pv->p = p; pv->c = clr; pv->t.set(0.f, 1.f); pv->e = extra; ++pv;
    p.add(c, a);
    pv->p = p; pv->c = clr; pv->t.set(0.f, 0.f); pv->e = extra; ++pv;
    p.sub(c, a);
    pv->p = p; pv->c = clr; pv->t.set(1.f, 1.f); pv->e = extra; ++pv;
    p.add(c, b);
    pv->p = p; pv->c = clr; pv->t.set(1.f, 0.f); pv->e = extra; ++pv;
}
} // namespace

const SDaFirePreset* SDaFirePreset::find(const shared_str& name)
{
    if (!g_presets_loaded)
        load_presets();
    if (!name.size() || 0 == xr_strcmp(name.c_str(), "off"))
        return nullptr;
    for (const SDaFirePreset& p : g_presets)
        if (0 == xr_strcmp(p.name.c_str(), name.c_str()))
            return &p;
    Msg("! [fire] shader fire preset [%s] is not in dead_air_x64_fire.ltx, the effect draws nothing", name.c_str());
    return nullptr;
}

CDaFireEffect::CDaFireEffect() = default;

CDaFireEffect::~CDaFireEffect()
{
    if (g_fluid_owner == this)
        g_fluid_owner = nullptr;
    if (g_fluid_claim == this)
        g_fluid_claim = nullptr;
    fluid_destroy();
    CDaFireEffect::OnDeviceDestroy();
}

// The grid box: the fire's own metre-scale piece of the world, wide enough for the flame and
// tall enough for the first couple of metres of plume. The transform's scale has to be uniform
// and equal to one cell times the largest grid dimension - the renderer folds the grid's own
// aspect ratio in afterwards.
void CDaFireEffect::fluid_create()
{
#if defined(USE_DX11)
    if (m_fluid || !m_preset)
        return;
    //  Building it is expensive; if it cannot be built yet, wait rather than try every frame.
    if (m_fluid_retry > 0.f)
    {
        m_fluid_retry -= Device.fTimeDelta;
        return;
    }
    m_fluid_retry = 2.f;
    const int W = FluidManager.GetTextureWidth();
    const int H = FluidManager.GetTextureHeight();
    const int D = FluidManager.GetTextureDepth();
    if (W <= 0 || H <= 0 || D <= 0)
        return;

    const bool blast = m_preset->blast;
    m_fluid_cell = blast ? m_preset->bl_cell : m_preset->fl_cell;
    const int maxDim = _max(W, _max(H, D));
    const Fvector O = origin();
    const float srcY = O.y + (blast ? m_preset->bl_lift : m_preset->fl_base);
    //  The floor of the box sits below the source so the ground under it - and the way the
    //  fireball spreads along that ground - is inside; the rest is headroom for the plume.
    const float below = blast ? (m_preset->bl_radius + 0.5f) : 0.6f;
    m_fluid_centre.set(O.x, srcY - below + m_fluid_cell * float(H) * 0.5f, O.z);

    //  A blast has two seconds to live and cannot afford a triangle sweep of the level; the
    //  ground under it is what shapes it, and that is a grid of downward rays.
    if (blast ? !fluid_ground() : !fluid_voxelize())
        return;

    Fmatrix scale, translate, transform;
    const float s = m_fluid_cell * float(maxDim);
    scale.scale(s, s, s);
    translate.translate(m_fluid_centre);
    transform.mul(translate, scale);

    dx113DFluidData::Settings settings;
    settings.m_SimulationType = dx113DFluidData::ST_DA_FIRE;
    settings.m_fHemi = 0.35f;
    settings.m_fConfinementScale = m_preset->fl_vort;
    //  Every channel fades on its own terms inside the shader, so nothing is scaled here.
    settings.m_fDecay = 1.f;
    settings.m_fGravityBuoyancy = 0.f;

    m_fluid = std::make_unique<dx113DFluidData>();
    m_fluid->InitProcedural(transform, settings);
    m_fluid->SetStaticObstacles(m_fluid_obst);
    m_fluid_retry = 0.f;
    m_fluid_time = 0.f;
    m_fluid_acc = 0.f;
    fluid_params();
    if (blast)
        Msg("* [fire] blast [%s] from [%s], %.2f m/cell, box %.1f x %.1f m at %.1f %.1f %.1f",
            m_preset->name.c_str(), (m_Def && m_Def->Name()) ? m_Def->Name() : "?", m_fluid_cell,
            m_fluid_cell * float(W), m_fluid_cell * float(H), O.x, srcY, O.z);
#endif
}

// The level itself, turned into the occupancy volume the simulation treats as solid. The wood,
// the ground and anything else standing in the box are all just cells here, which is what lets
// the flame's base follow whatever it is actually burning on.
bool CDaFireEffect::fluid_voxelize()
{
#if defined(USE_DX11)
    if (!g_pGameLevel)
        return false;
    const CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    if (!model)
        return false;

    const int W = FluidManager.GetTextureWidth();
    const int H = FluidManager.GetTextureHeight();
    const int D = FluidManager.GetTextureDepth();
    const float cell = m_fluid_cell;
    const Fvector C = m_fluid_centre;
    Fvector half;
    half.set(cell * float(W) * 0.5f, cell * float(H) * 0.5f, cell * float(D) * 0.5f);

    xr_vector<u8> vox(size_t(W) * size_t(H) * size_t(D), 0);

    CDB::COLLIDER xrc;
    xrc.r_clear();
    xrc.box_query(CDB::OPT_FULL_TEST, model, C, half);
    const size_t hits = xrc.r_count();
    if (!hits)
        return false;

    const CDB::TRI* tris = model->get_tris();
    const Fvector* verts = model->get_verts();
    const CDB::RESULT* rb = xrc.r_begin();

    xr_vector<Fvector> poly, tmp;
    poly.reserve(16);
    tmp.reserve(16);

    Fvector lo, hi;
    lo.sub(C, half);
    hi.add(C, half);

    size_t marked = 0;
    for (size_t t = 0; t < hits; ++t)
    {
        const CDB::TRI& tri = tris[rb[t].id];
        poly.clear();
        poly.push_back(verts[tri.verts[0]]);
        poly.push_back(verts[tri.verts[1]]);
        poly.push_back(verts[tri.verts[2]]);
        for (int a = 0; a < 3 && poly.size() >= 3; ++a)
        {
            clip_axis(poly, tmp, a, axis_of(lo, a), true);
            if (poly.size() >= 3)
                clip_axis(poly, tmp, a, axis_of(hi, a), false);
        }
        if (poly.size() < 3)
            continue;

        //  Fan the clipped polygon and splat each piece a little denser than one sample per
        //  cell, so nothing thin slips between two samples.
        for (size_t f = 2; f < poly.size(); ++f)
        {
            const Fvector& a = poly[0];
            const Fvector& b = poly[f - 1];
            const Fvector& c = poly[f];
            const float e = _max(a.distance_to(b), _max(a.distance_to(c), b.distance_to(c)));
            const int n = clampr(iCeil(e / (cell * 0.4f)), 1, 320);
            const float inv = 1.f / float(n);
            for (int i = 0; i <= n; ++i)
            {
                for (int j = 0; j <= n - i; ++j)
                {
                    const float u = float(i) * inv;
                    const float v = float(j) * inv;
                    Fvector p;
                    p.x = a.x + (b.x - a.x) * u + (c.x - a.x) * v;
                    p.y = a.y + (b.y - a.y) * u + (c.y - a.y) * v;
                    p.z = a.z + (b.z - a.z) * u + (c.z - a.z) * v;
                    //  Grid y runs downward, and the depth slices are sampled half a texel in.
                    const int cx = iFloor((p.x - C.x) / cell + float(W) * 0.5f);
                    const int cy = iFloor(float(H) * 0.5f - (p.y - C.y) / cell);
                    const int cz = iFloor((p.z - C.z) / cell + float(D) * 0.5f - 0.5f);
                    if (cx < 0 || cx >= W || cy < 0 || cy >= H || cz < 0 || cz >= D)
                        continue;
                    u8& b8 = vox[size_t(cz) * size_t(W) * size_t(H) + size_t(cy) * size_t(W) + size_t(cx)];
                    if (!b8)
                    {
                        b8 = 255;
                        ++marked;
                    }
                }
            }
        }
    }
    xrc.r_clear();

    if (!marked)
        return false;

    D3D_TEXTURE3D_DESC desc{};
    desc.Width = W;
    desc.Height = H;
    desc.Depth = D;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Usage = D3D_USAGE_IMMUTABLE;
    desc.BindFlags = D3D_BIND_SHADER_RESOURCE;

    D3D_SUBRESOURCE_DATA init{};
    init.pSysMem = vox.data();
    init.SysMemPitch = UINT(W);
    init.SysMemSlicePitch = UINT(W * H);

    _RELEASE(m_fluid_obst);
    if (FAILED(HW.pDevice->CreateTexture3D(&desc, &init, &m_fluid_obst)))
        return false;
    //  How much of the bed the flame can actually take hold on: an empty cell with a solid one
    //  within two below it, inside the disc and the bed's height. A pile of sticks at this
    //  resolution can leave almost none, and then there is nowhere for a flame to stand.
    {
        const float rad = ((m_preset->fl_radius > 0.f) ? m_preset->fl_radius : m_preset->radius * 1.15f) / cell;
        const float bed = m_preset->fl_bed / cell;
        const Fvector O = origin();
        const float sx = (O.x - C.x) / cell + float(W) * 0.5f;
        const float sy = float(H) * 0.5f - (O.y + m_preset->fl_base - C.y) / cell;
        const float sz = (O.z - C.z) / cell + float(D) * 0.5f - 0.5f;
        const auto solid = [&](int x, int y, int z) {
            return x >= 0 && x < W && y >= 0 && y < H && z >= 0 && z < D &&
                vox[size_t(z) * size_t(W) * size_t(H) + size_t(y) * size_t(W) + size_t(x)] != 0;
        };
        int anchors = 0;
        int topY = H;
        for (int z = 0; z < D; ++z)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const float dx = float(x) - sx, dz = float(z) - sz, dy = float(y) - sy;
                    if (dx * dx + dz * dz > rad * rad || _abs(dy) > bed || solid(x, y, z))
                        continue;
                    if (solid(x, y + 1, z) || solid(x, y + 2, z))
                    {
                        ++anchors;
                        if (y < topY)
                            topY = y;
                    }
                }
        const float topWorld = (topY < H) ? (C.y + cell * (float(H) * 0.5f - float(topY))) : O.y;
        Msg("* [fire] fluid campfire at %.1f %.1f %.1f: %u solid cell(s), %d anchor(s), fuel bed up to %+.2f m",
            O.x, O.y, O.z, u32(marked), anchors, topWorld - O.y);
    }
    return true;
#else
    return false;
#endif
}

// The ground under a blast, as occupancy: one downward ray every other column, and every cell
// below the height it found is solid. A fireball is shaped by the surface it is born on far
// more than by anything standing around it, and this costs a fraction of a triangle sweep.
bool CDaFireEffect::fluid_ground()
{
#if defined(USE_DX11)
    if (!g_pGameLevel)
        return false;
    const CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    if (!model)
        return false;

    const int W = FluidManager.GetTextureWidth();
    const int H = FluidManager.GetTextureHeight();
    const int D = FluidManager.GetTextureDepth();
    const float cell = m_fluid_cell;
    const Fvector C = m_fluid_centre;
    const float top = C.y + cell * float(H) * 0.5f;
    const float span = cell * float(H);

    constexpr int step = 2;
    const int nx = W / step + 1;
    const int nz = D / step + 1;
    xr_vector<float> height(size_t(nx) * size_t(nz), -flt_max);

    CDB::COLLIDER xrc;
    const Fvector down{0.f, -1.f, 0.f};
    for (int zi = 0; zi < nz; ++zi)
    {
        for (int xi = 0; xi < nx; ++xi)
        {
            Fvector from;
            from.set(C.x + cell * (float(xi * step) - float(W) * 0.5f), top,
                C.z + cell * (float(zi * step) + 0.5f - float(D) * 0.5f));
            xrc.r_clear();
            xrc.ray_query(CDB::OPT_ONLYNEAREST, model, from, down, span);
            if (xrc.r_count())
                height[size_t(zi) * size_t(nx) + size_t(xi)] = top - xrc.r_begin()->range;
        }
    }
    xrc.r_clear();

    xr_vector<u8> vox(size_t(W) * size_t(H) * size_t(D), 0);
    size_t marked = 0;
    for (int z = 0; z < D; ++z)
    {
        const int zi = _min(z / step, nz - 1);
        for (int x = 0; x < W; ++x)
        {
            const float h = height[size_t(zi) * size_t(nx) + size_t(_min(x / step, nx - 1))];
            if (h == -flt_max)
                continue;
            //  Grid y runs downward: everything from the ground down is solid.
            const int y0 = iCeil(float(H) * 0.5f - (h - C.y) / cell);
            for (int y = _max(0, y0); y < H; ++y)
            {
                vox[size_t(z) * size_t(W) * size_t(H) + size_t(y) * size_t(W) + size_t(x)] = 255;
                ++marked;
            }
        }
    }
    //  No ground inside the box is a perfectly good answer - a charge can go off on a roof,
    //  over a drop, or in mid air. The volume is then simply all air, and the fireball is a
    //  free one. What must not happen is the effect being cancelled for it.

    D3D_TEXTURE3D_DESC desc{};
    desc.Width = W;
    desc.Height = H;
    desc.Depth = D;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Usage = D3D_USAGE_IMMUTABLE;
    desc.BindFlags = D3D_BIND_SHADER_RESOURCE;

    D3D_SUBRESOURCE_DATA init{};
    init.pSysMem = vox.data();
    init.SysMemPitch = UINT(W);
    init.SysMemSlicePitch = UINT(W * H);

    _RELEASE(m_fluid_obst);
    return SUCCEEDED(HW.pDevice->CreateTexture3D(&desc, &init, &m_fluid_obst));
#else
    return false;
#endif
}

// Everything the passes read, converted from metres and seconds into cells and steps.
void CDaFireEffect::fluid_params()
{
#if defined(USE_DX11)
    if (!m_fluid || !m_preset)
        return;
    const SDaFirePreset& P = *m_preset;
    const int W = FluidManager.GetTextureWidth();
    const int H = FluidManager.GetTextureHeight();
    const int D = FluidManager.GetTextureDepth();
    const float cell = m_fluid_cell;
    const float h = g_fluid_step;
    const Fvector C = m_fluid_centre;
    const Fvector O = origin();

    dx113DFluidData::Settings settings = m_fluid->GetSettings();
    dx113DFluidData::DaFireParams& f = settings.m_DaFire;

    const float srcY = O.y + (P.blast ? P.bl_lift : P.fl_base);
    f.m_vSource.set((O.x - C.x) / cell + float(W) * 0.5f, float(H) * 0.5f - (srcY - C.y) / cell,
        (O.z - C.z) / cell + float(D) * 0.5f - 0.5f);
    f.m_fRadius = ((P.fl_radius > 0.f) ? P.fl_radius : P.radius * 1.15f) / cell;
    f.m_fBedRange = P.fl_bed / cell;

    //  A blast throws its charge in over the first moments and then only burns what it has.
    //  The envelope is squared so the first frames get most of it.
    if (P.blast)
    {
        const float k = 1.f - clampr(m_blast_t / P.bl_inject, 0.f, 1.f);
        f.m_fBlastInject = k * k;
        f.m_fBlastRadius = P.bl_radius / cell;
        f.m_fBlastSpeed = P.bl_speed * h / cell;
        f.m_fBlastPilot = P.bl_pilot;
        f.m_fGroundJet = P.bl_ring;
        f.m_fGroundDust = P.bl_dust;
        //  The ground run outlives the charge by a good half second: that is how long a real
        //  ring keeps travelling before it runs out of the push that started it.
        const float rk = 1.f - clampr(m_blast_t / (P.bl_inject * 4.f), 0.f, 1.f);
        f.m_fRingEnv = rk * rk;
        const float e1 = expf(-m_blast_t / P.bl_div_tau);
        const float e2 = expf(-m_blast_t / P.bl_div_neg_tau);
        f.m_fBlastDiv = (P.bl_div * e1 - P.bl_div * P.bl_div_neg * (1.f - e1) * e2) * h;
        f.m_fWindFloor = P.bl_wind_floor;
    }
    else
    {
        f.m_fBlastInject = 0.f;
        f.m_fBlastRadius = 0.f;
        f.m_fBlastSpeed = 0.f;
        f.m_fBlastPilot = 0.f;
        f.m_fGroundJet = 0.f;
        f.m_fGroundDust = 0.f;
        f.m_fRingEnv = 0.f;
        f.m_fBlastDiv = 0.f;
        f.m_fWindFloor = 0.f;
    }

    f.m_fIgnition = P.fl_ignition;
    f.m_fBurnPerT = P.fl_burn * h;
    f.m_fTPerBurn = P.fl_t_per_burn;
    f.m_fCooling = P.fl_cooling * h;
    f.m_fFuelPerBurn = P.fl_fuel_per_burn;
    f.m_fSmokePerBurn = P.fl_smoke_per_burn;
    f.m_fExpansion = P.fl_expansion;
    //  The one line the whole going-out sequence hangs on: the fuel top-up and the pilot that
    //  keeps the bed alight are the same branch in the shader, so taking this to zero stops
    //  both. Nothing else in the simulation needs to know the fire is going out.
    f.m_fFuelTarget = P.fl_fuel * douse_k();
    f.m_fCouple = P.fl_couple * h;
    //  Soot at the bed with no fuel and no heat behind it - the steaming afterwards.
    f.m_fSmoulder = P.fl_smoulder_soot * smoulder_k();

    //  The wind, on the grid's axes: world up is -y here. A velocity of one means one cell
    //  per step, so metres per second turn into cells with the step and the cell size.
    const float toCells = h / cell;
    f.m_vWind.set(m_wind.x * toCells, -m_wind.y * toCells, m_wind.z * toCells);
    f.m_fWindRelax = P.fl_wind_relax * h;

    //  An acceleration in m/s2 moves a cell-per-step velocity by a*h*h/cell.
    f.m_fBuoyancy = P.fl_buoyancy * h * h / cell;
    f.m_fInject = P.fl_inject * toCells;
    f.m_fTime = m_fluid_time;
    f.m_fSmokeFade = std::max(0.f, 1.f - P.fl_smoke_fade * h);
    f.m_fVelDamp = std::max(0.f, 1.f - P.fl_vel_damp * h);
    //  Cetegen: a pool fire puffs at about 1.5/sqrt(D) Hz.
    f.m_fPuff = 1.5f / _sqrt(_max(0.2f, 2.f * P.radius));
    f.m_iIterations = P.fl_iterations;

    f.m_fEmission = P.fl_emission * P.intensity;
    f.m_fAbsorb = P.fl_absorb;
    f.m_fAlbedo = P.fl_albedo;
    f.m_fEmber = P.fl_ember;
    f.m_fCoreT = P.fl_core_t;
    f.m_fSmokeGain = P.fl_smoke_gain;
    f.m_fFade = m_fluid_fade;
    f.m_fHotAbsorb = P.fl_absorb_hot;

    //  One step toward the sun, in the grid's texture space: the box is an axis-aligned
    //  scale, so a world direction is just divided by the box's size on each axis, and the
    //  texture's y runs the other way from the world's.
    const auto& env = g_pGamePersistent->Environment();
    Fvector sun = env.CurrentEnv.sun_dir;
    sun.invert();
    sun.normalize_safe();
    f.m_vSunStep.set(sun.x * P.fl_shadow_step / (cell * float(W)),
        -sun.y * P.fl_shadow_step / (cell * float(H)), sun.z * P.fl_shadow_step / (cell * float(D)));
    f.m_vSunColor.set(env.CurrentEnv.sun_color.x, env.CurrentEnv.sun_color.y, env.CurrentEnv.sun_color.z);
    f.m_fShadow = P.fl_shadow;
    f.m_fSmokeLift = P.fl_lift * h * h / cell;
    f.m_fEmisPow = P.fl_emis_pow;
    f.m_fEdgeFade = P.fl_edge_fade;
    f.m_fDrainBand = P.fl_drain_band;
    f.m_fDrainRate = P.fl_drain;
    //  The light the flame throws back into its own plume.
    f.m_vFireLight.set(1.f, 0.42f, 0.13f);
    f.m_vFireLight.mul(0.7f * P.intensity);

    //  Confinement on a schedule for a blast: nothing while the charge is still expanding
    //  cleanly, hard through the moments the fireball's surface is breaking into lobes, then
    //  down to a level that keeps the smoke churning without shredding it.
    settings.m_fConfinementScale = P.fl_vort;
    if (P.blast)
    {
        const float ramp = clampr((m_blast_t - 0.02f) / 0.05f, 0.f, 1.f);
        settings.m_fConfinementScale = P.fl_vort * ramp * (0.4f + 0.6f * expf(-m_blast_t / 0.55f));
    }
    m_fluid->SetSettings(settings);
#endif
}

// Where the billboard plume takes over from the grid: the top of the box, carried downwind by
// however far the wind would have pushed it on the way up.
Fvector CDaFireEffect::fluid_plume_start() const
{
    Fvector p = m_fluid_centre;
#if defined(USE_DX11)
    const float top = m_fluid_cell * float(FluidManager.GetTextureHeight()) * 0.5f;
    const float rise = 2.6f;
    p.y += top - 0.12f;
    p.x += m_wind.x * (top / rise);
    p.z += m_wind.z * (top / rise);
#endif
    return p;
}

void CDaFireEffect::fluid_destroy()
{
#if defined(USE_DX11)
    m_fluid.reset();
    _RELEASE(m_fluid_obst);
#endif
}

void CDaFireEffect::fluid_render(CBackend& cmd_list)
{
#if defined(USE_DX11)
    if (!m_fluid)
        return;
    (void)cmd_list;

    //  A fixed step keeps the flame the same shape whatever the frame rate; two steps is the
    //  most we will spend catching up, the rest of the backlog is simply dropped.
    m_fluid_acc += std::min(Device.fTimeDelta, 0.1f);
    int steps = 0;
    while (m_fluid_acc >= g_fluid_step && steps < 2)
    {
        m_fluid_acc -= g_fluid_step;
        m_fluid_time += g_fluid_step;
        ++steps;
        fluid_params();
        FluidManager.Update(*m_fluid, 1.f);
    }
    if (!steps)
        fluid_params();
    FluidManager.RenderFluid(*m_fluid);
#endif
}

BOOL CDaFireEffect::Compile(CPEDef* def)
{
    m_Def = def;
    m_preset = def ? SDaFirePreset::find(def->m_DaFire) : nullptr;
    m_seed = ::Random.randF(0.f, 64.f);
    m_puffs.reserve(64);
    // No actions, no particles: the PAPI effect behind the base class stays empty.
    RefreshShader();
    return TRUE;
}

void CDaFireEffect::OnDeviceCreate()
{
    // The "off" preset keeps a shader too: the graph refuses a visual without one.
    geom.create(FVF::F_LIT, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    shader.create("da_fire");
    m_smoke_geom.create(smoke_decl, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    m_smoke_shader.create("da_smoke");
}

void CDaFireEffect::OnDeviceDestroy()
{
    //  The grid belongs to the device: its textures came from it and its size came from the
    //  fluid manager, and neither of those survives a renderer being torn down and remade.
    fluid_destroy();
    geom.destroy();
    shader.destroy();
    m_smoke_geom.destroy();
    m_smoke_shader.destroy();
}

void CDaFireEffect::Play()
{
    m_RT_Flags.set(flRT_DefferedStop, FALSE);
    m_RT_Flags.set(flRT_Playing, TRUE);
    m_dying = 0.f;
    if (m_preset && m_preset->blast)
    {
        //  A blast starts the moment it is played and lives out its own clock.
        m_blast_t = 0.f;
        m_fluid_retry = 0.f;
        fluid_destroy();
    }
}

//  Going out, in the two shapes the rest of the file wants it in.
//
//  A fire is not switched off. The bed stops feeding the flame over fl_douse seconds - what is
//  already burning above it keeps burning, rises and cools out on its own, because every
//  dissipation term in the simulation runs whether anything is being injected or not. Then the
//  soaked bed steams: soot with no fuel and no heat behind it, thick at first and a thin wisp
//  by the end of fl_smoulder.
float CDaFireEffect::douse_k() const
{
    if (m_dying <= 0.f || !m_preset)
        return 1.f;
    return clampr(1.f - m_dying / std::max(m_preset->fl_douse, 0.05f), 0.f, 1.f);
}

float CDaFireEffect::smoulder_k() const
{
    if (m_dying <= 0.f || !m_preset)
        return 0.f;
    const float T = std::max(m_preset->fl_smoulder, 0.1f);
    if (m_dying >= T)
        return 0.f;
    //  Up in under a second, then straight down over the rest: wet wood smokes hardest just
    //  after it goes out and then thins the whole way, rather than dropping to nothing in the
    //  first three seconds and leaving eight of nothing after it.
    const float rise = clampr(m_dying / 0.7f, 0.f, 1.f);
    const float left = clampr(1.f - m_dying / T, 0.f, 1.f);
    return rise * left;
}

float CDaFireEffect::dying_end() const
{
    return m_preset ? std::max(m_preset->fl_smoulder, 1.5f) : 1.5f;
}

void CDaFireEffect::Stop(BOOL bDefferedStop)
{
    if (bDefferedStop && m_preset)
    {
        // The flame dies down over a second and the plume drifts off before the effect ends.
        m_RT_Flags.set(flRT_DefferedStop, TRUE);
        if (m_dying <= 0.f)
            m_dying = 0.001f;
    }
    else
    {
        m_RT_Flags.set(flRT_Playing | flRT_DefferedStop, FALSE);
        m_puffs.clear();
    }
}

Fvector CDaFireEffect::origin() const
{
    return m_RT_Flags.is(flRT_XFORM) ? m_XFORM.c : m_InitialPosition;
}

Fvector CDaFireEffect::flame_base() const
{
    Fvector b = origin();
    b.y += m_preset->base_height;
    return b;
}

float CDaFireEffect::wind_speed() const
{
    return _sqrt(m_wind.x * m_wind.x + m_wind.z * m_wind.z);
}

// Thomas: the flame length along its axis shrinks as u*^-0.21 in wind.
float CDaFireEffect::flame_length() const
{
    const float d = 2.f * m_preset->radius;
    const float uc = powf(g_gravity * g_burn_rate * d / g_rho_air, 1.f / 3.f);
    // Thomas gives u*^-0.21; for a fire this size the plain saturating cut reads truer.
    (void)uc;
    return m_preset->height * (1.f - 0.3f * clampr(wind_speed() / 8.f, 0.f, 1.f));
}

Fvector CDaFireEffect::flame_tip() const
{
    // The tip of the tilted axis: the smoke leaves from there.
    const float L = flame_length();
    Fvector axis{m_wdir.x * m_tilt, 1.f, m_wdir.y * m_tilt};
    axis.normalize_safe();
    Fvector tip = flame_base();
    tip.mad(axis, L * 1.05f);
    return tip;
}

void CDaFireEffect::update_smoke(float dt, const Fvector& tip)
{
    const auto& env = g_pGamePersistent->Environment();
    const SDaFirePreset& P = *m_preset;
    const float uh = wind_speed();
    const Fvector O = origin();

    // Birth at the flame tip. A cooler fire (strong wind cools it) lifts its smoke slower.
    //
    // Once the fire is out there is no tip to be born at: the puffs come off the bed itself,
    // slower and wider, and only as long as it steams. That is the whole of the smoke tail -
    // the parcels then travel on exactly the same Briggs update as the live plume's.
    const float sm = smoulder_k();
    const bool alive = m_dying <= 0.f;
    if (P.smoke && (alive || sm > 0.f))
    {
        const Fvector src = alive ? tip : flame_base();
        m_puff_acc += P.smoke_rate * (alive ? 1.f : P.fl_smoulder_rate * sm) * dt;
        const float lift = clampr(1.5f * powf(2.f / std::max(uh, 0.7f), 1.f / 3.f), 0.8f, 3.f);
        const float w0 = alive ? lift : lift * 0.3f;
        while (m_puff_acc >= 1.f && m_puffs.size() < 160)
        {
            m_puff_acc -= 1.f;
            SPuff s;
            const float ang = ::Random.randF(0.f, PI_MUL_2);
            const float rr = ::Random.randF(0.f, P.radius * (alive ? 0.6f : 0.9f));
            s.pos.set(src.x + _cos(ang) * rr, src.y + ::Random.randF(-0.1f, 0.1f), src.z + _sin(ang) * rr);
            s.vel.set(m_wind.x * 0.5f, w0 * ::Random.randF(0.85f, 1.15f), m_wind.z * 0.5f);
            s.age = 0.f;
            s.life = P.smoke_life * ::Random.randF(0.75f, 1.25f);
            s.r0 = P.smoke_size * ::Random.randF(0.8f, 1.2f);
            s.seed = ::Random.randF(0.f, 32.f);
            s.rot = ang;
            s.spin = ::Random.randF(-0.35f, 0.35f);
            s.ou_x = s.ou_z = 0.f;
            m_puffs.push_back(s);
        }
    }
    else
        m_puff_acc = 0.f;

    // Briggs' bent-over plume in a few lines: the parcel rises with a buoyancy that decays as
    // (1 + t/0.25)^-1/3 (the x^2/3 trajectory), horizontally it IS the air - the wind of the
    // service at the parcel's own height (the log profile makes smoke aloft run ahead of the
    // flame) plus an Ornstein-Uhlenbeck wander for the turbulence the field does not carry.
    const float qc = 0.7f * P.heat_kw;
    const float ou_tau = 10.f;
    const float ou_sigma = 0.1f * std::max(uh, 0.3f);
    const float ou_k = expf(-dt / ou_tau);
    const float ou_noise = ou_sigma * _sqrt(1.f - ou_k * ou_k);
    for (size_t i = 0; i < m_puffs.size();)
    {
        SPuff& s = m_puffs[i];
        s.age += dt;
        if (s.age >= s.life)
        {
            s = m_puffs.back();
            m_puffs.pop_back();
            continue;
        }
        const float zr = std::max(s.pos.y - tip.y, 0.f);
        const float w_vert = 1.1f * powf(qc / std::max(zr, 0.5f), 1.f / 3.f);
        const float w0 = clampr(1.5f * powf(2.f / std::max(uh, 0.7f), 1.f / 3.f), 0.8f, 3.f);
        const float w_bent = w0 * powf(1.f + s.age / 0.25f, -1.f / 3.f);
        const float bent = clampr(uh / w_vert, 0.f, 1.f);
        float w = 0.6f * w_vert + (w_bent - 0.6f * w_vert) * bent;
        // A dying fire's plume cools out with the flame under it. What the soaked bed steams
        // out afterwards still rises, just barely - a floor rather than nothing, or the tail
        // would hang in the air where it was born.
        if (m_dying > 0.f)
            w *= std::max(0.22f, 1.f - m_dying * 0.5f);
        const float above = std::max(s.pos.y - O.y, 0.3f);
        Fvector air = env.WindAt(s.pos, above);
        air.mul(m_wind_exposure);
        s.ou_x = s.ou_x * ou_k + ::Random.randF(-1.f, 1.f) * ou_noise * 1.7f;
        s.ou_z = s.ou_z * ou_k + ::Random.randF(-1.f, 1.f) * ou_noise * 1.7f;
        const Fvector target{air.x + s.ou_x, w, air.z + s.ou_z};
        // Smoke has no inertia of its own; the short relaxation only hides the sampling steps.
        const float k = 1.f - expf(-dt / 0.35f);
        s.vel.x += (target.x - s.vel.x) * k;
        s.vel.y += (target.y - s.vel.y) * k;
        s.vel.z += (target.z - s.vel.z) * k;
        s.pos.mad(s.vel, dt);
        s.rot += s.spin * dt;
        ++i;
    }
}

void CDaFireEffect::OnFrame(u32 frame_dt)
{
    ZoneScoped;

    if (!m_RT_Flags.is(flRT_Playing))
        return;
    if (!m_preset)
    {
        // "off": nothing to run, nothing to wait for.
        if (m_RT_Flags.is(flRT_DefferedStop))
            m_RT_Flags.set(flRT_Playing | flRT_DefferedStop, FALSE);
        vis.box.set(origin(), origin());
        vis.box.grow(EPS_L);
        vis.box.getsphere(vis.sphere.P, vis.sphere.R);
        return;
    }
    const float dt = std::min(float(frame_dt) * 0.001f, 0.1f);
    m_time += dt;
    if (m_dying > 0.f)
        m_dying += dt;
    if (m_preset->blast)
    {
        if (m_blast_t < 0.f)
        {
            m_RT_Flags.set(flRT_Playing | flRT_DefferedStop, FALSE);
            return;
        }
        m_blast_t += dt;
        if (m_blast_t > m_preset->bl_duration)
        {
            m_blast_t = -1.f;
            m_RT_Flags.set(flRT_Playing | flRT_DefferedStop, FALSE);
            fluid_destroy();
            vis.box.set(origin(), origin());
            vis.box.grow(EPS_L);
            vis.box.getsphere(vis.sphere.P, vis.sphere.R);
            return;
        }
    }

    const auto& env = g_pGamePersistent->Environment();
    const Fvector base = flame_base();

    // The wind at the flame, refreshed a few times a second: a world query with a shelter test.
    m_wind_stamp += dt;
    if (m_wind_stamp > 0.25f)
    {
        m_wind_stamp = 0.f;
        m_wind_exposure = env.WindExposure(base);
        m_wind = env.WindAt(base, 0.75f);
        m_wind.mul(m_wind_exposure);
    }
    const float uh = wind_speed();
    if (uh > 0.05f)
        m_wdir.set(m_wind.x / uh, m_wind.z / uh);

    // Near enough, on a preset that runs it, and the nearest such fire in the scene: only one
    // grid is worth the passes. The grid itself is built or dropped on the render thread.
    const float d2 = Device.vCameraPosition.distance_to_sqr(origin());
    if (Device.dwFrame != g_fluid_frame)
    {
        g_fluid_frame = Device.dwFrame;
        g_fluid_owner = g_fluid_claim;
        g_fluid_claim = nullptr;
        g_fluid_claim_d2 = flt_max;
    }
    //  Never while the level is still coming up: the frames the engine pumps through the
    //  loading screen are not worth a fluid step, and building the grid needs the level's
    //  collision model to be there in the first place.
    const bool ready = g_pGameLevel && g_pGameLevel->bReady && !Device.dwPrecacheFrame;
    //  A quarter of the range of hysteresis: building the grid means voxelising the level,
    //  and a player standing on the boundary should not pay for that twice a second.
    const float lim = ps_r__fire_fluid_dist * (m_fluid ? 1.25f : 1.f);
    //  A fire that is going out keeps the grid it already has - that is where the flame it
    //  still has to burn out lives - but it never claims a new one.
    const bool eligible =
        ready && ps_r__fire_fluid && m_preset->fluid && d2 < lim * lim && (m_dying <= 0.f || m_fluid);
    //  A blast outranks a campfire for the one grid the frame can afford, but by a factor
    //  rather than absolutely: something going off across the camp has no business taking the
    //  grid from the fire the player is standing at. It does take it on the frame it goes off
    //  rather than waiting its turn.
    const float rank = m_preset->blast ? d2 * 0.2f : (m_dying > 0.f ? d2 * 4.f : d2);
    if (eligible && rank < g_fluid_claim_d2)
    {
        g_fluid_claim_d2 = rank;
        g_fluid_claim = this;
    }
    m_fluid_wanted = eligible && g_fluid_owner == this;
    //  Half a second of crossover: at the distance where the grid takes over, both are drawn
    //  and one fades into the other instead of the flame changing shape in a single frame.
    if (m_preset->blast)
    {
        //  A blast leaves rather than stops: over its last moments the whole volume thins out,
        //  so what is left of the cloud goes instead of being switched off.
        const float left = m_preset->bl_duration - m_blast_t;
        m_fluid_fade = m_fluid_wanted ? clampr(left / m_preset->bl_fade, 0.f, 1.f) : 0.f;
    }
    else
        m_fluid_fade = clampr(m_fluid_fade + (m_fluid_wanted ? dt : -dt) / 0.5f, 0.f, 1.f);

    // Tilt. The AGA correlation (cos theta = u*^-1/2) is for pool fires a metre and more
    // across and lays a campfire almost flat in a breeze; a saturating law reads true for a
    // fire this size: 18 degrees at 2 m/s, 30 at 4, 45 at 8, 50 at most. The filter (a quarter
    // of a second) lags a gust so the flame tears downwind before it settles.
    const float theta = deg2rad(50.f) * (1.f - expf(-uh / 4.5f)) * m_preset->lean;
    const float tilt = tanf(std::min(theta, deg2rad(60.f)));
    const float k = 1.f - expf(-dt / 0.25f);
    m_tilt += (tilt - m_tilt) * k;
    m_gust += (env.eff_wind_gust - m_gust) * k;

    // While the grid is running it carries the flame and the first couple of metres of the
    // plume itself, so the billboards start where the box ends instead of at the flame's tip.
    const Fvector tip = m_fluid ? fluid_plume_start() : flame_tip();
    if (!m_preset->blast)
        update_smoke(dt, tip);

    if (m_dying > dying_end() && m_puffs.empty())
    {
        m_RT_Flags.set(flRT_Playing | flRT_DefferedStop, FALSE);
        return;
    }

    // Bounds: for a blast, the grid box it lives in; otherwise the tilted flame envelope plus
    // every puff. Kept in the effect's own space, as the base class does (world unless the
    // parent transform is applied at render time).
    if (m_preset->blast)
    {
        Fvector c = origin();
        c.y += m_preset->bl_lift;
        vis.box.set(c, c);
        vis.box.grow(m_preset->bl_cell * 48.f);
        vis.box.getsphere(vis.sphere.P, vis.sphere.R);
        return;
    }
    const float L = m_preset->height * 1.7f;
    Fbox box;
    box.set(base, base);
    box.grow(m_preset->radius * 2.f);
    box.modify(tip);
    Fvector top = tip;
    top.y += L * 0.6f;
    box.modify(top);
    box.grow(m_preset->radius);
    for (const SPuff& s : m_puffs)
    {
        const float r = s.r0 + 0.5f * std::max(s.pos.y - tip.y, 0.f) + 0.3f;
        Fvector lo, hi;
        lo.sub(s.pos, Fvector{r, r, r});
        hi.add(s.pos, Fvector{r, r, r});
        box.modify(lo);
        box.modify(hi);
    }
    if (m_RT_Flags.is(flRT_XFORM))
    {
        Fmatrix inv;
        inv.invert(m_XFORM);
        Fvector c, mn, mx;
        box.getcenter(c);
        inv.transform_tiny(c);
        Fvector half;
        box.getradius(half);
        const float rr = half.magnitude();
        mn.sub(c, Fvector{rr, rr, rr});
        mx.add(c, Fvector{rr, rr, rr});
        box.set(mn, mx);
    }
    vis.box.set(box);
    vis.box.getsphere(vis.sphere.P, vis.sphere.R);
}

void CDaFireEffect::render_flame(CBackend& cmd_list, float fade)
{
    const SDaFirePreset& P = *m_preset;
    const Fvector base = flame_base();
    //  Going out, the flame has to sink into the bed rather than just dim: the march writes
    //  opacity whether it glows or not, so an emission-only fade leaves a flame-shaped hole
    //  in front of whatever is behind it. The fluid path has a simulation to burn itself out
    //  in; this one does it with its own length.
    const float L = flame_length() * (0.15f + 0.85f * douse_k());
    const float uh = wind_speed();
    const float d = 2.f * P.radius;

    // The rasterization vehicle: a camera-facing quad over the volume's bounding sphere. Its
    // depth means nothing (the shader marches from the eye and reads the scene depth itself),
    // so when the eye is inside the sphere the quad simply fills the view.
    Fvector axis{m_wdir.x * m_tilt, 1.f, m_wdir.y * m_tilt};
    axis.normalize_safe();
    Fvector centre = base;
    centre.mad(axis, L * 0.75f);
    // The base stretches downwind a little; the pool-fire drag law would double it.
    const float drag = 1.f + 0.15f * clampr(uh / 6.f, 0.f, 1.f);
    const float sr = _sqrt(_sqr(L * 1.2f) + _sqr(P.radius * drag * 2.f)) + 0.15f;
    const Fvector& eye = Device.vCameraPosition;
    const float dist = eye.distance_to(centre);
    Fvector qc;
    float half;
    if (dist > sr * 1.5f)
    {
        qc = centre;
        half = sr / _sqrt(1.f - _sqr(sr / dist)) * 1.04f;
    }
    else
    {
        qc.mad(eye, Device.vCameraDirection, 0.3f);
        half = 0.3f * tanf(deg2rad(Device.fFOV) * 0.5f) * std::max(Device.fASPECT, 1.f / Device.fASPECT) * 1.6f;
    }
    Fvector right, top;
    right.mul(Device.vCameraRight, half);
    top.mul(Device.vCameraTop, half);

    u32 offset;
    FVF::LIT* pv = static_cast<FVF::LIT*>(RImplementation.Vertex.Lock(4, geom->vb_stride, offset));
    Fvector a, b, p;
    a.sub(top, right);
    b.add(top, right);
    p.sub(qc, b);
    pv->set(p.x, p.y, p.z, 0xffffffff, 0.f, 1.f); ++pv;
    p.add(qc, a);
    pv->set(p.x, p.y, p.z, 0xffffffff, 0.f, 0.f); ++pv;
    p.sub(qc, a);
    pv->set(p.x, p.y, p.z, 0xffffffff, 1.f, 1.f); ++pv;
    p.add(qc, b);
    pv->set(p.x, p.y, p.z, 0xffffffff, 1.f, 0.f); ++pv;
    RImplementation.Vertex.Unlock(4, geom->vb_stride);

    // Puffing: 1.5/sqrt(D) Hz in calm air, up to four times that in wind.
    const float fpuff = 1.5f / _sqrt(d) * std::min(4.f, 1.f + uh);
    cmd_list.set_c("da_fire_a", base.x, base.y, base.z, m_time);
    cmd_list.set_c("da_fire_b", P.radius, L, P.intensity * fade, m_seed);
    cmd_list.set_c("da_fire_c", m_wdir.x * m_tilt, m_wdir.y * m_tilt, uh, m_gust);
    cmd_list.set_c("da_fire_d", fpuff, drag, sr, P.height);
    cmd_list.set_c("da_fire_e", centre.x, centre.y, centre.z, m_wind_exposure);

    cmd_list.set_xform_world(Fidentity);
    cmd_list.set_Geometry(geom);
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
    cmd_list.set_CullMode(CULL_CCW);
}

void CDaFireEffect::render_smoke(CBackend& cmd_list)
{
    if (m_puffs.empty() || !m_smoke_shader)
        return;
    const SDaFirePreset& P = *m_preset;
    const auto& env = g_pGamePersistent->Environment();
    const Fvector tip = flame_tip();
    const Fvector& eye = Device.vCameraPosition;
    const float uh = wind_speed();
    const float bent = clampr(uh / 3.f, 0.f, 1.f);

    // Back to front within the plume; the graph already sorted the plume against the rest.
    xr_vector<std::pair<float, u32>> order;
    order.reserve(m_puffs.size());
    for (u32 i = 0; i < u32(m_puffs.size()); ++i)
        order.emplace_back(eye.distance_to_sqr(m_puffs[i].pos), i);
    std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first > r.first; });

    // Sky light on the albedo: a well-burning fire smokes light grey, a resinous one dark.
    const float albedo = 0.62f - 0.34f * P.smoke_grey;
    const Fvector4& hemi = env.CurrentEnv.hemi_color;
    const Fvector3& amb = env.CurrentEnv.ambient;
    const Fvector3 sky{(hemi.x * 0.55f + amb.x) * albedo, (hemi.y * 0.55f + amb.y) * albedo, (hemi.z * 0.55f + amb.z) * albedo};
    const Fvector3& sun = env.CurrentEnv.sun_color;
    Fvector to_sun = env.CurrentEnv.sun_dir;
    to_sun.invert();
    to_sun.normalize_safe();
    const float sun_r = to_sun.dotproduct(Device.vCameraRight);
    const float sun_u = to_sun.dotproduct(Device.vCameraTop);
    const float sun_f = -to_sun.dotproduct(Device.vCameraDirection);

    const u32 count = u32(order.size());
    u32 offset;
    SSmokeVertex* pv = static_cast<SSmokeVertex*>(RImplementation.Vertex.Lock(count * 4, m_smoke_geom->vb_stride, offset));
    for (const auto& [dsq, idx] : order)
    {
        const SPuff& s = m_puffs[idx];
        const float agen = s.age / s.life;
        const float zr = std::max(s.pos.y - tip.y, 0.f);
        // Radius: self-entrainment (0.12 z calm, 0.6 z bent over) plus ambient dispersion.
        Fvector rel;
        rel.sub(s.pos, tip);
        const float x_down = std::max(rel.x * m_wdir.x + rel.z * m_wdir.y, 0.f);
        const float r = s.r0 + (0.12f + 0.10f * bent) * zr + 0.05f * x_down;
        // Opacity: dilution as the puff grows, a fade-in over the first 0.3 s, the tail fade.
        float alpha = P.smoke_alpha * powf(s.r0 / r, 1.2f);
        alpha *= clampr(s.age / 0.3f, 0.f, 1.f);
        const float tail = clampr((agen - 0.7f) / 0.3f, 0.f, 1.f);
        alpha *= 1.f - tail * tail * (3.f - 2.f * tail);
        // A puff at the eye would blank the view: fade out inside a metre.
        alpha *= clampr((_sqrt(dsq) - 0.4f) / 0.8f, 0.f, 1.f);
        if (alpha < 0.002f)
        {
            SSmokeVertex* pend = pv + 4;
            for (; pv != pend; ++pv)
            {
                pv->p = s.pos; pv->c = 0; pv->t.set(0.f, 0.f); pv->e.set(0.f, 0.f, 0.f, 0.f);
            }
            continue;
        }
        // Wood smoke is blue-grey, not the colour of the autumn sky it sits under: the sky
        // light's luminance tinted cool, cooler still the thinner the puff.
        const float lum = sky.x * 0.3f + sky.y * 0.59f + sky.z * 0.11f;
        const float thin = 0.45f + 0.35f * (1.f - clampr(alpha / 0.2f, 0.f, 1.f));
        Fvector3 col = sky;
        col.x += (lum * 0.92f - col.x) * thin;
        col.y += (lum * 0.98f - col.y) * thin;
        col.z += (lum * 1.12f - col.z) * thin;
        const float dtip = s.pos.distance_to(tip);
        const float glow = P.intensity * (m_dying > 0.f ? std::max(0.f, 1.f - m_dying) : 1.f) * 0.4f / (1.f + dtip * dtip * 0.6f);
        const u32 clr = color_rgba_f(clampr(col.x, 0.f, 1.f), clampr(col.y, 0.f, 1.f), clampr(col.z, 0.f, 1.f), alpha);
        const float sa = _sin(s.rot), ca = _cos(s.rot);
        Fvector right, top;
        right.set(Device.vCameraRight); right.mul(r);
        top.set(Device.vCameraTop); top.mul(r);
        Fvector rr, tt;
        rr.mul(right, ca); rr.mad(top, sa);
        tt.mul(top, ca); tt.mad(right, -sa);
        Fvector4 extra;
        extra.set(s.seed, agen, glow, r);
        fill_quad(pv, s.pos, rr, tt, clr, extra);
    }
    RImplementation.Vertex.Unlock(count * 4, m_smoke_geom->vb_stride);

    cmd_list.set_Element(m_smoke_shader->E[0]);
    cmd_list.set_c("da_smoke_sun", sun_r, sun_u, sun_f, 1.f);
    cmd_list.set_c("da_smoke_suncol", sun.x * albedo * 0.8f, sun.y * albedo * 0.8f, sun.z * albedo * 0.8f, m_time);
    cmd_list.set_xform_world(Fidentity);
    cmd_list.set_Geometry(m_smoke_geom);
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, count * 4, 0, count * 2);
    cmd_list.set_CullMode(CULL_CCW);
}

void CDaFireEffect::Render(CBackend& cmd_list, float, bool)
{
    if (!m_preset || !m_RT_Flags.is(flRT_Playing))
        return;
    // The same distance cut as the sprite effects (r__particle_dist).
    if (ps_r__particle_dist > 0)
    {
        const float lim = float(ps_r__particle_dist);
        if (Device.vCameraPosition.distance_to_sqr(origin()) > lim * lim)
            return;
    }
    if (m_fluid_wanted && !m_fluid)
        fluid_create();
    else if (!m_fluid_wanted && m_fluid && m_fluid_fade <= 0.f)
        fluid_destroy();
    //  The marched flame has no simulation to burn itself out in, so it does it by hand, on
    //  the same ramp the bed's fuel follows. render_flame takes it on the emission AND on the
    //  flame's length: emission alone would leave a flame-shaped hole, because the march
    //  writes opacity whether it glows or not.
    const float fade = douse_k();
    if (m_preset->blast)
    {
        //  Nothing to fall back to: a blast is the volume or it is nothing.
        if (m_fluid)
            fluid_render(cmd_list);
        return;
    }
    if (m_fluid && m_fluid_fade > 0.f)
        fluid_render(cmd_list);
    if (fade > 0.f && m_fluid_fade < 1.f)
        render_flame(cmd_list, fade * (1.f - m_fluid_fade));
    render_smoke(cmd_list);
}
} // namespace xray::render::RENDER_NAMESPACE::PS
