#include "StdAfx.h"
#include "Common/LevelGameDef.h"
#include "ai_space.h"
#include "ParticlesObject.h"
#include "xrScriptEngine/script_process.hpp"
#include "xrScriptEngine/script_engine.hpp"
#include "Level.h"
#include "game_cl_base.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "xrPhysics/PhysicsCommon.h"
#include "level_sounds.h"
#include "GamePersistent.h"
#include "xrEngine/Rain.h"
#include "xrEngine/Environment.h"

namespace
{
// Worst case triangles inspected per pass. Every shipped level fits under it and is swept whole;
// the stride only ever engages on something far denser, and a water body tessellated into
// hundreds of triangles survives it anyway.
constexpr u32 water_sweep_budget = 1u << 18;
// Cells per axis over the water extent - one down-ray per cell that holds water, so a thousand
// picks at the absolute worst and in practice a couple of dozen. The resolution also decides how
// finely two ponds can be told apart, which is why it is not the dozen it started at: a Zone
// level is about a kilometre across, so this is a cell every thirty metres.
constexpr int water_grid = 32;
constexpr float water_probe_range = 60.f; // metres of bed to search for; past it the sample drops
constexpr float water_probe_drop = 0.05f; // start the ray just under the surface, not on it
// Depth reported when not one ray found a bed - water over a hole in the collision, or a bed out
// of probe range. The same value CEnvironment falls back to, so the two never disagree.
constexpr float water_depth_unknown = 1.5f;

struct water_cell
{
    Fvector top; // highest liquid centroid seen in this cell
    Fbox2 span;  // XZ footprint of the liquid actually found here, not the cell's own rectangle
    int body;    // which connected sheet this cell belongs to, -1 until the flood fill runs
    bool used;
};

bool is_liquid(const CDB::TRI& tri)
{
    const SGameMtl* mtl = GMLib.GetMaterialByIdx(u16(tri.material));
    return mtl && mtl->Flags.test(SGameMtl::flLiquid);
}

// ---- The baked water field -------------------------------------------------------------------
// Metres of channel A, the clamp DESIGN2 puts on the distance to the nearest bank. It doubles as
// the distance transform's infinity: the chamfer sweep only ever grows away from its seeds, so
// everything that really is nearer than the clamp still comes out exact.
constexpr float water_shore_max = 32.f;

// ---- The puddle fill map ----------------------------------------------------------------------
// Baked in the same sweep, from the same detail-slot heights the bed comes from: how deep rain
// would stand at this texel once it has run downhill. Deeper than the cap reads as full - past a
// couple of hand-widths it is a pond and belongs to the water field, not to the puddle system.
// The cap lives on CEnvironment because three places decode with it: this bake, the CPU replica
// of the mask, and DA_PUDDLE_FILL_MAX in da_puddles.h.
constexpr float puddle_fill_max = CEnvironment::puddle_fill_depth;
// The rise the relaxation gives a dammed cell over its lowest way out. A millimetre is small
// enough to leave the depth exact to the quantisation of the source heights and large enough
// that a flat basin still slopes towards its outlet instead of sitting at one value.
constexpr float puddle_fill_eps = .001f;
// Two-directional sweeps. A full relaxation over a 1024 grid is thousands of them; forty settle
// every basin small enough to hold a puddle, and the cap is what keeps this a load-time cost
// rather than a stall.
constexpr int puddle_fill_passes = 40;
// Ground the detail grid never described. Neighbours drain into it, so a place the bake knows
// nothing about grows no puddle rather than a guessed one.
constexpr float puddle_no_ground = -1000.f;
// Starting head of a cell the flood has not reached down to yet.
constexpr float puddle_flooded = 1e30f;

// level.details read straight out of the VFS for its slot heightmap. CDetailManager holds the
// same file open already, but it lives behind the render DLL and the game layer has no route to
// it, so the bake parses the two fields it needs itself. The layout, the quantisation and the
// slot pitch below are DetailFormat.h's DetailHeader/DetailSlot - keep them in step.
struct detail_heightmap
{
    static constexpr u32 expect_version = 3; // DETAIL_VERSION
    static constexpr u32 slot_bytes = 16; // sizeof(DetailSlot)
    static constexpr float slot_size = 2.f; // DETAIL_SLOT_SIZE

    IReader* file{};
    const u8* slots{};
    u32 slot_count{};
    int size_x{}, size_z{}, offs_x{}, offs_z{};

