// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"

#include "da_opt_bench.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "Level.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/graph_engine.h"
#include "xrCDB/xrCDB.h"
#include "xrCDB/Frustum.h"
#include "xrEngine/device.h"

#include <algorithm>

namespace
{
// A private xorshift: the benchmark must not disturb the game's own random
// sequence, and it must produce the same workload on every run and every build.
class bench_rng
{
    u32 m_state;

public:
    explicit bench_rng(u32 seed) : m_state(seed ? seed : 0x9e3779b9u) {}

    u32 next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return m_state;
    }

    // [0, 1)
    float unit() { return float(next() >> 8) * (1.f / 16777216.f); }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

struct bench_totals
{
    u64 queries{};
    u64 hits{};
    double range_sum{};
    u64 material_sum{};
};

// Rays that look like the ones the game actually casts: they start inside the level
// volume and point anywhere, with a reach a bullet or a sight check would use.
void make_ray(bench_rng& rng, const Fbox& bb, Fvector& origin, Fvector& direction)
{
    origin.set(rng.range(bb.vMin.x, bb.vMax.x), rng.range(bb.vMin.y, bb.vMax.y), rng.range(bb.vMin.z, bb.vMax.z));

    // Rejection-free direction: a normalized gaussian-ish sample is overkill here, the
    // point is only that the set is fixed and covers every octant.
    do
    {
        direction.set(rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f));
    } while (direction.square_magnitude() < EPS_S);
    direction.normalize();
}

void run_ray_mode(pcstr label, u32 mode, CDB::MODEL* model, const Fbox& bb, u32 count, float reach, u32 seed)
{
    CDB::COLLIDER collider;
    bench_rng rng(seed);
    bench_totals totals;

    CTimer timer;
    timer.Start();
    for (u32 i = 0; i < count; ++i)
    {
        Fvector origin, direction;
        make_ray(rng, bb, origin, direction);

        collider.ray_query(mode, model, origin, direction, reach);

        const size_t found = collider.r_count();
        totals.queries++;
        totals.hits += found;
        const CDB::RESULT* it = found ? collider.r_begin() : nullptr;
        for (size_t r = 0; r < found; ++r)
        {
            totals.range_sum += double(it[r].range);
            totals.material_sum += it[r].dummy;
        }
    }
    const float seconds = timer.GetElapsed_sec();

    Msg("~ [optbench] cdb.%s rays=%u time=%.3f ms rate=%.0f/s hits=%llu range_sum=%.6f mat=%llu", label, count,
        seconds * 1000.f, seconds > 0.f ? float(count) / seconds : 0.f, totals.hits, totals.range_sum,
        totals.material_sum);
}

void run_box(CDB::MODEL* model, const Fbox& bb, u32 count, u32 seed)
{
    CDB::COLLIDER collider;
    bench_rng rng(seed);
    bench_totals totals;

    CTimer timer;
    timer.Start();
    for (u32 i = 0; i < count; ++i)
    {
        Fvector center;
        center.set(rng.range(bb.vMin.x, bb.vMax.x), rng.range(bb.vMin.y, bb.vMax.y), rng.range(bb.vMin.z, bb.vMax.z));
        const Fvector dim{ rng.range(0.5f, 3.f), rng.range(0.5f, 3.f), rng.range(0.5f, 3.f) };

        collider.box_query(CDB::OPT_FULL_TEST, model, center, dim);

        const size_t found = collider.r_count();
        totals.queries++;
        totals.hits += found;
        const CDB::RESULT* it = found ? collider.r_begin() : nullptr;
        for (size_t r = 0; r < found; ++r)
            totals.material_sum += it[r].dummy;
    }
    const float seconds = timer.GetElapsed_sec();

    Msg("~ [optbench] cdb.box boxes=%u time=%.3f ms rate=%.0f/s hits=%llu mat=%llu", count, seconds * 1000.f,
        seconds > 0.f ? float(count) / seconds : 0.f, totals.hits, totals.material_sum);
}

class CCC_BenchCDB : public IConsole_Command
{
public:
    CCC_BenchCDB(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
        {
            Msg("! [optbench] dar_bench_cdb: no level loaded");
            return;
        }

        u32 count = 20000;
        if (args && xr_strlen(args))
            sscanf(args, "%u", &count);
        if (count < 100)
            count = 100;
        if (count > 2000000)
            count = 2000000;

        CDB::MODEL* model = Level().ObjectSpace.GetStaticModel();
        const Fbox& bb = Level().ObjectSpace.GetBoundingVolume();

        Msg("~ [optbench] cdb level bbox (%.1f %.1f %.1f)-(%.1f %.1f %.1f) tris=%u", VPUSH(bb.vMin), VPUSH(bb.vMax),
            model->get_tris_count());

        // The four modes the game actually asks for, each on the same ray set.
        run_ray_mode("nearest_cull", CDB::OPT_CULL | CDB::OPT_ONLYNEAREST, model, bb, count, 150.f, 0x5eed0001);
        run_ray_mode("nearest", CDB::OPT_ONLYNEAREST, model, bb, count, 150.f, 0x5eed0001);
        run_ray_mode("first_cull", CDB::OPT_CULL | CDB::OPT_ONLYFIRST, model, bb, count, 150.f, 0x5eed0001);
        run_ray_mode("all", 0, model, bb, count, 150.f, 0x5eed0001);
        run_ray_mode("nearest_short", CDB::OPT_CULL | CDB::OPT_ONLYNEAREST, model, bb, count, 10.f, 0x5eed0002);
        run_box(model, bb, count / 10, 0x5eed0003);

        Msg("~ [optbench] cdb done");
    }
};

