#pragma once

#include <array>
#include <atomic>
#include <cstdio>

#include "xr_types.h"
#include "xrMemory.h"

#include <cstring>

#pragma pack(push, 4)
#pragma warning(push)
#pragma warning(disable : 4200)
struct XRCORE_API str_value
{
    // Shared string nodes are copied and released from worker threads (parallel loaders, task
    // manager), so the counter must be atomic: a single lost increment makes clean() free a node
    // that is still referenced, and the stale text memory is then recycled into garbage.
    std::atomic<u32> dwReference;
    u32 dwLength;
    u32 dwCRC;
    str_value* next;
    char value[];
};
static_assert(sizeof(std::atomic<u32>) == sizeof(u32) && std::atomic<u32>::is_always_lock_free,
    "str_value relies on a lock-free 4-byte atomic reference counter");

// Diagnostics for the shared string pool, enabled with the -str_debug command line key.
XRCORE_API extern bool g_shared_str_debug;
// Called when a reference release observes a counter that is already zero (double release or
// a stale shared_str). The node is intentionally left alive; the report identifies the culprit.
XRCORE_API void shared_str_report_reference_underflow(const str_value* node);

struct XRCORE_API str_value_cmp
{
    // less
    IC bool operator()(const str_value* A, const str_value* B) const { return A->dwCRC < B->dwCRC; };
};

#pragma warning(pop)

struct str_container_impl;
class IWriter;
//////////////////////////////////////////////////////////////////////////
class XRCORE_API str_container
{
public:
    str_container();
    ~str_container();

    str_value* dock(pcstr value) const;
    void clean() const;
    void dump() const;
    void dump(IWriter* W) const;
    void verify() const;

    [[nodiscard]]
    std::pair<size_t, size_t> stat_economy() const;

private:
    str_container_impl* impl;
};
XRCORE_API extern str_container* g_pStringContainer;

//////////////////////////////////////////////////////////////////////////
class shared_str
{
    str_value* p_{};

protected:
    // ref-counting
    void _dec() noexcept
    {
        if (!p_)
            return;
        // fetch_sub returns the pre-decrement count: 1 means this was the final reference.
        const u32 previous = p_->dwReference.fetch_sub(1, std::memory_order_acq_rel);
        if (previous <= 1)
        {
            if (!previous)
                shared_str_report_reference_underflow(p_);
            p_ = nullptr;
        }
    }

public:
    void _set(pcstr rhs)
    {
        // dock() returns an already-acquired reference taken under the container lock, closing
        // the window where clean() could sweep a zero-reference node before we adopted it.
        str_value* v = g_pStringContainer->dock(rhs);
        _dec();
        p_ = v;
    }
    void _set(shared_str const& rhs) noexcept
    {
        str_value* v = rhs.p_;
        if (v)
            v->dwReference.fetch_add(1, std::memory_order_relaxed);
        _dec();
        p_ = v;
    }
    void _set(std::nullptr_t) noexcept
    {
        _dec();
        p_ = nullptr;
    }

    [[nodiscard]]
    const str_value* _get() const { return p_; }

public:
    // construction
    shared_str() = default;
    shared_str(pcstr rhs)
    {
        p_ = nullptr;
        _set(rhs);
    }
    shared_str(shared_str const& rhs) noexcept
    {
        p_ = nullptr;
        _set(rhs);
    }
    shared_str(shared_str&& rhs) noexcept
        : p_(rhs.p_)
    {
        rhs.p_ = nullptr;
    }
    ~shared_str() { _dec(); }
    // assignment & accessors
    shared_str& operator=(pcstr rhs)
    {
        _set(rhs);
        return *this;
    }
    shared_str& operator=(shared_str const& rhs) noexcept
    {
        _set(rhs);
        return *this;
    }
    shared_str& operator=(shared_str&& rhs) noexcept
    {
        p_ = rhs.p_;
        rhs.p_ = nullptr;
        return *this;
    }
    shared_str& operator=(std::nullptr_t) noexcept
    {
        _set(nullptr);
        return *this;
    }

