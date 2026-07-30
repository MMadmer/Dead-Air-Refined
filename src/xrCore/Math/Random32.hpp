////////////////////////////////////////////////////////////////////////////
//	Module 		: random32.cpp
//	Created 	: 09.03.2004
//  Modified 	: 09.03.2004
//	Author		: Dmitriy Iassenev
//	Description : 32-bit peudo random number generator
////////////////////////////////////////////////////////////////////////////

#pragma once
#include "xrCore/xrCore.h"

#include <atomic>

class CRandom32
{
private:
    u32 m_seed;

public:
    inline u32 seed() const { return std::atomic_ref<const u32>(m_seed).load(std::memory_order_relaxed); }
    inline void seed(u32 seed) { std::atomic_ref(m_seed).store(seed, std::memory_order_relaxed); }
    inline u32 random(u32 range)
    {
        std::atomic_ref state(m_seed);
        u32 current = state.load(std::memory_order_relaxed);
        u32 next;
        do
        {
            next = 0x08088405u * current + 1u;
        }
        while (!state.compare_exchange_weak(current, next, std::memory_order_relaxed));
        return u32((u64(next) * u64(range)) >> 32u);
    }
};
