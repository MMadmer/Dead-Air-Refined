// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The lamp shadow cache (da_smap_cache.h). Every shadowed lamp face used to walk the level and draw every static
// caster in its frustum each frame - an indoor hall's chandelier alone cost the game thread a quarter of the frame.
// A face whose transform and size have held keeps its statics' depth in a 4096 atlas; the frame copies that into the
// light's rect of the live map and draws what moves on top. A face costs the game thread per draw, whatever its size, so
// the small faces take the slots first. The copy is texel for texel: the slot is drawn at the
// face's own size with the same bias, so the shadow cannot differ from the stock one.

#include "stdafx.h"

#include "Layers/xrRenderPC_R4/da_smap_cache.h"
#include "Layers/xrRender/light.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Two 4096 pages side by side: the user's lamp test hall has a hundred faces, 26 Mtexel of slots.
constexpr u32 kAtlas = 4096;
constexpr u32 kPages = 2;
constexpr u32 kMinSlot = 32;
constexpr u32 kLevels = 8; // 4096 .. 32

struct Slot
{
    light* owner = nullptr;
    u16 x = 0, y = 0;
    u16 size = 0; // a power of two
    u32 used = 0; // the frame it was last copied
};

xr_vector<Slot> g_slots;
xr_vector<s32> g_free_slots;
// The buddy allocator's free squares per level (level 0 = the whole atlas).
xr_vector<std::pair<u16, u16>> g_free[kLevels];
bool g_started = false;

u32 level_of(u32 size)
{
    u32 level = 0;
    for (u32 s = kAtlas; s > size && level + 1 < kLevels; s >>= 1)
        ++level;
    return level;
}

void reset_atlas()
{
    for (Slot& slot : g_slots)
        if (slot.owner)
            slot.owner->smap_cache.slot = -1;
    g_slots.clear();
    g_free_slots.clear();
    for (auto& list : g_free)
        list.clear();
    for (u32 page = 0; page < kPages; ++page)
        g_free[0].push_back({ u16(page * kAtlas), 0 });
    g_started = true;
}

bool take_square(u32 level, std::pair<u16, u16>& out)
{
    if (!g_free[level].empty())
    {
        out = g_free[level].back();
        g_free[level].pop_back();
        return true;
    }
    if (level == 0)
        return false;
    std::pair<u16, u16> parent;
    if (!take_square(level - 1, parent))
        return false;
    const u16 half = u16(kAtlas >> level);
    g_free[level].push_back({ u16(parent.first + half), parent.second });
    g_free[level].push_back({ parent.first, u16(parent.second + half) });
    g_free[level].push_back({ u16(parent.first + half), u16(parent.second + half) });
    out = parent;
    return true;
}

void give_square(u32 level, std::pair<u16, u16> square)
{
    // Merge with the three buddies when they are all free.
    while (level > 0)
    {
        const u16 size = u16(kAtlas >> level);
        const u16 px = u16(square.first & ~(2 * size - 1)), py = u16(square.second & ~(2 * size - 1));
        auto& list = g_free[level];
        u32 found = 0;
        for (const auto& s : list)
            if ((s.first & ~(2 * size - 1)) == px && (s.second & ~(2 * size - 1)) == py)
                ++found;
        if (found != 3)
            break;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::pair<u16, u16>& s) {
            return (s.first & ~(2 * size - 1)) == px && (s.second & ~(2 * size - 1)) == py;
        }), list.end());
        square = { px, py };
        --level;
    }
    g_free[level].push_back(square);
}

void free_slot(s32 index)
{
    Slot& slot = g_slots[index];
    if (slot.owner)
        slot.owner->smap_cache.slot = -1;
    give_square(level_of(slot.size), { slot.x, slot.y });
    slot = {};
    g_free_slots.push_back(index);
}

// A slot for the light, the least recently copied ones not of this frame given up while the atlas is full.
s32 alloc_slot(light* L, u32 face)
{
    u32 size = kMinSlot;
    while (size < face)
        size <<= 1;
    const u32 level = level_of(size);
    std::pair<u16, u16> square;
    while (!take_square(level, square))
    {
        s32 oldest = -1;
        for (s32 i = 0; i < s32(g_slots.size()); ++i)
        {
            const Slot& s = g_slots[i];
            if (s.owner && s.used != Device.dwFrame && (oldest < 0 || s.used < g_slots[oldest].used))
                oldest = i;
        }
        if (oldest < 0)
            return -1;
        free_slot(oldest);
    }
    s32 index;
    if (!g_free_slots.empty())
    {
        index = g_free_slots.back();
        g_free_slots.pop_back();
    }
    else
    {
        index = s32(g_slots.size());
        g_slots.emplace_back();
    }
    g_slots[index] = { L, square.first, square.second, u16(size), Device.dwFrame };
    return index;
}

bool same(const Fmatrix& a, const Fmatrix& b)
{
    const float* pa = &a._11;
    const float* pb = &b._11;
    for (u32 i = 0; i < 16; ++i)
        if (_abs(pa[i] - pb[i]) > 1e-5f * (1.f + _abs(pa[i])))
            return false;
    return true;
}

void draw_quad(CBackend& cmd_list, float x, float y, float size, float clear)
{
    RImplementation.Target->phase_smap_cache_quad(cmd_list, x, y, size, clear);
}
} // namespace

