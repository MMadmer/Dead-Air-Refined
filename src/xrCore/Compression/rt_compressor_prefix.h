// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Kept apart from rt_compressor.h, which xrCore.h (every module's precompiled header) includes.
#pragma once

// The first dst_len bytes of an LZO stream (all of it when it is shorter): the bytes produced, 0 when the
// stream is broken. Unlike rtc_decompress, a run crossing dst_len is clipped instead of refused.
extern XRCORE_API size_t rtc_decompress_prefix(void* dst, size_t dst_len, const void* src, size_t src_len);
