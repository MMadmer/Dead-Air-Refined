// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_tactics.h"
#include "da_ai_morale.h"
#include "da_ai_npc.h"

#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "ai_space.h"
#include "cover_evaluators.h"
#include "cover_manager.h"
#include "cover_point.h"
#include "restricted_object.h"
#include "stalker_movement_restriction.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/ai_object_location.h"

// Cover scoring and path exposure of the tactics lane. Everything is measured from the NPC's belief of the threat,
// never from the target object (F3), on the level graph's per-direction cover values (no rays).

namespace DaAi::Tactics
{
namespace
{
// Weapon band, aggression and cover discipline of the NPC, once per frame for all covers of a search; whether it stands
// exposed toward the threat, once per frame and threat.
void score_context(const CAI_Stalker& npc, TacticsState& state, const Fvector& threat)
{
    if (state.score_frame == Device.dwFrame && state.score_threat.similar(threat, EPS_L))
        return;
    state.score_frame = Device.dwFrame;
    state.score_threat = threat;
    state.score_band = band(npc);
    state.score_aggr = Morale::aggression(npc);
    state.score_cover = Morale::cover(npc);
    state.score_exposed = exposed(npc, threat);
}

// The next bound of a covered approach: a cover that cuts the known distance by at least approach_min_gain_m, holds
// cover toward the belief and is reached without crossing more open ground than the weapon allows.
class CDaAiApproachEvaluator final : public CCoverEvaluatorBase
{
    using inherited = CCoverEvaluatorBase;

    CAI_Stalker* m_npc{};
    Fvector m_threat{};
    float m_threat_distance{};
    WeaponBand m_band{};
    float m_cover{};
    float m_floor{};
    float m_limit{};
    bool m_limited{};

public:
    CDaAiApproachEvaluator(CAI_Stalker& npc) : inherited(&npc.movement().restrictions()), m_npc(&npc) {}

    void setup(const Fvector& threat, float threat_distance, const WeaponBand& weapon_band, float cover, float floor)
    {
        inherited::setup();
        m_threat = threat;
        m_threat_distance = threat_distance;
        m_band = weapon_band;
        m_cover = cover;
        m_floor = _max(floor, weapon_band.min);
        // A bound always closes in: a short gun's allowance toward the enemy.
        m_limited = open_limit(*m_npc, true, m_limit);
    }

    void evaluate_cover(const CCoverPoint* cover_point, float weight) override
    {
        if (fis_zero(weight))
            return;

        const Config& c = config();
        const float distance = m_threat.distance_to(cover_point->position());
        if (distance > m_threat_distance - c.approach_min_gain_m || distance < m_floor)
            return;
        if (cover_toward(cover_point->level_vertex_id(), cover_point->position(), m_threat) > c.approach_cover_max)
            return;

        // Prefer the bound nearest to the effective range, then the short and covered ones.
        const float target = _max(m_band.effective, m_band.min);
        float value = _abs(distance - target) / _max(target, 1.f);
        value += m_start_position.distance_to(cover_point->position()) * c.score_travel;
        if (value / weight >= m_best_value)
            return;

        const float exposure =
            path_exposure(*m_npc, cover_point->position(), m_threat, m_limited ? m_limit : c.approach_radius_m);
        if (m_limited && exposure > m_limit)
            return;
        value += exposure * c.score_exposure * (.5f + m_cover);
        value /= weight;
        if (value >= m_best_value)
            return;

        m_selected = cover_point;
        m_best_value = value;
    }

    void evaluate_smart_cover(smart_cover::cover const* smart_cover, float const& weight) override {}
};

// Fall back: a cover farther from the belief and never toward it; for fanatics a relocation that is not farther from
// the belief than the current position.
class CDaAiFallBackEvaluator final : public CCoverEvaluatorBase
{
    using inherited = CCoverEvaluatorBase;

    CAI_Stalker* m_npc{};
    Fvector m_threat{};
    float m_threat_distance{};
    bool m_fanatic{};
    float m_cover{};

public:
    CDaAiFallBackEvaluator(CAI_Stalker& npc) : inherited(&npc.movement().restrictions()), m_npc(&npc) {}

    void setup(const Fvector& threat, float threat_distance, bool fanatic, float cover)
    {
        inherited::setup();
        m_threat = threat;
        m_threat_distance = threat_distance;
        m_fanatic = fanatic;
        m_cover = cover;
    }

