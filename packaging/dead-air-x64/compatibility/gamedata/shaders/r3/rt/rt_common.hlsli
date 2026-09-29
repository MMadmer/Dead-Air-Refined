#ifndef	rt_common_hlsli_included
#define	rt_common_hlsli_included

// The ray tracing passes (DXC, cs_6_5, HLSL 2021): the bindings of the one RT root signature
// (da_rt_device.cpp), the view every pass carries at the head of its constants, and the maths the
// passes share. Every pass declares its own constants (b0), inputs (t0-t15) and outputs (u0-u7).

#include "gbuffer_pack.h"

// Static world, trees and moving objects, split by what may cast which shadow (dxr.md 3.3).
#define RT_MASK_STATIC			0x01
#define RT_MASK_STATIC_ALPHA	0x02
#define RT_MASK_FOLIAGE			0x04
#define RT_MASK_TRUNK			0x08
#define RT_MASK_DYNAMIC			0x10
#define RT_MASK_SKINNED			0x20
// Static geometry whose shader has no shadow pass: the cascades never saw it cast.
#define RT_MASK_NOSHADOW		0x40
// Shadow rays never see vegetation nor anything that moves: plant shadows stay in the swaying
// cascades, moving objects keep their shadow maps.
#define RT_RAYS_SHADOW			( RT_MASK_STATIC | RT_MASK_STATIC_ALPHA )
#define RT_RAYS_AO				( RT_MASK_STATIC | RT_MASK_STATIC_ALPHA | RT_MASK_TRUNK | RT_MASK_DYNAMIC | RT_MASK_SKINNED | RT_MASK_NOSHADOW )
#define RT_RAYS_REFLECTION		0xFF

// RtView.scene.z
#define RT_VIEW_GBUFFER_PACKED	0x1		// GBUFFER_OPTIMIZATION: normal and hemi live in the position target
#define RT_VIEW_HISTORY_VALID	0x2		// the histories hold the previous frame
#define RT_VIEW_VELOCITY		0x4		// the motion vectors were drawn this frame

RaytracingAccelerationStructure	rt_scene		: register( t0, space2 );
// Bindless: the layer's persistent range, indexed by the geometry tables.
Texture2D						rt_textures[]	: register( t0, space100 );
ByteAddressBuffer				rt_buffers[]	: register( t0, space101 );

SamplerState					rt_linear_wrap	: register( s0 );	// anisotropic 4
SamplerState					rt_point_clamp	: register( s1 );
SamplerState					rt_linear_clamp	: register( s2 );
SamplerComparisonState			rt_compare		: register( s3 );	// less-equal

// The camera of this frame (DaRtView, da_rt_passes.cpp). Matrices are row-major and apply as
// mul( float4( p, 1 ), m ), like the engine's Fmatrix.
struct RtView
{
	row_major float4x4	view;			// world -> view
	row_major float4x4	inv_view;		// view -> world
	row_major float4x4	vp;				// world -> clip, this frame, unjittered
	row_major float4x4	vp_prev;		// the same for the previous frame
	float4				pos_decompress;	// view ray of a render pixel: z * ( pos * zw - xy ), jitter folded in
	float4				proj;			// x projection _33, y _43, z tan( vertical fov / 2 ), w view z of the far plane
	float4				size;			// render width, height, 1 / width, 1 / height
	float4				camera;			// world position; w seconds
	float4				sun;			// xyz world direction toward the sun; w tan of the sun disc's radius
	uint4				scene;			// x static instances, y static texture base, z RT_VIEW_*, w frame index
	float4				frame;			// xy the projection jitter in render pixels (this frame's textures show a
										// point that far from its unjittered projection), z the alpha-test reference
};

// ---- depth and positions ---------------------------------------------------------------------

float rt_linear_z( float depth, RtView v )
{
	return v.proj.y / ( depth - v.proj.x );
}

bool rt_is_sky( float depth )
{
	return depth >= 0.999999f;
}

float3 rt_view_pos( float2 pixel_center, float z, RtView v )
{
	return float3( z * ( pixel_center * v.pos_decompress.zw - v.pos_decompress.xy ), z );
}

float3 rt_to_world( float3 p_view, RtView v )
{
	return mul( float4( p_view, 1.0f ), v.inv_view ).xyz;
}

float3 rt_dir_to_world( float3 d_view, RtView v )
{
	return mul( d_view, (float3x3)v.inv_view );
}

float3 rt_dir_to_view( float3 d_world, RtView v )
{
	return mul( d_world, (float3x3)v.view );
}

