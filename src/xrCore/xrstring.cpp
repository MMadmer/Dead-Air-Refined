#include "stdafx.h"
#pragma hdrstop // Huh?

#include "xrstring.h"
#include "Threading/Lock.hpp"
#include "xrCore/_std_extensions.h"

#include "FS_impl.h"
#include <SDL.h>

#include <new>

XRCORE_API str_container* g_pStringContainer = nullptr;
XRCORE_API bool g_shared_str_debug = false;

void shared_str_report_reference_underflow(const str_value* node)
{
    // Keep the pool usable: log the first few offenders instead of tearing the process down.
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 16)
        Msg("! shared_str: reference underflow on node[%p]", static_cast<const void*>(node));
}

#if 1

struct str_container_impl
{
    Lock cs;
    static constexpr size_t buffer_size = 1024u * 256u;
    str_value* buffer[buffer_size];
    int num_docs;

    str_container_impl()
    {
        num_docs = 0;
        ZeroMemory(buffer, sizeof(buffer));
    }

    str_value* find(u32 crc, u32 length, pcstr str) const
    {
        str_value* candidate = buffer[crc % buffer_size];
        while (candidate)
        {
            if (candidate->dwCRC == crc && candidate->dwLength == length &&
                !memcmp(candidate->value, str, length))
            {
                return candidate;
            }

            candidate = candidate->next;
        }

        return nullptr;
    }

    void insert(str_value* value)
    {
        str_value** element = &buffer[value->dwCRC % buffer_size];
        value->next = *element;
        *element = value;
    }

    std::pair<size_t, size_t> clean()
    {
        size_t freed{}, alive{};
        for (size_t i = 0; i < buffer_size; ++i)
        {
            str_value** current = &buffer[i];

            while (*current != nullptr)
            {
                str_value* value = *current;
                // Revivals happen exclusively in dock() under the container lock held here,
                // so a zero count observed now cannot be acquired concurrently.
                if (!value->dwReference.load(std::memory_order_acquire))
                {
                    *current = value->next;
                    xr_free(value);
                    ++freed;
                }
                else
                {
                    ++alive;
                    current = &value->next;
                }
            }
        }
        return { freed, alive };
    }

    void verify() const
    {
        Msg("strings verify started");
        for (size_t i = 0; i < buffer_size; ++i)
        {
            const str_value* value = buffer[i];
            while (value)
            {
                const auto crc = crc32(value->value, value->dwLength);
                string32 crc_str;
                R_ASSERT3(crc == value->dwCRC, "CorePanic: read-only memory corruption (shared_strings)",
                    xr_itoa(value->dwCRC, crc_str, 16));
                R_ASSERT3(value->dwLength == xr_strlen(value->value),
                    "CorePanic: read-only memory corruption (shared_strings, internal structures)", value->value);
                value = value->next;
            }
        }
        Msg("strings verify completed");
    }

    void dump(FILE* f) const
    {
        for (size_t i = 0; i < buffer_size; ++i)
        {
            str_value* value = buffer[i];
            while (value)
            {
                fprintf(f, "ref[%4u]-len[%3u]-crc[%8X] : %s\n",
                    value->dwReference.load(std::memory_order_relaxed), value->dwLength, value->dwCRC,
                    value->value);
                value = value->next;
            }
        }
    }

    void dump(IWriter* f) const
    {
        for (size_t i = 0; i < buffer_size; ++i)
        {
            str_value* value = buffer[i];
            string4096 temp;
            while (value)
            {
                xr_sprintf(temp, sizeof(temp), "ref[%4u]-len[%3u]-crc[%8X] : %s\n",
                    value->dwReference.load(std::memory_order_relaxed), value->dwLength,
                    value->dwCRC, value->value);
                f->w_string(temp);
                value = value->next;
            }
        }
    }

    std::pair<size_t, size_t> stat_economy() const
    {
        size_t bytes{}, count{};
        for (size_t i = 0; i < buffer_size; ++i)
        {
            const str_value* value = buffer[i];
            while (value)
            {
                ++count;
                const u32 references = value->dwReference.load(std::memory_order_relaxed);
                if (references > 1)
                    bytes += (references - 1) * (value->dwLength + 1);
                value = value->next;
            }
        }
        return { bytes, count };
    }
};

str_container::str_container() :
    impl(xr_new<str_container_impl>())