    void evaluate_cover(const CCoverPoint* cover_point, float weight) override
    {
        if (fis_zero(weight))
            return;

        const Config& c = config();
        const Fvector& position = cover_point->position();
        const float distance = m_threat.distance_to(position);
        const float travel = m_start_position.distance_to(position);
        if (m_fanatic)
        {
            if (distance > m_threat_distance + EPS_L || travel < c.fallback_min_move_m)
                return;
        }
        else
        {
            if (distance < m_threat_distance + c.fallback_min_gain_m)
                return;
            const Fvector to_cover = Fvector().sub(position, m_start_position);
            const Fvector to_threat = Fvector().sub(m_threat, m_start_position);
            if (to_cover.dotproduct(to_threat) > 0.f)
                return;
        }

        const float cover_value = cover_toward(cover_point->level_vertex_id(), position, m_threat);
        if (cover_value > c.approach_cover_max)
            return;

        float value = cover_value + travel * c.score_travel;
        if (value / weight >= m_best_value)
            return;
        value += path_exposure(*m_npc, position, m_threat, c.fallback_radius_m) * c.score_exposure * (.5f + m_cover);
        value /= weight;
        if (value >= m_best_value)
            return;

        m_selected = cover_point;
        m_best_value = value;
    }

    void evaluate_smart_cover(smart_cover::cover const* smart_cover, float const& weight) override {}
};

// A route candidate of the flank searches: the cover and its cheap geometry terms; the agent's checks and the path
// exposure, the costly parts, are judged afterwards for the cheapest few only ([ai_tactics] flank_agent_checks,
// flank_checks): a search over a wide radius stays bounded.
struct RouteCandidate
{
    const CCoverPoint* cover;
    float value;
};

// Main thread only (the scheduler is serial): one scratch list for every flank search.
xr_vector<RouteCandidate> g_route_candidates;

bool cheaper(const RouteCandidate& a, const RouteCandidate& b) { return a.value < b.value; }

// The agent's checks of a route cover (restrictions, the mates' covers, the known enemies): 0 = refused, else the
// danger weight (1 = no danger), which divides the cover's value.
float agent_weight(CAI_Stalker& npc, const CStalkerMovementRestrictor& restrictor, const CCoverPoint& cover)
{
    return npc.movement().restrictions().accessible(cover.position()) && restrictor(&cover) ?
        restrictor.weight(&cover) :
        0.f;
}

// Flank: a cover on the ordered side of the line from the belief to the NPC and well off it, at the weapon's range,
// covered toward the belief; the open ground of the way to it (the combat-vector cost of the approach) is judged by
// judge_flank_leg.
class CDaAiFlankEvaluator final : public CCoverEvaluatorBase
{
    using inherited = CCoverEvaluatorBase;

    Fvector m_threat{};
    Fvector m_anchor{};
    u8 m_side{};
    float m_cos_near{}; // cos of the smallest angle off the line
    float m_cos_far{}; // cos of the largest
    float m_near{};
    float m_far{};
    float m_desired{};

public:
    CDaAiFlankEvaluator(CAI_Stalker& npc) : inherited(&npc.movement().restrictions()) {}

    void setup(const Fvector& threat, const Fvector& anchor, u8 side, const WeaponBand& weapon_band)
    {
        inherited::setup();
        g_route_candidates.clear();
        const Config& c = config();
        m_threat = threat;
        m_anchor = anchor;
        m_side = side;
        m_cos_near = _cos(deg2rad(c.flank_angle_deg.lo));
        m_cos_far = _cos(deg2rad(c.flank_angle_deg.hi));
        const float current = threat.distance_to_xz(anchor);
        m_near = _max(_max(weapon_band.min, c.cover_min_enemy_m), 1.f);
        m_desired = _max(m_near, _min(current, weapon_band.effective > 0.f ? weapon_band.effective : current));
        m_far = _max(m_desired, current) + c.approach_min_gain_m;
    }

    void evaluate_cover(const CCoverPoint* cover_point, float weight) override
    {
        if (fis_zero(weight))
            return;

        const Config& c = config();
        const Fvector& position = cover_point->position();
        Fvector offset = Fvector().sub(position, m_threat);
        offset.y = 0.f;
        const float distance = offset.magnitude();
        if (distance < m_near || distance > m_far)
            return;
        Fvector axis = Fvector().sub(m_anchor, m_threat);
        axis.y = 0.f;
        const float axis_length = axis.magnitude();
        if (axis_length < EPS_L)
            return;
        const float cos_off = offset.dotproduct(axis) / (distance * axis_length);
        if (cos_off > m_cos_near || cos_off < m_cos_far || side_of(m_threat, m_anchor, position) != m_side)
            return;
        if (cover_toward(cover_point->level_vertex_id(), position, m_threat) > c.approach_cover_max)
            return;

        float value = _abs(distance - m_desired) / _max(m_desired, 1.f);
        value += m_start_position.distance_to(position) * c.score_travel;
        g_route_candidates.push_back({cover_point, value / weight});
    }

