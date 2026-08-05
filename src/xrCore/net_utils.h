#pragma once
#ifndef _INCDEF_NETUTILS_H_
#define _INCDEF_NETUTILS_H_
#include <string.h>
#include "xrCore/xr_types.h"
#include "xrCore/xrstring.h"
#include "client_id.h"
#include "xrCommon/xr_string.h"

// fwd. decl.
template <class T> struct _vector3;
typedef _vector3<float> Fvector;
template <class T> struct _vector4;
typedef _vector4<float> Fvector4;


#pragma pack(push, 1)

const u32 NET_PacketSizeLimit = 16 * 1024;

struct XRCORE_API IIniFileStream
{
    virtual void __stdcall move_begin() = 0;

    virtual void __stdcall w_float(float a) = 0;
    virtual void __stdcall w_vec3(const Fvector& a) = 0;
    virtual void __stdcall w_vec4(const Fvector4& a) = 0;
    virtual void __stdcall w_u64(u64 a) = 0;
    virtual void __stdcall w_s64(s64 a) = 0;
    virtual void __stdcall w_u32(u32 a) = 0;
    virtual void __stdcall w_s32(s32 a) = 0;
    virtual void __stdcall w_u16(u16 a) = 0;
    virtual void __stdcall w_s16(s16 a) = 0;
    virtual void __stdcall w_u8(u8 a) = 0;
    virtual void __stdcall w_s8(s8 a) = 0;
    virtual void __stdcall w_stringZ(pcstr S) = 0;

    virtual void __stdcall r_vec3(Fvector&) = 0;
    virtual void __stdcall r_vec4(Fvector4&) = 0;
    virtual void __stdcall r_float(float&) = 0;
    virtual void __stdcall r_u8(u8&) = 0;
    virtual void __stdcall r_u16(u16&) = 0;
    virtual void __stdcall r_u32(u32&) = 0;
    virtual void __stdcall r_u64(u64&) = 0;
    virtual void __stdcall r_s8(s8&) = 0;
    virtual void __stdcall r_s16(s16&) = 0;
    virtual void __stdcall r_s32(s32&) = 0;
    virtual void __stdcall r_s64(s64&) = 0;

    virtual void __stdcall r_string(pstr dest, u32 dest_size) = 0;
    // virtual void __stdcall r_tell () = 0;
    // virtual void __stdcall r_seek (u32 pos) = 0;
    virtual void __stdcall skip_stringZ() = 0;
};

#define INI_W(what_to_do)      \
    \
if(inistream)                  \
    \
{                       \
        inistream->what_to_do; \
    \
}

