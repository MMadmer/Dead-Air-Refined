// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Byte address buffers of 16-byte records read and written whole. DXC's SPIR-V turns Load4 / Store4 into four
// 32-bit accesses of a uint array; the same storage buffer seen as an array of uint4 is one 128-bit access. The
// Vulkan layer binds "<name>_v4" to the register of <name> (vk_spirv.cpp). The address of a 4-wide access is a
// multiple of 16. FXC and DXIL keep the byte address buffer as it is.
#ifndef DA_RAW_BUFFER_H
#define DA_RAW_BUFFER_H

#ifdef GRIZZLY_SPIRV
#define DA_RAW(name, reg)               ByteAddressBuffer name : register(reg); StructuredBuffer<uint4> name##_v4
#define DA_RAW_RW(name, reg)            RWByteAddressBuffer name : register(reg); RWStructuredBuffer<uint4> name##_v4
#define DA_LOAD4(name, addr)            name##_v4[(addr) >> 4]
#define DA_STORE4(name, addr, value)    name##_v4[(addr) >> 4] = (value)
#else
#define DA_RAW(name, reg)               ByteAddressBuffer name : register(reg)
#define DA_RAW_RW(name, reg)            RWByteAddressBuffer name : register(reg)
#define DA_LOAD4(name, addr)            name.Load4(addr)
#define DA_STORE4(name, addr, value)    name.Store4(addr, value)
#endif

#endif // DA_RAW_BUFFER_H