    void evaluate_smart_cover(smart_cover::cover const* smart_cover, float const& weight) override {}

    // A circle holding the whole area the geometry accepts (m_near..m_far from the belief, flank_angle_deg off the
    // line to the anchor on the side): a search needs to walk only the covers of that circle.
    bool bounds(Fvector& center, float& radius) const
    {
        Fvector axis = Fvector().sub(m_anchor, m_threat);
        axis.y = 0.f;
        const float length = axis.magnitude();
        if (length < EPS_L)
            return false;
        axis.div(length);
        const float lo = acosf(clampr(m_cos_near, -1.f, 1.f));
        const float hi = acosf(clampr(m_cos_far, -1.f, 1.f));
        const float k = _cos((hi - lo) * .5f);
        if (k < .1f)
            return false;
        // The axis turned by the middle angle toward the side (side_of: side 0 lies on the positive turn).
        const float mid = (m_side ? -.5f : .5f) * (lo + hi);
        const Fvector bisector = Fvector().set(axis.x * _cos(mid) - axis.z * _sin(mid), 0.f,
            axis.x * _sin(mid) + axis.z * _cos(mid));
        // For a centre on the bisector the farthest points of the sector are its edge ends at m_near and m_far; the
        // centre that makes the two equal, or the one that is best for the far arc alone.
        float distance = m_far * k;
        if (2.f * k * k * m_far > m_far + m_near)
            distance = (m_far + m_near) / (2.f * k);
        const float far_sqr = _sqr(m_far) - 2.f * m_far * distance * k + _sqr(distance);
        const float near_sqr = _sqr(m_near) - 2.f * m_near * distance * k + _sqr(distance);
        center = Fvector().mad(m_threat, bisector, distance);
        radius = _sqrt(_max(_max(far_sqr, near_sqr), 0.f)) + 1.f;
        return true;
    }
};

// The first leg of a two-leg flank: covers on the ordered side, well out from the line from the belief to the NPC,
// covered toward the belief; the open ground of the way to them is judged by judge_flank_vias, which keeps the best
// few (the second leg is searched from each of them).
class CDaAiFlankViaEvaluator final : public CCoverEvaluatorBase
{
    using inherited = CCoverEvaluatorBase;

    Fvector m_threat{};
    Fvector m_anchor{};
    Fvector m_axis{}; // unit, from the belief to the NPC
    u8 m_side{};
    float m_near{};
    float m_far{};

public:
    CDaAiFlankViaEvaluator(CAI_Stalker& npc) : inherited(&npc.movement().restrictions()) {}

    void setup(const Fvector& threat, const Fvector& anchor, u8 side, const WeaponBand& weapon_band)
    {
        inherited::setup();
        g_route_candidates.clear();
        const Config& c = config();
        m_threat = threat;
        m_anchor = anchor;
        m_axis = Fvector().sub(m_anchor, threat);
        m_axis.y = 0.f;
        const float current = m_axis.magnitude();
        m_axis.div(_max(current, EPS_L));
        m_side = side;
        m_near = _max(_max(weapon_band.min, c.cover_min_enemy_m), 1.f);
        m_far = current + c.approach_min_gain_m;
    }

    void evaluate_cover(const CCoverPoint* cover_point, float weight) override
    {
        if (fis_zero(weight))
            return;

        const Config& c = config();
        const Fvector& position = cover_point->position();
        Fvector offset = Fvector().sub(position, m_threat);
        offset.y = 0.f;
        const float distance = offset.magnitude();
        if (distance < m_near || distance > m_far || side_of(m_threat, m_anchor, position) != m_side)
            return;
        // Out from the line: the lateral distance gained is the progress of the first leg.
        const float lateral = _abs(m_axis.z * offset.x - m_axis.x * offset.z);
        if (lateral < c.approach_min_gain_m)
            return;
        if (cover_toward(cover_point->level_vertex_id(), position, m_threat) > c.approach_cover_max)
            return;

        const float travel = m_start_position.distance_to(position) * c.score_travel;
        const float value = travel - lateral / _max(c.flank_via_radius_m, 1.f);
        g_route_candidates.push_back({cover_point, value / weight});
    }

    void evaluate_smart_cover(smart_cover::cover const* smart_cover, float const& weight) override {}

