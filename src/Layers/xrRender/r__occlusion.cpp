#include "stdafx.h"
#include "r__occlusion.h"

#include "QueryHelper.h"

namespace xray::render::RENDER_NAMESPACE
{
R_occlusion::~R_occlusion(void) { occq_destroy(); }
void R_occlusion::occq_create(u32 limit)
{
    ZoneScoped;
    enabled = strstr(Core.Params, "-no_occq") ? false : true;
    handleGeneration = handleGeneration % (handleGenerationMask - 1) + 1;
    abandonedPollFrame = iInvalidHandle;
    pool.reserve(limit);
    used.reserve(limit);
    fids.reserve(limit);
    abandoned.reserve(limit);
    for (u32 it = 0; it < limit; it++)
    {
        Query q;
        q.order = it;
        if (FAILED(CreateQuery(&q.Q, D3D_QUERY_OCCLUSION)))
            break;
        pool.emplace_back(std::move(q));
    }
    std::reverse(pool.begin(), pool.end());
}
void R_occlusion::occq_destroy()
{
    for (const auto& q : used)
        ReleaseQuery(q.Q);
    for (const auto& q : pool)
        ReleaseQuery(q.Q);
    used.clear();
    pool.clear();
    fids.clear();
    abandoned.clear();
    abandonedPollFrame = iInvalidHandle;
}

u32 R_occlusion::make_handle(u32 slot) const
{
    VERIFY(slot <= handleIndexMask);
    return (handleGeneration << handleIndexBits) | slot;
}

bool R_occlusion::resolve_handle(u32 handle, u32& slot) const
{
    if (handle == iInvalidHandle || (handle >> handleIndexBits) != handleGeneration)
        return false;

    slot = handle & handleIndexMask;
    return slot < used.size() && used[slot].Q;
}

void R_occlusion::recycle_slot(u32 slot)
{
    Query& query = used[slot];
    VERIFY(query.Q);
    if (pool.empty())
        pool.emplace_back(query);
    else
    {
        int index = int(pool.size()) - 1;
        while (index >= 0 && pool[index].order < query.order)
            --index;
        pool.emplace(pool.begin() + index + 1, std::move(query));
    }

    used[slot].Q = 0;
    fids.emplace_back(slot);
}

bool R_occlusion::try_get_slot(u32 slot, occq_result& fragments, bool updateStats, bool doNotFlush)
{
    const HRESULT result = GetData(used[slot].Q, &fragments, sizeof(fragments),
        doNotFlush ? QueryGetDataDoNotFlush : 0);
    if (result == S_FALSE)
        return false;
    if (FAILED(result))
        fragments = occq_result(-1);

    if (updateStats && !fragments)
        RImplementation.BasicStats.OcclusionCulled++;
    recycle_slot(slot);
    return true;
}

void R_occlusion::poll_abandoned()
{
    if (abandonedPollFrame == Device.dwFrame)
        return;
    abandonedPollFrame = Device.dwFrame;

    size_t writeIndex{};
    for (u32 slot : abandoned)
    {
        occq_result fragments{};
        if (slot < used.size() && used[slot].Q && !try_get_slot(slot, fragments, false))
            abandoned[writeIndex++] = slot;
    }
    abandoned.resize(writeIndex);
}

u32 R_occlusion::occq_begin(u32& ID)
{
    if (!enabled)
    {
        ID = iInvalidHandle;
        return 0;
    }

    ScopeLock lock{ &render_lock };
    poll_abandoned();

    if (pool.empty())
    {
        const auto sz = used.size();
        Query q;
        q.order = static_cast<u32>(sz);
        if (FAILED(CreateQuery(&q.Q, D3D_QUERY_OCCLUSION)))
        {
            if ((Device.dwFrame % 40) == 0)
                Msg(" RENDER [Warning]: Too many occlusion queries were issued (>%zu)!!!", sz);
            ID = iInvalidHandle;
            return 0;
        }
        if (sz == used.capacity())
        {
            used.reserve(sz + occq_size_base);
            pool.reserve(sz + occq_size_base);
        }
        pool.emplace(pool.begin(), std::move(q));
    }

    RImplementation.BasicStats.OcclusionQueries++;
    u32 slot{};
    if (!fids.empty())
    {
        slot = fids.back();
        fids.pop_back();
        used[slot] = std::move(pool.back());
    }
    else
    {
        slot = static_cast<u32>(used.size());
        used.emplace_back(std::move(pool.back()));
    }
    ID = make_handle(slot);
    pool.pop_back();
    VERIFY(slot < used.size() && used[slot].Q);
    CHK_DX(BeginQuery(used[slot].Q));

    return used[slot].order;
}
void R_occlusion::occq_end(u32& ID)
{
    if (!enabled || ID == iInvalidHandle)
        return;

    ScopeLock lock{ &render_lock };

    u32 slot{};
    if (resolve_handle(ID, slot))
        CHK_DX(EndQuery(used[slot].Q));
}

bool R_occlusion::occq_try_get(u32& ID, occq_result& fragments)
{
    if (!enabled || ID == iInvalidHandle)
    {
        fragments = occq_result(-1);
        ID = iInvalidHandle;
        return true;
    }

    ScopeLock lock{ &render_lock };
    u32 slot{};
    if (!resolve_handle(ID, slot))
    {
        fragments = occq_result(-1);
        ID = iInvalidHandle;
        return true;
    }

    if (!try_get_slot(slot, fragments, true))
        return false;
    ID = iInvalidHandle;
    return true;
}

R_occlusion::occq_result R_occlusion::occq_get(u32& ID)
{
    occq_result fragments = occq_result(-1);
    if (!enabled || ID == iInvalidHandle)
    {
        ID = iInvalidHandle;
        return fragments;
    }

    CTimer T;
    T.Start();
    RImplementation.BasicStats.Wait.Begin();
    while (true)
    {
        bool ready{};
        {
            ScopeLock lock{ &render_lock };
            u32 slot{};
            if (!resolve_handle(ID, slot))
            {
                ID = iInvalidHandle;
                ready = true;
            }
            else if (try_get_slot(slot, fragments, true, false))
            {
                ID = iInvalidHandle;
                ready = true;
            }
        }
        if (ready)
            break;

        if (!SwitchToThread())
            Sleep(ps_r2_wait_sleep);

        if (T.GetElapsed_ms() > 500)
        {
            occq_cancel(ID);
            fragments = occq_result(-1);
            break;
        }
    }
    RImplementation.BasicStats.Wait.End();
    return fragments;
}

void R_occlusion::occq_cancel(u32& ID)
{
    if (!enabled || ID == iInvalidHandle)
    {
        ID = iInvalidHandle;
        return;
    }

    ScopeLock lock{ &render_lock };
    u32 slot{};
    if (resolve_handle(ID, slot))
    {
        occq_result fragments{};
        if (!try_get_slot(slot, fragments, false))
            abandoned.emplace_back(slot);
    }
    ID = iInvalidHandle;
}
} // namespace xray::render::RENDER_NAMESPACE
