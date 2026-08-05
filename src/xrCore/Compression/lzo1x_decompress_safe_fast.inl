/* SPDX-License-Identifier: GPL-2.0-or-later

   This decoder is derived from LZO 2.10 src/lzo1x_d2.c,
   src/lzo1x_d.ch, and src/lzo1_d.ch.

   Copyright (C) 1996-2017 Markus Franz Xaver Johannes Oberhumer
   All Rights Reserved.

   Dead Air changes keep the LZO1X bitstream and safe-decoder error
   semantics while using the platform memcpy implementation for large
   literals and non-overlapping matches.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

namespace xr_lzo
{
constexpr lzo_uint literalMemcpyThreshold = 32;
constexpr lzo_uint matchMemcpyThreshold = 8;

inline void copy_literal_run(unsigned char*& output, const unsigned char*& input, lzo_uint length)
{
    if (length >= literalMemcpyThreshold)
    {
        std::memcpy(output, input, length);
        output += length;
        input += length;
        return;
    }

    while (length >= sizeof(std::uint64_t))
    {
        std::uint64_t value;
        std::memcpy(&value, input, sizeof(value));
        std::memcpy(output, &value, sizeof(value));
        output += sizeof(value);
        input += sizeof(value);
        length -= sizeof(value);
    }

    if (length >= sizeof(std::uint32_t))
    {
        std::uint32_t value;
        std::memcpy(&value, input, sizeof(value));
        std::memcpy(output, &value, sizeof(value));
        output += sizeof(value);
        input += sizeof(value);
        length -= sizeof(value);
    }

    while (length--)
        *output++ = *input++;
}

inline void copy_match(unsigned char*& output, const unsigned char* match, lzo_uint length, lzo_uint offset)
{
    // memcpy is valid only when the whole match is non-overlapping.
    if (length >= matchMemcpyThreshold && offset >= length)
    {
        std::memcpy(output, match, length);
        output += length;
        return;
    }

    // Fixed-size chunks retain LZO's overlap semantics for short offsets.
    if (offset >= sizeof(std::uint64_t))
    {
        while (length >= sizeof(std::uint64_t))
        {
            std::uint64_t value;
            std::memcpy(&value, match, sizeof(value));
            std::memcpy(output, &value, sizeof(value));
            output += sizeof(value);
            match += sizeof(value);
            length -= sizeof(value);
        }
    }
    else if (offset >= sizeof(std::uint32_t))
    {
        while (length >= sizeof(std::uint32_t))
        {
            std::uint32_t value;
            std::memcpy(&value, match, sizeof(value));
            std::memcpy(output, &value, sizeof(value));
            output += sizeof(value);
            match += sizeof(value);
            length -= sizeof(value);
        }
    }

    while (length--)
        *output++ = *match++;
}

inline int decompress_safe(const lzo_bytep source, lzo_uint sourceLength,
    lzo_bytep destination, lzo_uintp destinationLength, lzo_voidp workMemory)
{
    auto* output = destination;
    const auto* input = source;
    const auto* const inputEnd = source + sourceLength;
    auto* const outputEnd = destination + *destinationLength;
    const unsigned char* match;
    lzo_uint matchOffset;
    lzo_uint token;

    (void)workMemory;
    *destinationLength = 0;

#define XR_LZO_NEED_INPUT(count) \
    do \
    { \
        if (static_cast<lzo_uint>(inputEnd - input) < static_cast<lzo_uint>(count)) \
            goto input_overrun; \
    } while (false)

#define XR_LZO_NEED_OUTPUT(count) \
    do \
    { \
        if (static_cast<lzo_uint>(outputEnd - output) < static_cast<lzo_uint>(count)) \
            goto output_overrun; \
    } while (false)

#define XR_LZO_TEST_INPUT_LENGTH(value) \
    do \
    { \
        if ((value) > std::numeric_limits<lzo_uint>::max() - 511) \
            goto input_overrun; \
    } while (false)

#define XR_LZO_TEST_OUTPUT_LENGTH(value) \
    do \
    { \
        if ((value) > std::numeric_limits<lzo_uint>::max() - 511) \
            goto output_overrun; \
    } while (false)

#define XR_LZO_SET_MATCH() \
    do \
    { \
        if (!matchOffset || matchOffset > static_cast<lzo_uint>(output - destination)) \
            goto lookbehind_overrun; \
        match = output - matchOffset; \
    } while (false)

    XR_LZO_NEED_INPUT(1);
    if (*input > 17)
    {
        token = *input++ - 17;
        if (token < 4)
            goto match_next;
        XR_LZO_NEED_OUTPUT(token);
        XR_LZO_NEED_INPUT(token + 3);
        copy_literal_run(output, input, token);
        goto first_literal_run;
    }

    for (;;)
    {
        XR_LZO_NEED_INPUT(3);
        token = *input++;
        if (token >= 16)
            goto match_token;

        if (token == 0)
        {
            while (*input == 0)
            {
                token += 255;
                ++input;
                XR_LZO_TEST_INPUT_LENGTH(token);
                XR_LZO_NEED_INPUT(1);
            }
            token += 15 + *input++;
        }

        XR_LZO_NEED_OUTPUT(token + 3);
        XR_LZO_NEED_INPUT(token + 6);
        copy_literal_run(output, input, token + 3);

first_literal_run:
        token = *input++;
        if (token >= 16)
            goto match_token;

        matchOffset = 1 + 0x0800 + (token >> 2) + (*input++ << 2);
        XR_LZO_SET_MATCH();
        XR_LZO_NEED_OUTPUT(3);
        *output++ = *match++;
        *output++ = *match++;
        *output++ = *match;
        goto match_done;

        for (;;)
        {
match_token:
            if (token >= 64)
            {
                matchOffset = 1 + ((token >> 2) & 7) + (*input++ << 3);
                token = (token >> 5) - 1;
                XR_LZO_SET_MATCH();
                XR_LZO_NEED_OUTPUT(token + 2);
                goto copy_match;
            }

            if (token >= 32)
            {
                token &= 31;
                if (token == 0)
                {
                    while (*input == 0)
                    {
                        token += 255;
                        ++input;
                        XR_LZO_TEST_OUTPUT_LENGTH(token);
                        XR_LZO_NEED_INPUT(1);
                    }
                    token += 31 + *input++;
                    XR_LZO_NEED_INPUT(2);
                }
                matchOffset = 1 + (input[0] >> 2) + (input[1] << 6);
                input += 2;
            }
            else if (token >= 16)
            {
                matchOffset = (token & 8) << 11;
                token &= 7;
                if (token == 0)
                {
                    while (*input == 0)
                    {
                        token += 255;
                        ++input;
                        XR_LZO_TEST_OUTPUT_LENGTH(token);
                        XR_LZO_NEED_INPUT(1);
                    }
                    token += 7 + *input++;
                    XR_LZO_NEED_INPUT(2);
                }
                matchOffset += (input[0] >> 2) + (input[1] << 6);
                input += 2;
                if (!matchOffset)
                    goto eof_found;
                matchOffset += 0x4000;
            }
            else
            {
                matchOffset = 1 + (token >> 2) + (*input++ << 2);
                XR_LZO_SET_MATCH();
                XR_LZO_NEED_OUTPUT(2);
                *output++ = *match++;
                *output++ = *match;
                goto match_done;
            }

            XR_LZO_SET_MATCH();
            XR_LZO_NEED_OUTPUT(token + 2);

copy_match:
            copy_match(output, match, token + 2, matchOffset);

match_done:
            token = input[-2] & 3;
            if (token == 0)
                break;

match_next:
            XR_LZO_NEED_OUTPUT(token);
            XR_LZO_NEED_INPUT(token + 3);
            *output++ = *input++;
            if (token > 1)
            {
                *output++ = *input++;
                if (token > 2)
                    *output++ = *input++;
            }
            token = *input++;
        }
    }

eof_found:
    *destinationLength = static_cast<lzo_uint>(output - destination);
    if (input == inputEnd)
        return LZO_E_OK;
    return input < inputEnd ? LZO_E_INPUT_NOT_CONSUMED : LZO_E_INPUT_OVERRUN;

input_overrun:
    *destinationLength = static_cast<lzo_uint>(output - destination);
    return LZO_E_INPUT_OVERRUN;

output_overrun:
    *destinationLength = static_cast<lzo_uint>(output - destination);
    return LZO_E_OUTPUT_OVERRUN;

lookbehind_overrun:
    *destinationLength = static_cast<lzo_uint>(output - destination);
    return LZO_E_LOOKBEHIND_OVERRUN;

#undef XR_LZO_SET_MATCH
#undef XR_LZO_TEST_OUTPUT_LENGTH
#undef XR_LZO_TEST_INPUT_LENGTH
#undef XR_LZO_NEED_OUTPUT
#undef XR_LZO_NEED_INPUT
}
}