    // Its search disk (flank_via_radius_m) is already small.
    bool bounds(Fvector& center, float& radius) const { return false; }
};

// The covers of a route search around a point, like the cover manager's best_cover but cheapest test first: only the
// evaluator's geometry runs here, and only the [ai_tactics] flank_agent_checks best candidates by it are kept for the
// agent's checks (restrictions, the mates' covers, the known enemies, the danger weight), which the judging gives them
// a few per update. The cover quadtree's walk costs by the covers of the circle it queries (a 55 m disk held
// thousands, ~0.3 ms): the smaller of the search disk and the circle around the evaluator's own area is walked, in
// tiles of flank_tile_m while the step's budget lasts; both limits hold for every cover, so the same covers are found.
// The leg keeps the best so far between steps; true once every tile is done (the leg sorted, best first).
template <typename Evaluator>
bool gather_route(TacticsState::FlankSearch::Leg& leg, float radius, Evaluator& evaluator, StepBudget& budget)
{
    // Main thread only (the scheduler is serial).
    static xr_vector<CCoverPoint*> covers;
    if (!leg.tiles.active)
    {
        Fvector query = leg.origin;
        float query_radius = radius;
        Fvector area;
        if (float area_radius; evaluator.bounds(area, area_radius) && area_radius < radius)
        {
            query = area;
            query_radius = area_radius;
        }
        start_cover_tiles(leg.tiles, query, query_radius, 0.f, config().flank_tile_m);
    }

    // The evaluator's setup emptied the scratch list: the best of the earlier tiles go back into it.
    for (u8 index = 0; index < leg.count; ++index)
        g_route_candidates.push_back({leg.cover[index], leg.value[index]});
    evaluator.initialize(leg.origin);
    const float radius_sqr = _sqr(radius);
    bool more = true;
    while (budget.next() && (more = next_cover_tile(leg.tiles, covers)))
    {
        for (const CCoverPoint* cover : covers)
        {
            // The stock best_cover's height window of 3 m.
            const Fvector& at = cover->position();
            if (leg.origin.distance_to_sqr(at) > radius_sqr || _abs(leg.origin.y - at.y) > 3.f)
                continue;
            evaluator.evaluate_cover(cover, 1.f);
        }
    }
    evaluator.finalize();

    const size_t keep = config().flank_agent_checks;
    if (g_route_candidates.size() > keep)
    {
        std::nth_element(
            g_route_candidates.begin(), g_route_candidates.begin() + keep, g_route_candidates.end(), cheaper);
        g_route_candidates.resize(keep);
    }
    if (!more)
        std::sort(g_route_candidates.begin(), g_route_candidates.end(), cheaper);
    leg.count = u8(g_route_candidates.size());
    for (u8 index = 0; index < leg.count; ++index)
    {
        leg.cover[index] = g_route_candidates[index].cover;
        leg.value[index] = g_route_candidates[index].value;
    }
    g_route_candidates.clear();
    if (more)
        return false;
    leg.gathered = true;
    leg.best_value = flt_max;
    return true;
}

// A new gather of a leg: from the search centre, with the NPC's place and weapon as they are now for all its tiles.
void begin_gather(TacticsState::FlankSearch::Leg& leg, const Fvector& origin, const Fvector& anchor,
    const WeaponBand& weapon_band)
{
    leg = {};
    leg.origin = origin;
    leg.anchor = anchor;
    leg.band = weapon_band;
}

// A short gun's bound toward a goal of the search: a cover nearer the goal, covered toward the belief, reached over at
// most its allowance of open ground; the nearest to the goal wins.
class CDaAiStepEvaluator final : public CCoverEvaluatorBase
{
    using inherited = CCoverEvaluatorBase;

    CAI_Stalker* m_npc{};
    Fvector m_goal{};
    Fvector m_threat{};
    float m_goal_distance{};
    float m_limit{};

public:
    CDaAiStepEvaluator(CAI_Stalker& npc) : inherited(&npc.movement().restrictions()), m_npc(&npc) {}

    void setup(const Fvector& goal, const Fvector& threat, float limit)
    {
        inherited::setup();
        m_goal = goal;
        m_threat = threat;
        m_goal_distance = m_npc->Position().distance_to(goal);
        m_limit = limit;
    }

    void evaluate_cover(const CCoverPoint* cover_point, float weight) override
    {
        if (fis_zero(weight))
            return;

        const Config& c = config();
        const Fvector& position = cover_point->position();
        const float distance = position.distance_to(m_goal);
        if (distance > m_goal_distance - c.approach_min_gain_m)
            return;
        if (cover_toward(cover_point->level_vertex_id(), position, m_threat) > c.approach_cover_max)
            return;

        float value = distance / _max(m_goal_distance, 1.f) + m_start_position.distance_to(position) * c.score_travel;
        value /= weight;
        if (value >= m_best_value)
            return;
        if (path_exposure(*m_npc, position, m_threat, m_limit) > m_limit)
            return;

        m_selected = cover_point;
        m_best_value = value;
    }

