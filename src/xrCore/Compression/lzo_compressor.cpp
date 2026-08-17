#include "stdafx.h"
#include "lzo_compressor.h"
#include "lzo/lzo1x.h"

int lzo_compress_dict(
    const void* input, u32 inputSize, void* output, u32& outputSize, void* workMem, const void* dict, u32 dictSize)
{
    // lzo_uint is 64-bit on x64, while the public engine ABI intentionally keeps a u32 size reference.
    lzo_uint lzoOutputSize = outputSize;
    const int result = lzo1x_999_compress_dict(
        (lzo_bytep)input, inputSize, (lzo_bytep)output, &lzoOutputSize, workMem, (lzo_bytep)dict, dictSize);
    R_ASSERT(lzoOutputSize <= type_max<u32>);
    outputSize = static_cast<u32>(lzoOutputSize);
    return result;
}

int lzo_decompress_dict(
    const void* input, u32 inputSize, void* output, u32& outputSize, void* workMem, const void* dict, u32 dictSize)
{
    // lzo_uint is 64-bit on x64, while the public engine ABI intentionally keeps a u32 size reference.
    lzo_uint lzoOutputSize = outputSize;
    const int result = lzo1x_decompress_dict_safe(
        (lzo_bytep)input, inputSize, (lzo_bytep)output, &lzoOutputSize, workMem, (lzo_bytep)dict, dictSize);
    R_ASSERT(lzoOutputSize <= type_max<u32>);
    outputSize = static_cast<u32>(lzoOutputSize);
    return result;
}

int lzo_initialize() { return lzo_init(); }
u32 lzo_get_workmem_size() { return LZO1X_999_MEM_COMPRESS; }
