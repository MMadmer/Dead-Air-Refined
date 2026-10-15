// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#ifndef DA_LATTICE_H
#define DA_LATTICE_H

// The hash of a lattice point, 0..1, for the value noises - one function for the shaders and the engine.
//
// Integer arithmetic on purpose. A value noise reads the hash at its cell's four corners, and the cell next
// door reads the same corner again: the two reads must agree to the bit, or the noise steps at the border.
// A float hash of the corner (frac((i + 1) * 127.1) and its kin) agrees only while the compiler keeps the
// sum and the product apart. DXC, the DirectX 12 compiler, folds them into i * 127.1 + 127.1, one rounding
// off at a few thousand, and frac() and the products after it blow that into another number: the puddles,
// the cloud deck and the fire broke into square cells on DirectX 12 and stayed smooth on DirectX 11.
// PCG2D (Jarzynski & Olano 2020) on the corner's integers is exact on any compiler and on the CPU.
//
// Scalar arguments and no #include: the engine includes this file by relative path (Environment.cpp), and the
// shader cache's include walker reads no <angle brackets>.

#ifdef __cplusplus
#define DA_LAT_FUNC inline
#define DA_LAT_UINT unsigned
#define DA_LAT_U(x) static_cast<unsigned>(static_cast<int>(x))
#define DA_LAT_F(x) static_cast<float>(x)
#else
#define DA_LAT_FUNC
#define DA_LAT_UINT uint
#define DA_LAT_U(x) uint(int(x))
#define DA_LAT_F(x) float(x)
#endif

// ix, iz: the corner (whole numbers in a float, as floor() leaves them); salt: one noise apart from another.
DA_LAT_FUNC float da_lattice_hash(float ix, float iz, DA_LAT_UINT salt)
{
    DA_LAT_UINT x = DA_LAT_U(ix) * 1664525u + 1013904223u;
    DA_LAT_UINT y = (DA_LAT_U(iz) ^ (salt * 0x9E3779B9u)) * 1664525u + 1013904223u;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16u;
    y ^= y >> 16u;
    x += y * 1664525u;
    y += x * 1664525u;
    y ^= y >> 16u;
    return DA_LAT_F(y >> 8u) * (1.0f / 16777216.0f);
}

// The same for a 3D lattice (the fire's volume).
DA_LAT_FUNC float da_lattice_hash3(float ix, float iy, float iz, DA_LAT_UINT salt)
{
    return da_lattice_hash(ix, iz, salt ^ (DA_LAT_U(iy) * 0x85EBCA6Bu + 0x68E31DA4u));
}

#endif