    void evaluate_smart_cover(smart_cover::cover const* smart_cover, float const& weight) override {}
};

// No cover away from the belief: the farthest reachable straight line away from it, turning up to 90 degrees.
bool point_away(CAI_Stalker& npc, const Fvector& threat, float radius, Fvector& position, u32& vertex)
{
    const CLevelGraph& graph = ai().level_graph();
    const u32 start_vertex = npc.ai_location().level_vertex_id();
    if (!graph.valid_vertex_id(start_vertex))
        return false;

    Fvector away = Fvector().sub(npc.Position(), threat);
    away.y = 0.f;
    if (away.square_magnitude() < EPS_L)
        return false;
    away.normalize();

    float yaw, pitch;
    away.getHP(yaw, pitch);
    for (const float turn : {0.f, PI_DIV_4, -PI_DIV_4, PI_DIV_2, -PI_DIV_2})
    {
        for (const float share : {1.f, .5f})
        {
            const Fvector target = Fvector().mad(npc.Position(), Fvector().setHP(yaw + turn, 0.f), radius * share);
            const u32 found = graph.check_position_in_direction(start_vertex, npc.Position(), target);
            if (!graph.valid_vertex_id(found) || !npc.movement().accessible(found))
                continue;
            vertex = found;
            position = graph.vertex_position(found);
            return true;
        }
    }
    return false;
}
} // namespace

float cover_toward(u32 vertex_id, const Fvector& from, const Fvector& threat)
{
    Fvector direction = Fvector().sub(threat, from);
    float yaw, pitch;
    direction.getHP(yaw, pitch);
    yaw = angle_normalize(yaw);
    const CLevelGraph& graph = ai().level_graph();
    return std::min(graph.high_cover_in_direction(yaw, vertex_id), graph.low_cover_in_direction(yaw, vertex_id));
}

bool exposed(const CAI_Stalker& npc, const Fvector& threat)
{
    const u32 vertex = npc.ai_location().level_vertex_id();
    return ai().level_graph().valid_vertex_id(vertex) &&
        cover_toward(vertex, npc.Position(), threat) > config().cover_open_value;
}

bool short_range(const WeaponBand& weapon_band)
{
    return weapon_band.engage_max > 0.f && weapon_band.engage_max <= config().short_range_engage_max;
}

float path_exposure(const CAI_Stalker& npc, const Fvector& to, const Fvector& threat, float stop_at)
{
    return path_exposure(npc.ai_location().level_vertex_id(), npc.Position(), to, threat, stop_at);
}

float path_exposure(u32 start_vertex, const Fvector& start, const Fvector& to, const Fvector& threat, float stop_at)
{
    const CLevelGraph& graph = ai().level_graph();
    const float straight = start.distance_to_xz(to);
    if (!graph.valid_vertex_id(start_vertex) || straight < EPS_L)
        return 0.f;

    // A grid walk along the straight line: each step leaves the cell through the side the line crosses first, into the
    // cell linked on that side, and counts the cell with the length of the line inside it; it stops as soon as the open
    // metres pass stop_at. The stock walk (CLevelGraph::mark_nodes_in_direction) intersected every neighbour's contour
    // at every cell, ~1 us a cell: four 50 m walks of a flank search step were a third of a millisecond (P10).
    const Config& c = config();
    const float cell_size = graph.header().cell_size();
    const float half = cell_size * .5f;
    const float dir_x = (to.x - start.x) / straight;
    const float dir_z = (to.z - start.z) / straight;
    const int step_x = dir_x > 0.f ? 1 : -1;
    const int step_z = dir_z > 0.f ? 1 : -1;
    // Where along the line it leaves a cell through its x or z side.
    const auto leave = [half](float center, float from, float dir, int step) {
        return _abs(dir) > EPS_S ? (center + half * float(step) - from) / dir : flt_max;
    };
    const auto linked = [&graph](u32 vertex, int x, int z) {
        CLevelGraph::const_iterator link, links_end;
        graph.begin(vertex, link, links_end);
        for (; link != links_end; ++link)
        {
            const u32 neighbour = graph.value(vertex, link);
            if (!graph.valid_vertex_id(neighbour))
                continue;
            int neighbour_x, neighbour_z;
            graph.unpack_xz(graph.vertex(neighbour), neighbour_x, neighbour_z);
            if (neighbour_x == x && neighbour_z == z)
                return neighbour;
        }
        return u32(-1);
    };
    const auto open_cell = [&](u32 vertex) {
        return cover_toward(vertex, graph.vertex_position(vertex), threat) > c.cover_open_value;
    };
    u32 vertex = start_vertex;
    float entered = 0.f; // where the line entered the current cell
    bool on_route = false; // the NPC's own cell is no part of the route
    float open = 0.f;
    // Every step moves one cell along the line's direction: a start cell off the line cannot walk forever.
    for (u32 steps = u32(2.f * straight / cell_size) + 8; steps && !graph.inside(vertex, to) && entered < straight;
         --steps)
    {
        const CLevelGraph::CLevelVertex* current = graph.vertex(vertex);
        float center_x, center_z;
        graph.unpack_xz(current, center_x, center_z);
        int cell_x, cell_z;
        graph.unpack_xz(current, cell_x, cell_z);
        const float leave_x = leave(center_x, start.x, dir_x, step_x);
        const float leave_z = leave(center_z, start.z, dir_z, step_z);
        // At a corner either side will do: the other one when this one has no link.
        const bool by_x = leave_x <= leave_z;
        u32 next = linked(vertex, cell_x + (by_x ? step_x : 0), cell_z + (by_x ? 0 : step_z));
        if (!graph.valid_vertex_id(next) && _abs(leave_x - leave_z) < EPS_L)
            next = linked(vertex, cell_x + (by_x ? 0 : step_x), cell_z + (by_x ? step_z : 0));
        // A wall on the straight line: the real path goes around it, its open share is a guess.
        if (!graph.valid_vertex_id(next))
            return open + _max(0.f, straight - entered) * c.exposure_unknown_share;

        const float left = _min(_max(_min(leave_x, leave_z), entered), straight);
        if (on_route && open_cell(vertex))
        {
            open += left - entered;
            if (open > stop_at)
                return open;
        }
        on_route = true;
        entered = left;
        vertex = next;
    }
    if (on_route && open_cell(vertex))
        open += _max(0.f, straight - entered);
    return open;
}

bool cover_terms(const CAI_Stalker& npc, const CCoverPoint& cover, const Fvector& threat, const Fvector& start,
    float threat_distance, float current_distance, float bound, float& value)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::cover_score))
        return true;

    const NpcState* owner = npc.da_ai();
    ScopedTimer timer(Stat::p_tactics_us, owner->community);
    score_context(npc, *state, threat);
    const Config& c = config();
    const WeaponBand& weapon_band = state->score_band;

    // Line of fire: a low wall with a clear view over it when standing.
    Fvector direction = Fvector().sub(threat, cover.position());
    float yaw, pitch;
    direction.getHP(yaw, pitch);
    yaw = angle_normalize(yaw);
    const CLevelGraph& graph = ai().level_graph();
    if (graph.low_cover_in_direction(yaw, cover.level_vertex_id()) <= c.lof_low_max &&
        graph.high_cover_in_direction(yaw, cover.level_vertex_id()) > c.cover_open_value)
        value -= c.score_line_of_fire;

    // Soft band preference: within the effective range, never inside the minimum.
    if (weapon_band.engage_max > 0.f)
    {
        const float beyond = (threat_distance - weapon_band.effective) / _max(weapon_band.effective, 1.f);
        if (beyond > 0.f)
            value += c.score_band * _min(1.f, beyond);
        if (weapon_band.min > 0.f && threat_distance < weapon_band.min)
            value += c.score_band * (weapon_band.min - threat_distance) / weapon_band.min;
    }

    // Aggressive profiles prefer covers closer to the belief, cautious ones farther from it (lower is better: a cover
    // closer than the NPC stands gains for aggression above 0.5).
    value += c.score_aggr * (2.f * state->score_aggr - 1.f) *
        clampr((threat_distance - current_distance) / c.score_aggr_range_m, -1.f, 1.f);
    value += start.distance_to(cover.position()) * c.score_travel;
    // Judged from the cover itself (the held cover's value): no way to walk, no open ground on it.
    if (value >= bound || start.similar(cover.position(), EPS_L))
        return true;

    // Path exposure; short-range weapons refuse a cover behind too much open ground (toward the enemy: their gap).
    float limit;
    const bool limited =
        open_limit(npc, threat_distance + c.at_cover_m < current_distance, limit, state->score_exposed);
    const float exposure = path_exposure(npc, cover.position(), threat, limited ? limit : c.approach_radius_m);
    if (limited && exposure > limit)
        return false;
    value += exposure * c.score_exposure * (.5f + state->score_cover);
    return true;
}