#define INI_ASSERT(what_to_do)                              \
    \
{                                                    \
        \
if(inistream) R_ASSERT3(0, #what_to_do, "not implemented"); \
    \
}

struct NET_Buffer
{
    u8 data[NET_PacketSizeLimit];
    u32 count;
};

namespace net_packet_detail
{
static ICF void write_inline(
    NET_Buffer& buffer, IIniFileStream* inistream, bool wAllow, const void* data, u32 count)
{
    R_ASSERT(!inistream || wAllow);
    VERIFY(data && count);
    VERIFY(buffer.count + count < NET_PacketSizeLimit);
    memcpy(buffer.data + buffer.count, data, count);
    buffer.count += count;
    VERIFY(buffer.count < NET_PacketSizeLimit);
}
}

// Keep the legacy xrCore exports while allowing clients to inline packet primitives.
#if defined(XRCORE_EXPORTS) || defined(XRAY_STATIC_BUILD)
#define XRCORE_NET_PACKET_API XRCORE_API
#else
#define XRCORE_NET_PACKET_API
#endif

class XRCORE_NET_PACKET_API NET_Packet
{
public:
    IIniFileStream* inistream{};

    void construct(const void* data, unsigned size)
    {
        memcpy(B.data, data, size);
        B.count = size;
    }

    NET_Buffer B{};
    u32 r_pos{};
    u32 timeReceive{};
    bool w_allow{ true };

public:
    // writing - main
    ICF void write_start()
    {
        B.count = 0;
        INI_W(move_begin());
    }
    ICF void w_begin(u16 type)
    {
        B.count = 0;
        w_u16(type);
    }

    struct W_guard
    {
        bool* guarded;
        W_guard(bool* b) noexcept : guarded(b) { *b = true; }
        ~W_guard() { *guarded = false; }
    };

    void w(const void* data, u32 count);
    void w_seek(u32 pos, const void* p, u32 count);
    ICF u32 w_tell() { return B.count; }
    // writing - utilities
    ICF void w_float(float a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 4);
        INI_W(w_float(a));
    } // float
    ICF void w_vec3(const Fvector& a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 3 * sizeof(float));
        INI_W(w_vec3(a));
    } // vec3
    ICF void w_vec4(const Fvector4& a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 4 * sizeof(float));
        INI_W(w_vec4(a));
    } // vec4
    ICF void w_u64(u64 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 8);
        INI_W(w_u64(a));
    } // qword (8b)
    ICF void w_s64(s64 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 8);
        INI_W(w_s64(a));
    } // qword (8b)
    ICF void w_u32(u32 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 4);
        INI_W(w_u32(a));
    } // dword (4b)
    ICF void w_s32(s32 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 4);
        INI_W(w_s32(a));
    } // dword (4b)
    ICF void w_u16(u16 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 2);
        INI_W(w_u16(a));
    } // word (2b)
    ICF void w_s16(s16 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 2);
        INI_W(w_s16(a));
    } // word (2b)
    ICF void w_u8(u8 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 1);
        INI_W(w_u8(a));
    } // byte (1b)
    ICF void w_s8(s8 a)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, &a, 1);
        INI_W(w_s8(a));
    } // byte (1b)

    void w_float_q16(float a, float min, float max);
    void w_float_q8(float a, float min, float max);
    void w_angle16(float a);
    void w_angle8(float a);
    void w_dir(const Fvector& D);
    void w_sdir(const Fvector& D);
    ICF void w_stringZ(pcstr S)
    {
        W_guard g(&w_allow);
        net_packet_detail::write_inline(B, inistream, w_allow, S, (u32)xr_strlen(S) + 1);
        INI_W(w_stringZ(S));
    }
    void w_stringZ(const shared_str& value);
    void w_matrix(Fmatrix& M);

    void w_clientID(ClientID& C) { w_u32(C.value()); }

    void w_chunk_open8(u32& position);
	void w_chunk_close8(u32 position);

	void w_chunk_open16(u32& position);
	void w_chunk_close16(u32 position);

    // reading
    void read_start();
    u32 r_begin(u16& type);
    void r_seek(u32 pos);
    u32 r_tell();

    void r(void* p, u32 count);
    bool r_eof();
    u32 r_elapsed();
    void r_advance(u32 size);

    // reading - utilities
    void r_vec3(Fvector& A);
    void r_vec4(Fvector4& A);
    void r_float(float& A);
    void r_u64(u64& A);
    void r_s64(s64& A);
    void r_u32(u32& A);
    void r_s32(s32& A);
    void r_u16(u16& A);
    void r_s16(s16& A);
    void r_u8(u8& A);
    void r_s8(s8& A);

    // IReader compatibility
    Fvector r_vec3();
    Fvector4 r_vec4();
    float r_float_q8(float min, float max);
    float r_float_q16(float min, float max);
    float r_float();
    u64 r_u64();
    s64 r_s64();
    u32 r_u32();
    s32 r_s32();
    u16 r_u16();
    s16 r_s16();
    u8 r_u8();
    s8 r_s8();

    void r_float_q16(float& A, float min, float max);
    void r_float_q8(float& A, float min, float max);
    void r_angle16(float& A);
    void r_angle8(float& A);
    void r_dir(Fvector& A);

    void r_sdir(Fvector& A);
    void r_stringZ(pstr S);
    void r_stringZ(xr_string& dest);
    void r_stringZ(shared_str& dest);

    void skip_stringZ();

    void r_stringZ_s(pstr string, u32 size);

    template <u32 size>
    inline void r_stringZ_s(char (&string)[size])
    {
        r_stringZ_s(string, size);
    }

    void r_matrix(Fmatrix& M);
    void r_clientID(ClientID& C);
};

namespace net_packet_detail
{
static ICF void write_string(NET_Packet& packet, const shared_str& value)
{
    NET_Packet::W_guard guard(&packet.w_allow);
    if (value)
    {
        write_inline(packet.B, packet.inistream, packet.w_allow,
            value.c_str(), static_cast<u32>(value.size() + 1));
    }
    else
    {
        constexpr u8 terminator = 0;
        write_inline(packet.B, nullptr, packet.w_allow, &terminator, sizeof(terminator));
        // Preserve the legacy nested guard state for null shared strings.
        packet.w_allow = false;
    }
    if (packet.inistream)
        packet.inistream->w_stringZ(value.c_str());
}
}

#undef XRCORE_NET_PACKET_API

#pragma pack(pop)

#endif /*_INCDEF_NETUTILS_H_*/