// Frame time end to end. The targeted benches above cannot see physics, AI scheduling or the
// renderer; this one sees all of it and nothing else, so it is the regression gate for every
// change that does not have a bench of its own.
class frame_sampler : public pureFrame
{
    xr_vector<float> m_samples;
    u32 m_remaining{};
    bool m_registered{};

public:
    void start(u32 frames)
    {
        m_samples.clear();
        m_samples.reserve(frames);
        m_remaining = frames;
        if (!m_registered)
        {
            Device.seqFrame.Add(this, REG_PRIORITY_LOW);
            m_registered = true;
        }
    }

    void OnFrame() override
    {
        if (!m_remaining)
            return;
        // Unscaled wall clock: time_factor must not be able to flatter or punish a run.
        m_samples.push_back(Device.fTimeDeltaUnscaled);
        if (--m_remaining)
            return;

        Device.seqFrame.Remove(this);
        m_registered = false;
        report();
    }

private:
    void report()
    {
        if (m_samples.size() < 8)
        {
            Msg("! [optbench] frame: not enough samples");
            return;
        }
        // Drop the first few: the frame the command was typed on carries the console.
        xr_vector<float> s(m_samples.begin() + 4, m_samples.end());
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (float v : s)
            sum += double(v);
        const double avg = sum / double(s.size());
        const float med = s[s.size() / 2];
        const float p95 = s[(s.size() * 95) / 100];
        Msg("~ [optbench] frame frames=%u avg=%.4f ms median=%.4f ms p95=%.4f ms fps=%.1f", u32(s.size()),
            avg * 1000.0, med * 1000.f, p95 * 1000.f, avg > 0.0 ? 1.0 / avg : 0.0);
        Msg("~ [optbench] frame done");
    }
};

static frame_sampler g_frame_sampler;

class CCC_BenchFrame : public IConsole_Command
{
public:
    CCC_BenchFrame(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
        {
            Msg("! [optbench] dar_bench_frame: no level loaded");
            return;
        }
        u32 frames = 1200;
        if (args && xr_strlen(args))
            sscanf(args, "%u", &frames);
        if (frames < 32)
            frames = 32;
        if (frames > 100000)
            frames = 100000;
        g_frame_sampler.start(frames);
        Msg("~ [optbench] frame sampling %u frame(s)", frames);
    }
};

class CCC_BenchPath : public IConsole_Command
{
public:
    CCC_BenchPath(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel || !ai().get_level_graph())
        {
            Msg("! [optbench] dar_bench_path: no level graph");
            return;
        }

        u32 count = 200;
        if (args && xr_strlen(args))
            sscanf(args, "%u", &count);
        if (count < 4)
            count = 4;
        if (count > 20000)
            count = 20000;

        const CLevelGraph& graph = ai().level_graph();
        const u32 vertices = graph.header().vertex_count();
        if (vertices < 2)
        {
            Msg("! [optbench] dar_bench_path: level graph has %u vertices", vertices);
            return;
        }

        using evaluator_type = SBaseParameters<float, u32, u32>;

        bench_rng rng(0x5eed0101);
        xr_vector<u32> path;
        u64 found = 0, path_nodes = 0;
        double length_sum = 0.0;

        CTimer timer;
        timer.Start();
        for (u32 i = 0; i < count; ++i)
        {
            const u32 from = rng.next() % vertices;
            const u32 to = rng.next() % vertices;
            path.clear();

            const bool ok = ai().graph_engine().search(graph, from, to, &path,
                evaluator_type(type_max<float>, u32(-1), 4096));
            if (ok)
            {
                found++;
                path_nodes += path.size();
                // Path length in metres, so a change in the route shows up and a change in
                // node count alone does not hide behind it.
                for (size_t n = 1; n < path.size(); ++n)
                    length_sum += double(graph.vertex_position(path[n]).distance_to(graph.vertex_position(path[n - 1])));
            }
        }
        const float seconds = timer.GetElapsed_sec();

        Msg("~ [optbench] path searches=%u time=%.3f ms rate=%.1f/s found=%llu nodes=%llu length=%.4f", count,
            seconds * 1000.f, seconds > 0.f ? float(count) / seconds : 0.f, found, path_nodes, length_sum);
        Msg("~ [optbench] path done");
    }
};
} // namespace

void da_opt_bench_register()
{
    CMD1(CCC_BenchCDB, "dar_bench_cdb");
    CMD1(CCC_BenchPath, "dar_bench_path");
    CMD1(CCC_BenchFrame, "dar_bench_frame");
}