bool open_ground_refused(const CAI_Stalker& npc, const Fvector& to, const Fvector& threat)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::cover_score))
        return false;

    ScopedTimer timer(Stat::p_tactics_us, npc.da_ai()->community);
    const bool approaching = to.distance_to(threat) + config().at_cover_m < npc.Position().distance_to(threat);
    float limit;
    return open_limit(npc, approaching, limit, !approaching && exposed(npc, threat)) &&
        path_exposure(npc, to, threat, limit) > limit;
}

u8 side_of(const Fvector& threat, const Fvector& anchor, const Fvector& point)
{
    const Fvector axis = Fvector().sub(anchor, threat);
    const Fvector offset = Fvector().sub(point, threat);
    return axis.z * offset.x - axis.x * offset.z > 0.f ? 1 : 0;
}

const CCoverPoint* find_approach_cover(CAI_Stalker& npc, const Fvector& threat, const Doctrine& doctrine, float floor)
{
    CDaAiApproachEvaluator evaluator(npc);
    evaluator.setup(threat, doctrine.distance, doctrine.band, Morale::cover(npc), floor);
    return ai().cover_manager().best_cover(
        npc.Position(), config().approach_radius_m, evaluator, CStalkerMovementRestrictor(&npc, true, false));
}

bool find_fall_back(CAI_Stalker& npc, const Fvector& threat, bool fanatic, bool away, Fvector& position, u32& vertex,
    bool& cover)
{
    const Config& c = config();
    CDaAiFallBackEvaluator evaluator(npc);
    evaluator.setup(threat, npc.Position().distance_to(threat), fanatic, Morale::cover(npc));
    const CCoverPoint* point = ai().cover_manager().best_cover(
        npc.Position(), c.fallback_radius_m, evaluator, CStalkerMovementRestrictor(&npc, true, true));
    cover = !!point;
    if (point)
    {
        position = point->position();
        vertex = point->level_vertex_id();
        return true;
    }
    return away && !fanatic && point_away(npc, threat, c.fallback_radius_m, position, vertex);
}