    [[nodiscard]]
    bool operator!() const { return p_ == nullptr; }
    [[nodiscard]]
    explicit operator bool() const { return p_ != nullptr; }
    [[nodiscard]]
    char operator[](size_t id) { return p_->value[id]; }
    [[nodiscard]]
    char operator[](size_t id) const { return p_->value[id]; }

    [[nodiscard]]
    pcstr c_str() const { return p_ ? p_->value : nullptr; }

    // misc func
    [[nodiscard]]
    size_t size() const
    {
        if (nullptr == p_)
            return 0;

        return p_->dwLength;
    }

    [[nodiscard]]
    bool empty() const
    {
        return size() == 0;
    }

    void swap(shared_str& rhs) noexcept
    {
        str_value* tmp = p_;
        p_ = rhs.p_;
        rhs.p_ = tmp;
    }

    [[nodiscard]]
    bool equal(const shared_str& rhs) const { return (p_ == rhs.p_); }
};

inline int __cdecl xr_sprintf(shared_str& destination, pcstr format_string, ...)
{
    string4096 buf;
    va_list args;
    va_start(args, format_string);
    const int vs_sz = vsnprintf(buf, sizeof(buf) - 1, format_string, args);
    buf[sizeof(buf) - 1] = 0;
    va_end(args);
    if (vs_sz >= 0)
        destination = buf;
    return vs_sz;
}

template<>
struct std::hash<shared_str>
{
    [[nodiscard]] size_t operator()(const shared_str& str) const noexcept
    {
        return str ? str._get()->dwCRC : std::hash<pcstr>{}(nullptr);
    }
};

bool operator==(const shared_str&, std::nullptr_t) = delete;
bool operator!=(const shared_str&, std::nullptr_t) = delete;

bool operator==(std::nullptr_t, const shared_str&) = delete;
bool operator!=(std::nullptr_t, const shared_str&) = delete;

// res_ptr == res_ptr
// res_ptr != res_ptr
// const res_ptr == ptr
// const res_ptr != ptr
// ptr == const res_ptr
// ptr != const res_ptr
// res_ptr < res_ptr
// res_ptr > res_ptr
IC bool operator==(shared_str const& a, shared_str const& b) { return a._get() == b._get(); }
IC bool operator!=(shared_str const& a, shared_str const& b) { return a._get() != b._get(); }
IC bool operator<(shared_str const& a, shared_str const& b) { return a._get() < b._get(); }
IC bool operator>(shared_str const& a, shared_str const& b) { return a._get() > b._get(); }
// externally visible standard functionality
IC void swap(shared_str& lhs, shared_str& rhs) noexcept { lhs.swap(rhs); }
IC size_t xr_strlen(const shared_str& a) noexcept { return a.size(); }

ICF int xr_strcmp(const char* S1, const char* S2)
{
    return strcmp(S1, S2);
}

IC int xr_strcmp(const shared_str& a, const char* b) noexcept { return xr_strcmp(a.c_str(), b); }
IC int xr_strcmp(const char* a, const shared_str& b) noexcept { return xr_strcmp(a, b.c_str()); }
IC int xr_strcmp(const shared_str& a, const shared_str& b) noexcept
{
    if (a.equal(b))
        return 0;
    else
        return xr_strcmp(a.c_str(), b.c_str());
}

IC char* xr_strlwr(char* src)
{
    static constexpr std::array<unsigned char, 256> lowerTable = []
    {
        std::array<unsigned char, 256> table{};
        for (size_t i = 0; i < table.size(); ++i)
            table[i] = i >= 'A' && i <= 'Z' ? static_cast<unsigned char>(i + ('a' - 'A')) : static_cast<unsigned char>(i);
        return table;
    }();

    for (char* current = src; *current; ++current)
        *current = static_cast<char>(lowerTable[static_cast<unsigned char>(*current)]);

    return src;
}

IC void xr_strlwr(shared_str& src)
{
    if (src.c_str())
    {
        char* lp = xr_strdup(src.c_str());
        xr_strlwr(lp);
        src = lp;
        xr_free(lp);
    }
}

#pragma pack(pop)
////