#ifdef CONFIG_PROFILE_LOCKS
    , cs(MUTEX_PROFILE_ID(str_container))
#endif
{}

str_value* str_container::dock(pcstr value) const
{
    if (!value)
        return nullptr;

    // calc len
    const auto s_len = xr_strlen(value);
    const auto s_len_with_zero = s_len + 1;
    // The x86 header was smaller, so its page-size check rejected valid Dead Air strings only after moving to x64.
    VERIFY(s_len <= std::numeric_limits<u32>::max());

    const u32 length = static_cast<u32>(s_len);
    const u32 crc = crc32(value, s_len);

    impl->cs.Enter();

    // search
    str_value* result = impl->find(crc, length, value);

#ifdef DEBUG
    const bool is_leaked_string = !xr_strcmp(value, "enter leaked string here");
#endif // DEBUG

    // it may be the case, string is not found or has "non-exact" match
    if (!result
#ifdef DEBUG
        || is_leaked_string
#endif // DEBUG
        )
    {
        result = static_cast<str_value*>(xr_malloc(sizeof(str_value) + s_len_with_zero));

#ifdef DEBUG
        static int num_leaked_string = 0;
        if (is_leaked_string)
        {
            ++num_leaked_string;
            Msg("leaked_string: %d 0x%08x", num_leaked_string, result);
        }
#endif // DEBUG

        // Raw storage from xr_malloc: bring the atomic to life already owning the caller's reference.
        new (&result->dwReference) std::atomic<u32>(1);
        result->dwLength = length;
        result->dwCRC = crc;
        CopyMemory(result->value, value, s_len_with_zero);

        impl->insert(result);
    }
    else
    {
        // Acquire under the container lock: clean() holds the same lock, so it can never sweep
        // a zero-reference node between the lookup above and this revival.
        result->dwReference.fetch_add(1, std::memory_order_relaxed);
    }
    impl->cs.Leave();

    return result;
}

void str_container::clean() const
{
    impl->cs.Enter();
    // -str_debug: a CRC sweep right before recycling proves whether docked nodes were stomped in place.
    if (g_shared_str_debug)
        impl->verify();
    const auto [freed, alive] = impl->clean();
    if (g_shared_str_debug)
        Msg("* [x-ray]: str_debug: clean freed[%zu] nodes, alive[%zu]", freed, alive);
    impl->cs.Leave();
}

void str_container::verify() const
{
    impl->cs.Enter();
    impl->verify();
    impl->cs.Leave();
}

void str_container::dump() const
{
    impl->cs.Enter();
    FILE* F = fopen("d:\\$str_dump$.txt", "w");
    impl->dump(F);
    fclose(F);
    impl->cs.Leave();
}

void str_container::dump(IWriter* W) const
{
    impl->cs.Enter();
    impl->dump(W);
    impl->cs.Leave();
}

std::pair<size_t, size_t> str_container::stat_economy() const
{
    impl->cs.Enter();
    const auto [bytes, count] = impl->stat_economy();
    impl->cs.Leave();
    return { bytes, count };
}

str_container::~str_container()
{
    clean();
    // dump ();
    xr_delete(impl);
}

#else // 0/1

struct str_container_impl
{
    typedef xr_multiset<str_value*, str_value_cmp> cdb;
    int num_docs;
    str_container_impl() { num_docs = 0; }
    cdb container;
};

str_container::str_container() :
    impl(xr_new<str_container_impl>())
#ifdef CONFIG_PROFILE_LOCKS
    , cs(MUTEX_PROFILE_ID(str_container))
#endif
{}