bool gather_flank_leg(CAI_Stalker& npc, const Fvector& threat, u8 side, const WeaponBand& weapon_band,
    const CCoverPoint* from, TacticsState::FlankSearch::Leg& leg, StepBudget& budget)
{
    if (!leg.tiles.active)
        begin_gather(leg, from ? from->position() : npc.Position(), npc.Position(), weapon_band);
    CDaAiFlankEvaluator evaluator(npc);
    evaluator.setup(threat, leg.anchor, side, leg.band);
    return gather_route(leg, config().flank_radius_m, evaluator, budget);
}

bool judge_flank_leg(CAI_Stalker& npc, const Fvector& threat, float limit, const CCoverPoint* from,
    TacticsState::FlankSearch::Leg& leg, u32 walks, u32 checks, StepBudget& budget)
{
    const Config& c = config();
    const float cover = Morale::cover(npc);
    const CStalkerMovementRestrictor restrictor(&npc, true, false);
    for (; leg.next < leg.count && leg.judged < c.flank_checks; ++leg.next)
    {
        const u8 index = leg.next;
        // Best first by geometry, and the danger weight only raises a value: once a candidate's own value reaches the
        // best total, none of the rest can win.
        if (leg.value[index] >= leg.best_value)
            break;
        if (!walks || !checks || !budget.next())
            return false;
        --checks;
        const CCoverPoint& point = *leg.cover[index];
        const float weight = agent_weight(npc, restrictor, point);
        if (fis_zero(weight))
            continue;
        ++leg.judged;
        const float scale = 1.f / weight;
        const float value = leg.value[index] * scale;
        if (value >= leg.best_value)
            continue;
        --walks;
        const float exposure = from ?
            path_exposure(from->level_vertex_id(), from->position(), point.position(), threat, limit) :
            path_exposure(npc, point.position(), threat, limit);
        if (exposure > limit)
            continue;
        const float total = value + exposure * c.score_exposure * (.5f + cover) * scale;
        if (total >= leg.best_value)
            continue;
        leg.best_value = total;
        leg.best = &point;
        leg.best_open = exposure;
    }
    return true;
}