void CRenderTarget::phase_smap_cache_quad(CBackend& cmd_list, float x, float y, float size, float clear)
{
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Element(s_smap_cache->E[0]);
    cmd_list.set_c("da_smap_blit", x, y, size, clear);
    cmd_list.set_Geometry(g_smap_cache_quad);
    cmd_list.Render(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    cmd_list.set_CullMode(CULL_CCW);
}

u32 da_smap_cache_size(u32 raw, u32 previous)
{
    static constexpr u32 ladder[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536 };
    if (!ps_r__light_shadow_cache)
        return raw;
    // Kept while the wanted size stays within 30 % of it: the steps are 50 % apart, so the bands overlap.
    for (const u32 step : ladder)
        if (step == previous && raw * 10 >= step * 7 && raw * 10 <= step * 13)
            return step;
    u32 best = ladder[0];
    float best_d = flt_max;
    for (const u32 step : ladder)
    {
        const float d = _abs(logf(float(raw) / float(step)));
        if (d < best_d)
            best_d = d, best = step;
    }
    return best;
}

// QA (r__light_shadow_cache 2): the faces of each mode, logged every 60 frames.
static u32 g_counts[3]{}, g_counted_frame = 0, g_moving = 0, g_full = 0, g_texels = 0;
static void count(DaSmapCache mode)
{
    if (ps_r__light_shadow_cache < 2)
        return;
    if (g_counted_frame != Device.dwFrame)
    {
        if (g_counted_frame % 60 == 0 && g_counted_frame)
            Msg("* [smap cache] frame %u: stock %u (moving %u, no room %u), fill %u, use %u, %u Mtexel of faces",
                g_counted_frame, g_counts[0], g_moving, g_full, g_counts[1], g_counts[2], g_texels >> 20);
        g_counts[0] = g_counts[1] = g_counts[2] = g_moving = g_full = g_texels = 0;
        g_counted_frame = Device.dwFrame;
    }
    ++g_counts[u32(mode)];
}

static DaSmapCache decide(light* L);

DaSmapCache da_smap_cache_mode(light* L)
{
    const DaSmapCache mode = decide(L);
    count(mode);
    return mode;
}

static DaSmapCache decide(light* L)
{
    light::SmapCache& c = L->smap_cache;
    c.mode = u8(DaSmapCache::off);
    // How long the face has held still: a swinging lamp or the torch never settles and keeps the stock face.
    c.still = same(c.last, L->X.S.combine) ? c.still + 1 : 0;
    c.last = L->X.S.combine;
    if (!ps_r__light_shadow_cache || !RImplementation.Target->s_smap_cache || !RImplementation.o.HW_smap)
        return DaSmapCache::off;
    if (!g_started)
        reset_atlas();

    if (c.slot >= 0 && g_slots[c.slot].owner == L && c.size == L->X.S.size && same(c.combine, L->X.S.combine))
    {
        g_slots[c.slot].used = Device.dwFrame;
        c.mode = u8(DaSmapCache::use);
        return DaSmapCache::use;
    }
    g_texels += L->X.S.size * L->X.S.size;
    if (c.still < 2)
    {
        ++g_moving;
        return DaSmapCache::off;
    }
    if (c.slot >= 0 && g_slots[c.slot].owner == L && g_slots[c.slot].size < L->X.S.size)
        free_slot(c.slot);
    if (c.slot < 0 || g_slots[c.slot].owner != L)
        c.slot = alloc_slot(L, L->X.S.size);
    if (c.slot < 0)
    {
        ++g_full;
        return DaSmapCache::off;
    }
    c.size = L->X.S.size;
    c.combine = L->X.S.combine;
    g_slots[c.slot].used = Device.dwFrame;
    c.mode = u8(DaSmapCache::fill);
    return DaSmapCache::fill;
}

void da_smap_cache_fill_begin(CBackend& cmd_list, light* L)
{
    auto& target = *RImplementation.Target;
    const Slot& slot = g_slots[L->smap_cache.slot];
    cmd_list.set_pass_targets(target.rt_smap_surf, nullptr, nullptr, target.rt_smap_cache);
    cmd_list.SetViewport({ float(slot.x), float(slot.y), float(L->X.S.size), float(L->X.S.size), 0.f, 1.f });
    cmd_list.set_Stencil(FALSE);
    if (RImplementation.o.HW_smap)
        cmd_list.set_ColorWriteEnable(FALSE);
    draw_quad(cmd_list, 0.f, 0.f, float(L->X.S.size), 1.f);
}

void da_smap_cache_copy(CBackend& cmd_list, light* L)
{
    const Slot& slot = g_slots[L->smap_cache.slot];
    draw_quad(cmd_list, float(slot.x), float(slot.y), float(L->X.S.size), 0.f);
}

void da_smap_cache_release(light* L)
{
    const s32 index = L->smap_cache.slot;
    if (index >= 0 && index < s32(g_slots.size()) && g_slots[index].owner == L)
        free_slot(index);
    L->smap_cache.slot = -1;
}

void da_smap_cache_reset()
{
    if (g_started)
        reset_atlas();
}
} // namespace xray::render::RENDER_NAMESPACE
