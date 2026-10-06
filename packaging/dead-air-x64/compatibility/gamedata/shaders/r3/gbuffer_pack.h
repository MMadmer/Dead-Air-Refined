#ifndef	gbuffer_pack_h_included
#define	gbuffer_pack_h_included

// The G-buffer's bit packing: pure functions with no resources, shared by the FXC shaders (through
// common_functions.h) and the DXC ray tracing passes (rt/).

// Material codes 27-31 above the stock range: two-sided foliage (the blade grass) and snow. Pass the
// mtl through da_mtl_from_code / da_pack_foliage_mtl; never re-pack a .w read back as a float.
#include "da_material_codes.h"

#define USABLE_BIT_1                uint(0x00002000)
#define USABLE_BIT_2                uint(0x00004000)
#define USABLE_BIT_3                uint(0x00008000)
#define USABLE_BIT_4                uint(0x00010000)
#define USABLE_BIT_5                uint(0x00020000)
#define USABLE_BIT_6                uint(0x00040000)
#define USABLE_BIT_7                uint(0x00080000)
#define USABLE_BIT_8                uint(0x00100000)
#define USABLE_BIT_9                uint(0x00200000)
#define USABLE_BIT_10               uint(0x00400000)
#define USABLE_BIT_11               uint(0x00800000)   // At least two of those four bit flags must be mutually exclusive (i.e. all 4 bits must not be set together)
#define USABLE_BIT_12               uint(0x01000000)   // This is because setting 0x47800000 sets all 5 FP16 exponent bits to 1 which means infinity
#define USABLE_BIT_13               uint(0x02000000)   // This will be translated to a +/-MAX_FLOAT in the FP16 render target (0xFBFF/0x7BFF), overwriting the
#define USABLE_BIT_14               uint(0x04000000)   // mantissa bits where other bit flags are stored.
#define USABLE_BIT_15               uint(0x80000000)
#define MUST_BE_SET                 uint(0x40000000)   // This flag *must* be stored in the floating-point representation of the bit flag to store

/*
float2 gbuf_pack_normal( float3 norm )
{
   float2 res;

   res = 0.5 * ( norm.xy + float2( 1, 1 ) ) ;
   res.x *= ( norm.z < 0 ? -1.0 : 1.0 );

   return res;
}

float3 gbuf_unpack_normal( float2 norm )
{
   float3 res;

   res.xy = ( 2.0 * abs( norm ) ) - float2(1,1);

   res.z = ( norm.x < 0 ? -1.0 : 1.0 ) * sqrt( abs( 1 - res.x * res.x - res.y * res.y ) );

   return res;
}
*/

// Holger Gruen AMD - I change normal packing and unpacking to make sure N.z is accessible without ALU cost
// this help the HDAO compute shader to run more efficiently
float2 gbuf_pack_normal( float3 norm )
{
   float2 res;

   res.x  = norm.z;
   res.y  = 0.5f * ( norm.x + 1.0f ) ;
   res.y *= ( norm.y < 0.0f ? -1.0f : 1.0f );

   return res;
}

float3 gbuf_unpack_normal( float2 norm )
{
   float3 res;

   res.z  = norm.x;
   res.x  = ( 2.0f * abs( norm.y ) ) - 1.0f;
   res.y = ( norm.y < 0 ? -1.0 : 1.0 ) * sqrt( abs( 1 - res.x * res.x - res.z * res.z ) );

   return res;
}

// Raised by deffer_impl_flat for the pixels its puddle mask covers - rain puddles live on the
// static ground only. The puddle reflection pass reads the flag back and leaves every other
// pixel alone: a grass blade or a leaf card whose XZ falls on a puddle is not water, however
// level the depth around it looks from afar.
static uint da_gbuf_ground = 0u;

float gbuf_pack_hemi_mtl( float hemi, float mtl )
{
   uint packed_mtl = uint( ( mtl / 1.333333333 ) * 31.0 );
	// hemi keeps seven bits (steps of 1/127 - an occlusion term never showed the eighth); bit 13,
	// the low FP16 mantissa bit, carries the ground flag.
	uint packed = ( MUST_BE_SET + ( uint( saturate(hemi) * 127.9 ) << 14 ) + ( ( packed_mtl & uint( 31 ) ) << 21 )
		+ ( da_gbuf_ground != 0u ? USABLE_BIT_1 : 0u ) );

   if( ( packed & USABLE_BIT_13 ) == 0 )
      packed |= USABLE_BIT_14;

   if( packed_mtl & uint( 16 ) )
      packed |= USABLE_BIT_15;

   return asfloat( packed );
}

float gbuf_unpack_hemi( float mtl_hemi )
{
	return float( ( asuint( mtl_hemi ) >> 14 ) & uint(127) ) * (1.0/127.0);
}

float gbuf_unpack_ground( float mtl_hemi )
{
	return ( asuint( mtl_hemi ) & USABLE_BIT_1 ) != 0u ? 1.0 : 0.0;
}

float gbuf_unpack_mtl( float mtl_hemi )
{
   uint packed       = asuint( mtl_hemi );
   uint packed_hemi  = ( ( packed >> 21 ) & uint(15) ) + ( ( packed & USABLE_BIT_15 ) == 0 ? 0 : 16 );
   return float( packed_hemi ) * (1.0/31.0) * 1.333333333;
}

#endif	//	gbuffer_pack_h_included