// Screen position of a world point through an unjittered view-projection; w = clip w (view z).
float3 rt_project( float3 p_world, row_major float4x4 m )
{
	const float4 c = mul( float4( p_world, 1.0f ), m );
	const float2 ndc = c.xy / max( c.w, 1e-5f );
	return float3( ndc * float2( 0.5f, -0.5f ) + 0.5f, c.w );
}

// Metres one render pixel spans at view depth z.
float rt_pixel_size( float z, RtView v )
{
	return 2.0f * v.proj.z * z * v.size.w;
}

// ---- the G-buffer ----------------------------------------------------------------------------

struct RtSurface
{
	float3	n_view;
	float	z;			// view z the G-buffer stored (fp16)
	float	hemi;
	float	ground;
};

RtSurface rt_surface( Texture2D<float4> position, Texture2D<float4> normal, int2 pixel, RtView v )
{
	RtSurface s;
	const float4 p = position.Load( int3( pixel, 0 ) );
	[branch] if ( v.scene.z & RT_VIEW_GBUFFER_PACKED )
	{
		s.n_view = gbuf_unpack_normal( p.xy );
		s.z = p.z;
		s.hemi = gbuf_unpack_hemi( p.w );
		s.ground = gbuf_unpack_ground( p.w );
	}
	else
	{
		const float4 n = normal.Load( int3( pixel, 0 ) );
		s.n_view = n.xyz;
		s.z = p.z;
		s.hemi = n.w;
		s.ground = p.w < 0.0f ? 1.0f : 0.0f;
	}
	s.n_view = normalize( s.n_view );
	return s;
}

// First-person hands and items are drawn with their own projection: their depth does not sit on
// the view ray the main projection describes. The G-buffer's own z and the depth buffer's then
// disagree, and such a pixel gets neither shadow nor occlusion from the scene.
bool rt_is_hud( float z_depth, RtSurface s )
{
	return s.z < 2.0f && abs( z_depth - s.z ) > 0.05f + 0.1f * s.z;
}

// ---- directions and noise --------------------------------------------------------------------

float2 rt_oct_wrap( float2 v )
{
	return ( 1.0f - abs( v.yx ) ) * select( v >= 0.0f, float2( 1.0f, 1.0f ), float2( -1.0f, -1.0f ) );
}

float2 rt_oct_encode( float3 n )
{
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	n.xy = n.z >= 0.0f ? n.xy : rt_oct_wrap( n.xy );
	return n.xy;
}

float3 rt_oct_decode( float2 f )
{
	float3 n = float3( f.x, f.y, 1.0f - abs( f.x ) - abs( f.y ) );
	const float t = saturate( -n.z );
	n.xy += select( n.xy >= 0.0f, float2( -t, -t ), float2( t, t ) );
	return normalize( n );
}

// Interleaved gradient noise (Jimenez 2014) on a pixel, stepped per frame by the R2 sequence.
float rt_ign( float2 pixel )
{
	return frac( 52.9829189f * frac( dot( pixel, float2( 0.06711056f, 0.00583715f ) ) ) );
}

float2 rt_noise2( uint2 pixel, uint frame )
{
	const float2 r2 = frac( float2( 0.7548776662f, 0.5698402910f ) * float( frame & 1023u ) );
	return frac( float2( rt_ign( float2( pixel ) ), rt_ign( float2( pixel.yx ) + 17.0f ) ) + r2 );
}

void rt_basis( float3 n, out float3 t, out float3 b )
{
	const float s = n.z >= 0.0f ? 1.0f : -1.0f;
	const float a = -1.0f / ( s + n.z );
	const float c = n.x * n.y * a;
	t = float3( 1.0f + s * n.x * n.x * a, s * c, -s * n.x );
	b = float3( c, s + n.y * n.y * a, -n.y );
}

float3 rt_cosine_dir( float3 n, float2 u )
{
	float3 t, b;
	rt_basis( n, t, b );
	const float r = sqrt( u.x );
	const float phi = 6.28318530718f * u.y;
	return normalize( t * ( r * cos( phi ) ) + b * ( r * sin( phi ) ) + n * sqrt( max( 1.0f - u.x, 0.0f ) ) );
}

// A direction inside a cone of half-angle tangent tan_r around d.
float3 rt_cone_dir( float3 d, float tan_r, float2 u )
{
	float3 t, b;
	rt_basis( d, t, b );
	const float r = tan_r * sqrt( u.x );
	const float phi = 6.28318530718f * u.y;
	return normalize( d + t * ( r * cos( phi ) ) + b * ( r * sin( phi ) ) );
}

#endif	//	rt_common_hlsli_included