str_value* str_container::dock(str_c value)
{
    if (0 == value)
        return 0;

    impl->cs.Enter();

// ++impl->num_docs;
// if ( impl->num_docs == 10000000 )
// {
// Msg("shared_strings");
// g_find_chunk_counter.flush();
// }
//
// //#ifdef FIND_CHUNK_BENCHMARK_ENABLE
// find_chunk_auto_timer timer;
// //#endif // FIND_CHUNK_BENCHMARK_ENABLE

    str_value* result = 0;

    // calc len
    u32 s_len = xr_strlen(value);
    u32 s_len_with_zero = (u32)s_len + 1;
    VERIFY(sizeof(str_value) + s_len_with_zero < 4096);

    // setup find structure
    char header[sizeof(str_value)];
    str_value* sv = (str_value*)header;
    sv->dwReference = 0;
    sv->dwLength = s_len;
    sv->dwCRC = crc32(value, s_len);
    sv->next = NULL;

    // search
    str_container_impl::cdb::iterator I = impl->container.find(sv); // only integer compares :)
    if (I != impl->container.end())
    {
        // something found - verify, it is exactly our string
        str_container_impl::cdb::iterator save = I;
        for (; I != impl->container.end() && (*I)->dwCRC == sv->dwCRC; ++I)
        {
            str_value* V = (*I);
            if (V->dwLength != sv->dwLength)
                continue;
            if (0 != memcmp(V->value, value, s_len))
                continue;
            result = V; // found
            break;
        }
    }

    bool is_leaked_string = !xr_strcmp(value, "enter leaked string here");

    // it may be the case, string is not found or has "non-exact" match
    if (0 == result || is_leaked_string)
    {
        // Insert string

        result = (str_value*)xr_malloc(sizeof(str_value) + s_len_with_zero
#ifdef DEBUG_MEMORY_NAME
            ,
            "storage: sstring"
#endif // DEBUG_MEMORY_NAME
            );

        static int num11 = 0;

        if (is_leaked_string)
        {
            ++num11;
            Msg("leaked_string: %d 0x%08x", num11, result);
        }

        result->dwReference = 0;
        result->dwLength = sv->dwLength;
        result->dwCRC = sv->dwCRC;
        result->next = NULL;

        CopyMemory(result->value, value, s_len_with_zero);

        impl->container.insert(result);
    }

    impl->cs.Leave();

    return result;
}

void str_container::clean()
{
    impl->cs.Enter();
    str_container_impl::cdb::iterator it = impl->container.begin();
    str_container_impl::cdb::iterator end = impl->container.end();
    for (; it != end;)
    {
        str_value* sv = *it;
        if (0 == sv->dwReference)
        {
            str_container_impl::cdb::iterator i_current = it;
            str_container_impl::cdb::iterator i_next = ++it;
            xr_free(sv);
            impl->container.erase(i_current);
            it = i_next;
        }
        else
        {
            it++;
        }
    }
    if (impl->container.empty())
        impl->container.clear();
    impl->cs.Leave();
}

void str_container::verify()
{
    impl->cs.Enter();
    str_container_impl::cdb::iterator it = impl->container.begin();
    str_container_impl::cdb::iterator end = impl->container.end();
    for (; it != end; ++it)
    {
        str_value* sv = *it;
        u32 crc = crc32(sv->value, sv->dwLength);
        string32 crc_str;
        R_ASSERT3(crc == sv->dwCRC,
            "CorePanic: read-only memory corruption (shared_strings)", xr_itoa(sv->dwCRC, crc_str, 16));
        R_ASSERT3(sv->dwLength == xr_strlen(sv->value),
            "CorePanic: read-only memory corruption (shared_strings, internal structures)", sv->value);
    }
    impl->cs.Leave();
}

void str_container::dump()
{
    impl->cs.Enter();
    str_container_impl::cdb::iterator it = impl->container.begin();
    str_container_impl::cdb::iterator end = impl->container.end();
    FILE* F = fopen("d:\\$str_dump$.txt", "w");
    for (; it != end; it++)
        fprintf(
            F, "ref[%4d]-len[%3d]-crc[%8X] : %s\n", (*it)->dwReference, (*it)->dwLength, (*it)->dwCRC, (*it)->value);
    fclose(F);
    impl->cs.Leave();
}

u32 str_container::stat_economy()
{
    impl->cs.Enter();
    str_container_impl::cdb::iterator it = impl->container.begin();
    str_container_impl::cdb::iterator end = impl->container.end();
    int counter = 0;
    counter -= sizeof(*this);
    counter -= sizeof(str_container_impl::cdb::allocator_type);
    const int node_size = 20;
    for (; it != end; it++)
    {
        counter -= sizeof(str_value);
        counter -= node_size;
        counter += int((int((*it)->dwReference) - 1) * int((*it)->dwLength + 1));
    }
    impl->cs.Leave();

    return u32(counter);
}

str_container::~str_container()
{
    clean();
    // dump ();
    xr_delete(impl);
    // R_ASSERT(impl->container.empty());
}

#endif // 0/1