    void open()
    {
        if (!FS.exist("$level$", "level.details"))
            return;

        string_path fn;
        FS.update_path(fn, "$level$", "level.details");
        file = FS.r_open(fn);
        if (!file)
            return;

        bool ok = false;
        if (IReader* head = file->open_chunk(0))
        {
            // version, object count, offs_x, offs_z, size_x, size_z
            if (head->length() >= 6 * sizeof(u32) && head->r_u32() == expect_version)
            {
                head->r_u32();
                offs_x = head->r_s32();
                offs_z = head->r_s32();
                size_x = head->r_s32();
                size_z = head->r_s32();
                ok = size_x > 0 && size_z > 0;
            }
            head->close();
        }
        if (!ok)
            return;

        // The slot pointer outlives the sub-reader but not the file, exactly as
        // CDetailManager::Load keeps dtSlots past its own close().
        if (IReader* body = file->open_chunk(2))
        {
            slots = static_cast<const u8*>(body->pointer());
            slot_count = u32(body->length() / slot_bytes);
            body->close();
        }
    }

    void close()
    {
        if (file)
            FS.r_close(file);
        file = nullptr;
        slots = nullptr;
    }

    // Terrain base Y at a world XZ. A slot the compiler never wrote decodes to the -200 m floor
    // of the quantisation, and that zero is the only tell that the grid has no ground here.
    bool base_at(float wx, float wz, float& y) const
    {
        if (!slots)
            return false;

        const int dx = iFloor(wx / slot_size) + offs_x;
        const int dz = iFloor(wz / slot_size) + offs_z;
        if (dx < 0 || dz < 0 || dx >= size_x || dz >= size_z)
            return false;

        const u32 at = u32(dz) * u32(size_x) + u32(dx);
        if (at >= slot_count)
            return false;

        // First dword of DetailSlot: y_base is its low 12 bits, 1 unit = 20 cm from -200 m.
        // Assembled byte by byte because the chunk is not guaranteed to be dword-aligned.
        const u8* p = slots + size_t(at) * slot_bytes;
        const u32 packed = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
        const u32 y_base = packed & 0xfffu;
        if (!y_base)
            return false;

        y = float(y_base) * .2f - 200.f;
        return true;
    }
};

// Depth of whichever measured body a point falls in. Backwards, so the smallest containing body
// wins: the array is sorted biggest first, and a puddle sitting inside a lake's bounding box
// should answer with its own depth rather than the lake's.
float water_body_depth_at(const CEnvironment& env, float wx, float wz)
{
    for (int i = env.water_body_count - 1; i >= 0; --i)
    {
        const Fbox& e = env.water_bodies[i].extent;
        if (wx >= e.vMin.x && wx <= e.vMax.x && wz >= e.vMin.z && wz <= e.vMax.z)
            return env.water_bodies[i].depth;
    }
    return water_depth_unknown;
}

// Where rain would stand once it has run downhill - a Planchon-Darboux fill over the terrain
// heights the bed came from, run on the same grid so the two maps cannot disagree about ground.
//
// Every cell starts flooded to infinity except the ones water can leave through: the map border,
// the texels a water body already covers, and the ones the detail grid never described. The
// relaxation then lets each flooded cell back down to the lowest sill on any way out of it -
//     w[i] = max( terrain[i], min over the 4 neighbours of ( w[n] + eps ) )
// - and what is left standing above the terrain is the depth of the puddle that basin holds.
//
// The sweep direction alternates because one direction only carries a sill downstream of itself:
// a basin drained from the far corner would need as many passes as it is wide. Alternating
// carries a lowered cell both ways, which is what turns thousands of sweeps into tens.
void bake_puddle_fill(
    const xr_vector<CEnvironment::SWaterTexel>& field, const xr_vector<float>& terrain, xr_vector<float>& fill)
{
    constexpr int dim = CEnvironment::water_field_dim;
    constexpr size_t texels = size_t(dim) * size_t(dim);

    xr_vector<float> head;
    head.resize(texels);
    for (int z = 0; z < dim; ++z)
        for (int x = 0; x < dim; ++x)
        {
            const size_t at = size_t(z) * dim + size_t(x);
            const bool outlet =
                x == 0 || z == 0 || x == dim - 1 || z == dim - 1 || field[at].mask || terrain[at] <= puddle_no_ground;
            head[at] = outlet ? terrain[at] : puddle_flooded;
        }

    for (int pass = 0; pass < puddle_fill_passes; ++pass)
    {
        bool changed = false;
        const bool forward = (pass & 1) == 0;
        for (int zi = 1; zi < dim - 1; ++zi)
        {
            const int z = forward ? zi : dim - 1 - zi;
            for (int xi = 1; xi < dim - 1; ++xi)
            {
                const int x = forward ? xi : dim - 1 - xi;
                const size_t at = size_t(z) * dim + size_t(x);
                const float ground = terrain[at];
                // An outlet sits on its own ground, and so does a cell the flood already
                // finished draining. Neither can move again.
                if (head[at] <= ground)
                    continue;

                const float out =
                    _max(ground, _min(_min(head[at - 1], head[at + 1]), _min(head[at - dim], head[at + dim])) +
                        puddle_fill_eps);
                if (out < head[at])
                {
                    head[at] = out;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }

    fill.resize(texels);
    for (size_t at = 0; at < texels; ++at)
    {
        // A lake is not a puddle - the water field owns those texels. Neither is a cell the
        // relaxation never reached, nor one with no ground under it: unknown reads as dry, so
        // the failure mode is a missing puddle and never a puddle-coloured hillside.
        const float ground = terrain[at];
        const bool known = !field[at].mask && ground > puddle_no_ground && head[at] < puddle_flooded;
        const float depth = known ? _max(head[at] - ground, 0.f) : 0.f;
        fill[at] = _min(depth, puddle_fill_max) * (1.f / puddle_fill_max);
    }
}

// Bakes the level-wide water map: surface height, coverage, bed height and distance to the
// nearest bank, one texel every metre or so over the level's own bounding box. Everything from
// wave attenuation to the wade state reads this instead of asking the collision again, so it is
// worth a few dozen milliseconds once per level. Runs after measure_water_body has published the
// bodies, whose measured depth is the fallback where the detail grid has no ground.
void bake_water_field(CObjectSpace& space)
{
    CEnvironment& env = g_pGamePersistent->Environment();

    CDB::MODEL* model = space.GetStaticModel();
    const CDB::TRI* tris = space.GetStaticTris();
    const Fvector* verts = space.GetStaticVerts();
    if (!model || !tris || !verts)
        return;
    const u32 count = model->get_tris_count();
    if (!count)
        return;

    Fbox bounds = space.GetBoundingVolume();
    if (!bounds.is_valid())
        return;

    // Squared about its centre, because the field publishes a single metres-per-texel to the
    // shaders: a rectangular footprint would leave the two axes disagreeing about the scale.
    const float mid_x = .5f * (bounds.vMin.x + bounds.vMax.x);
    const float mid_z = .5f * (bounds.vMin.z + bounds.vMax.z);
    const float half = .5f * _max(_max(bounds.vMax.x - bounds.vMin.x, bounds.vMax.z - bounds.vMin.z), EPS_S);
    bounds.vMin.x = mid_x - half;
    bounds.vMax.x = mid_x + half;
    bounds.vMin.z = mid_z - half;
    bounds.vMax.z = mid_z + half;

    constexpr int dim = CEnvironment::water_field_dim;
    const float texel = (half * 2.f) / float(dim);
    const float inv_texel = 1.f / texel;

    xr_vector<CEnvironment::SWaterTexel> field;
    field.resize(size_t(dim) * size_t(dim));

    // Texel-index space: subtracting the half texel puts index i exactly on the centre of texel
    // i, so the coverage test is a plain point-in-triangle at integer coordinates.
    const auto to_ix = [&](float wx) { return (wx - bounds.vMin.x) * inv_texel - .5f; };
    const auto to_iz = [&](float wz) { return (wz - bounds.vMin.z) * inv_texel - .5f; };

    bool any = false;

    // Every liquid triangle, unstrided - the body measurement can afford to skip one because it
    // only wants an extent and a mean, but a triangle skipped here is a hole in the map.
    for (u32 i = 0; i < count; ++i)
    {
        const CDB::TRI& tri = tris[i];
        if (!is_liquid(tri))
            continue;

        const Fvector& va = verts[tri.verts[0]];
        const Fvector& vb = verts[tri.verts[1]];
        const Fvector& vc = verts[tri.verts[2]];

        const float ax = to_ix(va.x), az = to_iz(va.z);
        const float bx = to_ix(vb.x), bz = to_iz(vb.z);
        const float cx = to_ix(vc.x), cz = to_iz(vc.z);

        const float area2 = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
        // Nothing to rasterise from a triangle stood on edge; a water sheet always brings
        // horizontal ones as well.
        if (_abs(area2) < EPS_S)
            continue;
        const float inv_area = 1.f / area2;

        int x0 = iCeil(_min(_min(ax, bx), cx)), x1 = iFloor(_max(_max(ax, bx), cx));
        int z0 = iCeil(_min(_min(az, bz), cz)), z1 = iFloor(_max(_max(az, bz), cz));
        clamp(x0, 0, dim - 1);
        clamp(x1, 0, dim - 1);
        clamp(z0, 0, dim - 1);
        clamp(z1, 0, dim - 1);

        bool stamped = false;
        for (int z = z0; z <= z1; ++z)
        {
            const float pz = float(z);
            for (int x = x0; x <= x1; ++x)
            {
                const float px = float(x);
                const float wa = (cx - bx) * (pz - bz) - (px - bx) * (cz - bz);
                const float wb = (ax - cx) * (pz - cz) - (px - cx) * (az - cz);
                const float wc = (bx - ax) * (pz - az) - (px - ax) * (bz - az);
                if (wa * area2 < 0.f || wb * area2 < 0.f || wc * area2 < 0.f)
                    continue;

                const float y = (wa * va.y + wb * vb.y + wc * vc.y) * inv_area;
                CEnvironment::SWaterTexel& t = field[size_t(z) * dim + size_t(x)];
                if (!t.mask || y > t.surface)
                    t.surface = y;
                t.mask = 1;
                stamped = true;
                any = true;
            }
        }

        // A triangle narrower than a texel can miss every centre. Stamp the one its centroid
        // falls in so a thin stream still reads as water instead of as a chain of holes.
        if (!stamped)
        {
            const int mx = iFloor((ax + bx + cx) / 3.f + .5f);
            const int mz = iFloor((az + bz + cz) / 3.f + .5f);
            if (mx >= 0 && mx < dim && mz >= 0 && mz < dim)
            {
                const float y = _max(_max(va.y, vb.y), vc.y);
                CEnvironment::SWaterTexel& t = field[size_t(mz) * dim + size_t(mx)];
                if (!t.mask || y > t.surface)
                    t.surface = y;
                t.mask = 1;
                any = true;
            }
        }
    }

    if (!any)
        return;

    // Bed, from the detail grid's 2 m heightmap where it reaches and from the containing body's
    // measured depth where it does not. Ground that decodes above the water is a bank caught
    // inside a water texel - it is clamped to the surface (zero depth) rather than rejected,
    // because falling back to the body depth there would dig a hole in the shallows.
    detail_heightmap details;
    details.open();

    // Terrain height everywhere, not only under water: the puddle fill needs the whole grid, and
    // reading it here means both maps stand on the same heights and the file is opened once.
    xr_vector<float> terrain;
    terrain.assign(size_t(dim) * size_t(dim), puddle_no_ground);

    for (int z = 0; z < dim; ++z)
    {
        const float wz = bounds.vMin.z + (float(z) + .5f) * texel;
        for (int x = 0; x < dim; ++x)
        {
            const size_t at = size_t(z) * dim + size_t(x);
            const float wx = bounds.vMin.x + (float(x) + .5f) * texel;

            float base;
            const bool grounded = details.base_at(wx, wz, base);
            if (grounded)
                terrain[at] = base;

            CEnvironment::SWaterTexel& t = field[at];
            if (!t.mask)
                continue;

            if (grounded)
                t.bed = _min(base, t.surface);
            else
                t.bed = t.surface - water_body_depth_at(env, wx, wz);
        }
    }
    details.close();

    // Distance to the nearest bank, defined on both sides of it: seed zero on every texel that
    // touches the other class, then a two-pass chamfer sweep with 1 and sqrt(2) weights in
    // metres. Off-grid neighbours count as the same class, so water running off the edge of the
    // level does not grow a bank there.
    const float step_1 = texel;
    const float step_2 = texel * 1.41421356f;
    const auto wet_at = [&](int x, int z) { return field[size_t(z) * dim + size_t(x)].mask != 0; };
    for (int z = 0; z < dim; ++z)
        for (int x = 0; x < dim; ++x)
        {
            const bool wet = wet_at(x, z);
            const bool edge = (x > 0 && wet_at(x - 1, z) != wet) || (x < dim - 1 && wet_at(x + 1, z) != wet) ||
                (z > 0 && wet_at(x, z - 1) != wet) || (z < dim - 1 && wet_at(x, z + 1) != wet);
            field[size_t(z) * dim + size_t(x)].shore = edge ? 0.f : water_shore_max;
        }

    for (int z = 0; z < dim; ++z)
        for (int x = 0; x < dim; ++x)
        {
            float d = field[size_t(z) * dim + size_t(x)].shore;
            if (x > 0)
                d = _min(d, field[size_t(z) * dim + size_t(x - 1)].shore + step_1);
            if (z > 0)
            {
                d = _min(d, field[size_t(z - 1) * dim + size_t(x)].shore + step_1);
                if (x > 0)
                    d = _min(d, field[size_t(z - 1) * dim + size_t(x - 1)].shore + step_2);
                if (x < dim - 1)
                    d = _min(d, field[size_t(z - 1) * dim + size_t(x + 1)].shore + step_2);
            }
            field[size_t(z) * dim + size_t(x)].shore = d;
        }

    for (int z = dim - 1; z >= 0; --z)
        for (int x = dim - 1; x >= 0; --x)
        {
            float d = field[size_t(z) * dim + size_t(x)].shore;
            if (x < dim - 1)
                d = _min(d, field[size_t(z) * dim + size_t(x + 1)].shore + step_1);
            if (z < dim - 1)
            {
                d = _min(d, field[size_t(z + 1) * dim + size_t(x)].shore + step_1);
                if (x < dim - 1)
                    d = _min(d, field[size_t(z + 1) * dim + size_t(x + 1)].shore + step_2);
                if (x > 0)
                    d = _min(d, field[size_t(z + 1) * dim + size_t(x - 1)].shore + step_2);
            }
            field[size_t(z) * dim + size_t(x)].shore = d;
        }

    // The puddle fill, from the heights gathered above. One bake and one hand-over for both maps:
    // the renderer keys its upload on the field's identity, so a fill with a lifetime of its own
    // would sooner or later be a level behind the field it is read beside.
    xr_vector<float> fill;
    bake_puddle_fill(field, terrain, fill);

    env.set_water_field(std::move(field), std::move(fill), bounds);
}

// Measures the level's water body once, from the static collision - the same liquid-material
// sweep qa_water_goto walks. The extent gives the sea-state solver its fetch, the mean depth
// gives the waves their shallow-water attenuation and the optics their path length. A level
// with no liquid triangle resets the body and says so, and costs nothing further.
void measure_water_body(CObjectSpace& space)
{
    CEnvironment& env = g_pGamePersistent->Environment();
    env.reset_water_body();

    CDB::MODEL* model = space.GetStaticModel();
    const CDB::TRI* tris = space.GetStaticTris();
    const Fvector* verts = space.GetStaticVerts();
    if (!model || !tris || !verts)
        return;
    const u32 count = model->get_tris_count();
    if (!count)
        return;

    const u32 stride = _max(1u, count / water_sweep_budget);

    Fbox extent;
    extent.invalidate();
    for (u32 i = 0; i < count; i += stride)
    {
        const CDB::TRI& tri = tris[i];
        if (!is_liquid(tri))
            continue;
        for (int k = 0; k < 3; ++k)
            extent.modify(verts[tri.verts[k]]);
    }
    if (!extent.is_valid())
        return;

    // Second pass bins the liquid triangles into a coarse grid and keeps the topmost centroid of
    // each cell. Probing from a real centroid rather than from the cell's centre keeps every ray
    // starting under actual water - the extent of an L-shaped pond covers a lot of dry bank, and
    // banks would drag the mean depth to nothing. Two ponds at different heights each keep their
    // own surface this way as well.
    water_cell cells[water_grid * water_grid]{};
    const float span_x = _max(extent.vMax.x - extent.vMin.x, EPS_S);
    const float span_z = _max(extent.vMax.z - extent.vMin.z, EPS_S);
    for (u32 i = 0; i < count; i += stride)
    {
        const CDB::TRI& tri = tris[i];
        if (!is_liquid(tri))
            continue;

        Fvector c;
        c.add(verts[tri.verts[0]], verts[tri.verts[1]]);
        c.add(verts[tri.verts[2]]);
        c.div(3.f);

        int cx = iFloor((c.x - extent.vMin.x) / span_x * water_grid);
        int cz = iFloor((c.z - extent.vMin.z) / span_z * water_grid);
        clamp(cx, 0, water_grid - 1);
        clamp(cz, 0, water_grid - 1);

        water_cell& cell = cells[cz * water_grid + cx];
        if (!cell.used || c.y > cell.top.y)
            cell.top = c;
        if (cell.used)
            cell.span.modify(Fvector2{c.x, c.z});
        else
            cell.span.set(c.x, c.z, c.x, c.z);
        cell.used = true;
        cell.body = -1;
    }

    // Split the occupied cells into connected sheets. A level is a kilometre across and almost
    // every one carries several disjoint bodies - a stream at one edge, a flooded pit at the
    // other - so one box around all of them would hand a two-metre pool the fetch and the depth
    // of a lake, and fetch is most of what decides how big the waves are. Plain 4-connected
    // flood fill over the grid, iterative so a long winding river cannot blow the stack.
    struct sheet
    {
        Fbox extent;
        double depth_sum;
        u32 probes;
        u32 cells;
    };
    xr_vector<sheet> sheets;
    xr_vector<int> stack;
    for (int start = 0; start < water_grid * water_grid; ++start)
    {
        if (!cells[start].used || cells[start].body >= 0)
            continue;

        const int id = int(sheets.size());
        sheets.emplace_back(sheet{});
        sheet& sh = sheets.back();
        sh.extent.invalidate();
        sh.depth_sum = 0.0;
        sh.probes = 0;
        sh.cells = 0;

        stack.clear();
        stack.push_back(start);
        cells[start].body = id;
        while (!stack.empty())
        {
            const int at = stack.back();
            stack.pop_back();
            water_cell& cell = cells[at];
            ++sh.cells;

            sh.extent.modify(Fvector{cell.span.min.x, cell.top.y, cell.span.min.y});
            sh.extent.modify(Fvector{cell.span.max.x, cell.top.y, cell.span.max.y});

            // One ray per cell, straight down from a real liquid centroid rather than from the
            // cell's centre: the extent of an L-shaped pond covers a lot of dry bank, and banks
            // would drag the mean depth to nothing.
            Fvector from = cell.top;
            from.y -= water_probe_drop;
            collide::rq_result bed;
            if (space.RayPick(from, Fvector{0.f, -1.f, 0.f}, water_probe_range, collide::rqtStatic, bed, nullptr))
            {
                // Another liquid surface below is not the bed either (stacked water, sheets
                // modelled two-sided).
                if (!(bed.element >= 0 && is_liquid(tris[bed.element])))
                {
                    sh.depth_sum += bed.range;
                    ++sh.probes;
                }
            }

            const int cx = at % water_grid;
            const int cz = at / water_grid;
            const int nb[4] = {cx > 0 ? at - 1 : -1, cx < water_grid - 1 ? at + 1 : -1,
                cz > 0 ? at - water_grid : -1, cz < water_grid - 1 ? at + water_grid : -1};
            for (const int n : nb)
            {
                if (n < 0 || !cells[n].used || cells[n].body >= 0)
                    continue;
                cells[n].body = id;
                stack.push_back(n);
            }
        }
    }
    if (sheets.empty())
        return;

    // Biggest first, then keep as many as the engine has room for. A level with a dozen puddles
    // loses the smallest of them to the pond-sized default, which is what they are anyway.
    std::sort(sheets.begin(), sheets.end(), [](const sheet& a, const sheet& b) { return a.cells > b.cells; });

    CEnvironment::SWaterBody bodies[CEnvironment::water_body_max];
    int written = 0;
    for (const sheet& sh : sheets)
    {
        if (written >= CEnvironment::water_body_max)
            break;
        if (!sh.extent.is_valid())
            continue;
        bodies[written].extent = sh.extent;
        bodies[written].depth = sh.probes ? float(sh.depth_sum / sh.probes) : water_depth_unknown;
        ++written;
    }
    env.set_water_bodies(bodies, written);
}
} // namespace

bool CLevel::Load_GameSpecific_Before()
{
    ZoneScoped;

    // AI space
    g_pGamePersistent->LoadTitle("st_loading_ai_objects");
    string_path fn_game;

    if (GamePersistent().GameType() == eGameIDSingle && !ai().get_alife() && FS.exist(fn_game, "$level$", "level.ai") &&
        !net_Hosts.empty())
        ai().load(net_SessionName());

    if (!GEnv.isDedicatedServer && !ai().get_alife() && ai().get_game_graph() && FS.exist(fn_game, "$level$", "level.game"))
    {
        IReader* stream = FS.r_open(fn_game);
        ai().patrol_path_storage_raw(*stream);
        FS.r_close(stream);
    }

    return (TRUE);
}

bool CLevel::Load_GameSpecific_After()
{
    ZoneScoped;

    R_ASSERT(m_StaticParticles.empty());
    // loading static particles
    string_path fn_game;
    if (FS.exist(fn_game, "$level$", "level.ps_static"))
    {
        ZoneScopedN("Load static particles");

        IReader* F = FS.r_open(fn_game);
        CParticlesObject* pStaticParticles;
        u32 chunk = 0;
        string256 ref_name;
        Fmatrix transform;
        Fvector zero_vel = {0.f, 0.f, 0.f};
        u32 ver = 0;
        for (IReader* OBJ = F->open_chunk_iterator(chunk); OBJ; OBJ = F->open_chunk_iterator(chunk, OBJ))
        {
            if (chunk == 0)
            {
                if (OBJ->length() == sizeof(u32))
                {
                    ver = OBJ->r_u32();
#ifndef MASTER_GOLD
                    Msg("PS new version, %d", ver);
#endif // #ifndef MASTER_GOLD
                    continue;
                }
            }
            u16 gametype_usage = 0;
            if (ver > 0)
            {
                gametype_usage = OBJ->r_u16();
            }
            OBJ->r_stringZ(ref_name, sizeof(ref_name));
            OBJ->r(&transform, sizeof(Fmatrix));
            transform.c.y += 0.01f;

            if ((g_pGamePersistent->m_game_params.m_e_game_type & EGameIDs(gametype_usage)) || (ver == 0))
            {
                pStaticParticles = CParticlesObject::Create(ref_name, FALSE, false);
                pStaticParticles->UpdateParent(transform, zero_vel);
                pStaticParticles->Play(false);
                m_StaticParticles.push_back(pStaticParticles);
            }
        }
        FS.r_close(F);
    }

    if (!GEnv.isDedicatedServer)
    {
        // loading static sounds
        VERIFY(m_level_sound_manager);
        m_level_sound_manager->Load();

        // loading sound environment
        if (FS.exist(fn_game, "$level$", "level.snd_env"))
        {
            IReader* F = FS.r_open(fn_game);
            Sound->set_geometry_env(F);
            FS.r_close(F);
        }
        // loading SOM
        if (FS.exist(fn_game, "$level$", "level.som"))
        {
            IReader* F = FS.r_open(fn_game);
            Sound->set_geometry_som(F);
            FS.r_close(F);
        }

        // loading random (around player) sounds
        if (pSettings->section_exist("sounds_random"))
        {
            ZoneScopedN("Load random sounds");

            CInifile::Sect& S = pSettings->r_section("sounds_random");
            Sounds_Random.reserve(S.Data.size());
            for (const auto& I : S.Data)
            {
                Sounds_Random.emplace_back().create(I.first.c_str(), st_Effect, sg_SourceType);
            }
            Sounds_Random_dwNextTime = Device.TimerAsync() + 50000;
            Sounds_Random_Enabled = FALSE;
        }

        if (g_pGamePersistent->pEnvironment && g_pGamePersistent->pEnvironment->eff_Rain)
            g_pGamePersistent->pEnvironment->eff_Rain->InvalidateState();

        if (FS.exist(fn_game, "$level$", "level.fog_vol"))
        {
            ZoneScopedN("Load fog volume");
            IReader* F = FS.r_open(fn_game);
            u16 version = F->r_u16();
            if (version == 2)
            {
                u32 cnt = F->r_u32();

                Fmatrix volume_matrix;
                for (u32 i = 0; i < cnt; ++i)
                {
                    F->r(&volume_matrix, sizeof(volume_matrix));
                    u32 sub_cnt = F->r_u32();
                    for (u32 is = 0; is < sub_cnt; ++is)
                    {
                        F->r(&volume_matrix, sizeof(volume_matrix));
                    }
                }
            }
            FS.r_close(F);
        }
    }

    if (!GEnv.isDedicatedServer)
    {
        // loading scripts
        auto& scriptEngine = *GEnv.ScriptEngine;
        scriptEngine.remove_script_process(ScriptProcessor::Level);
        shared_str scripts;
        if (pLevel->section_exist("level_scripts") && pLevel->line_exist("level_scripts", "script"))
            scripts = pLevel->r_string("level_scripts", "script");
        else
            scripts = "";
        scriptEngine.add_script_process(ScriptProcessor::Level, scriptEngine.CreateScriptProcess("level", scripts));
    }

    BlockCheatLoad();

    g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());

    // Here and not earlier: the collision model is loaded and Load_GameSpecific_CFORM has already
    // remapped the game-material ids onto its triangles, so flLiquid means what it says.
    measure_water_body(ObjectSpace);
    // Unconditionally, and not only where the sweep above found water. The field's water
    // channels come out empty on a dry level - which is correct, and every consumer gates on
    // coverage rather than on the map existing - but the puddle fill it bakes alongside them is
    // terrain concavity, and that is worth having on the levels that are nothing but terrain.
    bake_water_field(ObjectSpace);

    return TRUE;
}

struct translation_pair
{
    u32 m_id;
    u16 m_index;

    IC translation_pair(u32 id, u16 index)
    {
        m_id = id;
        m_index = index;
    }

    IC bool operator==(const u16& id) const { return (m_id == id); }
    IC bool operator<(const translation_pair& pair) const { return (m_id < pair.m_id); }
    IC bool operator<(const u16& id) const { return (m_id < id); }
};

struct translation_struct
{
    u16 m_id;
    u16 m_index;
    bool m_suppress_shadows;
    bool m_suppress_wm;
};

void CLevel::Load_GameSpecific_CFORM_Serialize(IWriter& writer)
{
    writer.w_u32(GMLib.GetLibraryCrc32());
}

bool CLevel::Load_GameSpecific_CFORM_Deserialize(IReader& reader)
{
    const auto materials_crc32 = GMLib.GetLibraryCrc32();
    const auto cached_materials_crc32 = reader.r_u32();
    return materials_crc32 == cached_materials_crc32;
}

void CLevel::Load_GameSpecific_CFORM(CDB::TRI* tris, u32 count)
{
    ZoneScoped;

    typedef xr_vector<translation_pair> ID_INDEX_PAIRS;
    ID_INDEX_PAIRS translator;
    translator.reserve(GMLib.CountMaterial());
    u16 default_id = (u16)GMLib.GetMaterialIdx("default");
    translator.emplace_back(u32(-1), default_id);

    u16 index = 0, static_mtl_count = 1;
    int max_ID = 0;
    int max_static_ID = 0;
    for (auto I = GMLib.FirstMaterial(); GMLib.LastMaterial() != I; ++I, ++index)
    {
        if (!(*I)->Flags.test(SGameMtl::flDynamic))
        {
            ++static_mtl_count;
            translator.emplace_back((*I)->GetID(), index);
            if ((*I)->GetID() > max_static_ID)
                max_static_ID = (*I)->GetID();
        }
        if ((*I)->GetID() > max_ID)
            max_ID = (*I)->GetID();
    }
    // Msg("* Material remapping ID: [Max:%d, StaticMax:%d]",max_ID,max_static_ID);
    VERIFY(max_static_ID < 0xFFFF);

    if (static_mtl_count < 128)
    {
        CDB::TRI* I = tris;
        CDB::TRI* E = tris + count;
        for (; I != E; ++I)
        {
            const auto i = std::find(translator.cbegin(), translator.cend(), (u16)(*I).material);
            if (i != translator.end())
            {
                (*I).material = (*i).m_index;
                const SGameMtl* mtl = GMLib.GetMaterialByIdx((*i).m_index);
                (*I).suppress_shadows = mtl && mtl->Flags.is(SGameMtl::flSuppressShadows);
                (*I).suppress_wm = mtl && mtl->Flags.is(SGameMtl::flSuppressWallmarks);
                continue;
            }

            xrDebug::Fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
        }
        return;
    }

    std::sort(translator.begin(), translator.end());
    {
        CDB::TRI* I = tris;
        CDB::TRI* E = tris + count;
        for (; I != E; ++I)
        {
            const auto i = std::lower_bound(translator.cbegin(), translator.cend(), (u16)(*I).material);
            if ((i != translator.cend()) && ((*i).m_id == (*I).material))
            {
                (*I).material = (*i).m_index;
                const SGameMtl* mtl = GMLib.GetMaterialByIdx((*i).m_index);
                (*I).suppress_shadows = mtl->Flags.is(SGameMtl::flSuppressShadows);
                (*I).suppress_wm = mtl->Flags.is(SGameMtl::flSuppressWallmarks);
                continue;
            }

            xrDebug::Fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
        }
    }
}

void CLevel::Load_GameSpecific_CFORM_SetMaterials(CDB::TRI* tris, u32 count, xr_map<u16, shared_str>& gameMtls)
{
    // SkyLoader: reassing material indexes because they could have changed after various gamemtl.xr edits
    xr_vector<translation_struct> translator;
    translator.reserve(gameMtls.size());
    for (const auto& [id, mtlName] : gameMtls)
    {
        SGameMtl* mtl = GMLib.GetMaterial(mtlName.c_str());
        R_ASSERT2(mtl, make_string("Game material '%s' not found", mtlName.c_str()).c_str());

        translation_struct mat;
        mat.m_id = id;
        mat.m_index = GMLib.GetMaterialIdx(mtlName.c_str());
        mat.m_suppress_shadows = mtl->Flags.is(SGameMtl::flSuppressShadows);
        mat.m_suppress_wm = mtl->Flags.is(SGameMtl::flSuppressWallmarks);
        translator.emplace_back(std::move(mat));
    }

    CDB::TRI* I = tris;
    CDB::TRI* E = tris + count;
    for (; I != E; ++I)
    {
        auto it = std::find_if(translator.begin(), translator.end(), [=](const translation_struct& mat) { return mat.m_id == (u16)(*I).material; });
        if (it != translator.end())
        {
            (*I).material = (*it).m_index;
            (*I).suppress_shadows = (*it).m_suppress_shadows;
            (*I).suppress_wm = (*it).m_suppress_wm;
        }
        else
            xrDebug::Fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
    }
}


void CLevel::BlockCheatLoad()
{
#ifndef DEBUG
    if (game && (GameID() != eGameIDSingle))
        phTimefactor = 1.f;
#endif
}
