#include "stdafx.h"
#pragma hdrstop

#include <array>

// Reflects CRC bits in the lookup table
constexpr u32 reflect(u32 ref, char ch) noexcept
{
    // Used only by Init_CRC32_Table().

    u32 value(0);

    // Swap bit 0 for bit 7
    // bit 1 for bit 6, etc.
    for (int i = 1; i < (ch + 1); i++)
    {
        if (ref & 1)
            value |= 1 << (ch - i);
        ref >>= 1;
    }
    return value;
}

constexpr std::array<u32, 256> generate_crc32_lookup_table() noexcept
{
    std::array<u32, 256> crc32_table{};

    // This is the official polynomial used by CRC-32
    // in PKZip, WinZip and Ethernet.
    u32 ulPolynomial = 0x04c11db7;

    // 256 values representing ASCII character codes.
    for (int i = 0; i <= 0xFF; i++)
    {
        crc32_table[i] = reflect(i, 8) << 24;
        for (int j = 0; j < 8; j++)
            crc32_table[i] = (crc32_table[i] << 1) ^ (crc32_table[i] & (1 << 31) ? ulPolynomial : 0);
        crc32_table[i] = reflect(crc32_table[i], 32);
    }

    return crc32_table;
}

constexpr std::array<std::array<u32, 256>, 8> generate_crc32_slicing_table() noexcept
{
    std::array<std::array<u32, 256>, 8> table{};
    table[0] = generate_crc32_lookup_table();

    for (size_t slice = 1; slice < table.size(); ++slice)
    {
        for (size_t value = 0; value < table[slice].size(); ++value)
        {
            const u32 previous = table[slice - 1][value];
            table[slice][value] = (previous >> 8) ^ table[0][previous & 0xff];
        }
    }

    return table;
}

static constexpr auto crc32_table = generate_crc32_slicing_table();

static u32 update_crc32(const u8* buffer, u32 len, u32 crc)
{
    while (len >= 8)
    {
        u32 first;
        std::memcpy(&first, buffer, sizeof(first));
        first ^= crc;

        crc = crc32_table[7][first & 0xff] ^
            crc32_table[6][(first >> 8) & 0xff] ^
            crc32_table[5][(first >> 16) & 0xff] ^
            crc32_table[4][first >> 24] ^
            crc32_table[3][buffer[4]] ^
            crc32_table[2][buffer[5]] ^
            crc32_table[1][buffer[6]] ^
            crc32_table[0][buffer[7]];
        buffer += 8;
        len -= 8;
    }

    while (len--)
        crc = (crc >> 8) ^ crc32_table[0][(crc & 0xff) ^ *buffer++];

    return crc;
}

u32 crc32(const void* P, u32 len)
{
    return update_crc32(static_cast<const u8*>(P), len, 0xffffffff) ^ 0xffffffff;
}

u32 crc32(const void* P, u32 len, u32 starting_crc)
{
    return update_crc32(static_cast<const u8*>(P), len, 0xffffffff ^ starting_crc) ^ 0xffffffff;
}

u32 path_crc32(const char* path, u32 len)
{
    u32 ulCRC = 0xffffffff;
    u8* buffer = (u8*)path;

    while (len--)
    {
        const u8 c = *buffer;
        if (c != '/' && c != _DELIMITER)
        {
            ulCRC = (ulCRC >> 8) ^ crc32_table[0][(ulCRC & 0xFF) ^ *buffer];
        }

        ++buffer;
    }

    return ulCRC ^ 0xffffffff;
}