bool gather_flank_vias(CAI_Stalker& npc, const Fvector& threat, u8 side, const WeaponBand& weapon_band,
    TacticsState::FlankSearch::Leg& leg, StepBudget& budget)
{
    if (!leg.tiles.active)
        begin_gather(leg, npc.Position(), npc.Position(), weapon_band);
    CDaAiFlankViaEvaluator evaluator(npc);
    evaluator.setup(threat, leg.anchor, side, leg.band);
    return gather_route(leg, config().flank_via_radius_m, evaluator, budget);
}

bool judge_flank_vias(CAI_Stalker& npc, const Fvector& threat, float limit, TacticsState::FlankSearch::Leg& leg,
    u32 walks, u32 checks, StepBudget& budget, const CCoverPoint** covers, float* open, u8 capacity)
{
    capacity = _min(capacity, TacticsState::FlankSearch::via_max);
    const CStalkerMovementRestrictor restrictor(&npc, true, false);
    for (; leg.next < leg.count && leg.found < capacity && leg.judged < config().flank_checks; ++leg.next)
    {
        if (!walks || !checks || !budget.next())
            return false;
        --checks;
        const CCoverPoint* cover = leg.cover[leg.next];
        if (fis_zero(agent_weight(npc, restrictor, *cover)))
            continue;
        ++leg.judged;
        --walks;
        const float exposure = path_exposure(npc, cover->position(), threat, limit);
        if (exposure > limit)
            continue;
        covers[leg.found] = cover;
        open[leg.found] = exposure;
        ++leg.found;
    }
    return true;
}

void start_cover_tiles(CoverTiles& tiles, const Fvector& centre, float radius, float inner, float tile_m)
{
    tiles = {};
    tiles.active = true;
    tiles.centre = centre;
    tiles.radius = radius;
    tiles.inner = inner;
    if (radius <= tile_m)
    {
        // One query of the disk itself.
        tiles.per_row = 1;
        tiles.side = 2.f * radius + 1.f;
        tiles.query = radius;
    }
    else
    {
        // Squares inscribed in circles of tile_m; one more square per row than the diameter needs, so the grid holds
        // the whole disk inside its half-open squares.
        tiles.side = tile_m * _sqrt(2.f);
        tiles.per_row = u16(iFloor(2.f * radius / tiles.side) + 1);
        tiles.query = tile_m + .01f;
    }
    const float half = .5f * tiles.side * float(tiles.per_row);
    tiles.corner.set(centre.x - half, centre.y, centre.z - half);
}

bool next_cover_tile(CoverTiles& tiles, xr_vector<CCoverPoint*>& covers)
{
    covers.clear();
    const u32 count = u32(tiles.per_row) * tiles.per_row;
    while (tiles.next < count)
    {
        const u32 index = tiles.next++;
        const float x0 = tiles.corner.x + tiles.side * float(index % tiles.per_row);
        const float z0 = tiles.corner.z + tiles.side * float(index / tiles.per_row);
        const float x1 = x0 + tiles.side;
        const float z1 = z0 + tiles.side;
        // A square outside the disk or inside the ring's hole holds nothing wanted.
        const float near_x = _max(_max(x0 - tiles.centre.x, tiles.centre.x - x1), 0.f);
        const float near_z = _max(_max(z0 - tiles.centre.z, tiles.centre.z - z1), 0.f);
        const float far_x = _max(_abs(x0 - tiles.centre.x), _abs(x1 - tiles.centre.x));
        const float far_z = _max(_abs(z0 - tiles.centre.z), _abs(z1 - tiles.centre.z));
        if (_sqr(near_x) + _sqr(near_z) > _sqr(tiles.radius) || _sqr(far_x) + _sqr(far_z) < _sqr(tiles.inner))
            continue;
        const Fvector middle = Fvector().set(.5f * (x0 + x1), tiles.centre.y, .5f * (z0 + z1));
        ai().cover_manager().covers().nearest(tiles.per_row == 1 ? tiles.centre : middle, tiles.query, covers);
        const auto outside = [&](const CCoverPoint* cover) {
            const Fvector& at = cover->position();
            return cover->m_is_smart_cover || at.x < x0 || at.x >= x1 || at.z < z0 || at.z >= z1;
        };
        covers.erase(std::remove_if(covers.begin(), covers.end(), outside), covers.end());
        return true;
    }
    tiles.active = false;
    return false;
}

const CCoverPoint* find_step_cover(CAI_Stalker& npc, const Fvector& goal, const Fvector& threat, float limit)
{
    CDaAiStepEvaluator evaluator(npc);
    evaluator.setup(goal, threat, limit);
    return ai().cover_manager().best_cover(
        npc.Position(), config().approach_radius_m, evaluator, CStalkerMovementRestrictor(&npc, true, false));
}
} // namespace DaAi::Tactics
