#pragma once

namespace xray::render::RENDER_NAMESPACE
{
constexpr u32 occq_size_base = 768; // // queue for occlusion queries
constexpr u32 occq_size = 2 * occq_size_base * R__NUM_PARALLEL_CONTEXTS; // // queue for occlusion queries

// must conform to following order of allocation/free
// a(A), a(B), a(C), a(D), ....
// f(A), f(B), f(C), f(D), ....
// a(A), a(B), a(C), a(D), ....
//	this mean:
//		use as litle of queries as possible
//		first try to use queries allocated first
//	assumption:
//		used queries number is much smaller than total count

class R_occlusion
{
public:
#if defined(USE_DX11)
    using occq_result = u64;
#elif defined(USE_OGL)
    using occq_result = u32;
#else
#   error No graphics API selected or enabled!
#endif

private:
    struct Query
    {
#if defined(USE_DX11)
        ID3DQuery* Q{};
#elif defined(USE_OGL)
        GLuint Q{};
#else
#   error No graphics API selected or enabled!
#endif
        u32 order{};
    };

    static constexpr u32 iInvalidHandle = 0xFFFFFFFF;
    static constexpr u32 handleIndexBits = 20;
    static constexpr u32 handleIndexMask = (1u << handleIndexBits) - 1;
    static constexpr u32 handleGenerationMask = (1u << (32 - handleIndexBits)) - 1;

    bool enabled{ true };
    xr_vector<Query> pool; // sorted (max ... min), insertions are usually at the end
    xr_vector<Query> used; // id's are generated from this and it is cleared from back only
    xr_vector<u32> fids; // free id's
    xr_vector<u32> abandoned; // slots whose owners disappeared before the GPU result was ready
    u32 handleGeneration{};
    u32 abandonedPollFrame{ iInvalidHandle };

    Lock render_lock{};

    u32 make_handle(u32 slot) const;
    bool resolve_handle(u32 handle, u32& slot) const;
    bool try_get_slot(u32 slot, occq_result& fragments, bool updateStats, bool doNotFlush = true);
    void recycle_slot(u32 slot);
    void poll_abandoned();

public:
    ~R_occlusion();

    void occq_create(u32 limit);
    void occq_destroy();
    u32 occq_begin(u32& ID); // returns 'order'
    void occq_end(u32& ID);
    bool occq_try_get(u32& ID, occq_result& fragments);
    occq_result occq_get(u32& ID);
    void occq_cancel(u32& ID);
};
} // namespace xray::render::RENDER_NAMESPACE
