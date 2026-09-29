#ifndef	rt_geometry_hlsli_included
#define	rt_geometry_hlsli_included

// What a ray hit: the geometry and instance records the scene writes (da_rt_scene.cpp,
// DaRtGeometryRecord / DaRtInstanceRecord), the vertex decode of the engine's vertex formats and the
// alpha test of cut-out surfaces.

#include "rt_common.hlsli"

// One mesh of a bottom-level structure, 32 bytes. Indexed by InstanceID() + GeometryIndex(); an
// InstanceID with RT_INSTANCE_DYNAMIC set reads the per-frame table.
struct RtGeometry
{
	uint	vb;			// bindless raw buffer of the vertices
	uint	vb_offset;	// byte offset of vertex 0
	uint	ib;			// bindless raw buffer of the 16-bit indices
	uint	ib_offset;	// byte offset of the first index
	uint	layout;		// stride | normal offset << 8 | texcoord offset << 16 | texcoord kind << 24 | normal kind << 28
	uint	layout2;	// tangent offset | binormal offset << 8 (255 = none)
	uint	albedo;		// texture: static-range index, or absolute with RT_GEOM_ABSOLUTE; ~0 = none
	uint	flags;		// RT_GEOM_*
};

// One instance of the acceleration structure, 16 bytes, by InstanceIndex(): below the static count
// the level's table, above it the per-frame table.
struct RtInstance
{
	float	root_y;		// a tree's root height (the trunk rule of the occlusion rays)
	float	hemi_scale;	// hemisphere light: vertex value * scale + bias
	float	hemi_bias;
	uint	flags;		// RT_INST_*
};

#define RT_INSTANCE_DYNAMIC		0x800000u	// InstanceID bit: the geometry record is in the per-frame table

#define RT_GEOM_ALPHA			0x1u		// alpha-tested against the albedo's alpha
#define RT_GEOM_EMISSIVE		0x2u		// self-lit (lamps, screens)
#define RT_GEOM_ABSOLUTE		0x4u		// albedo is an absolute bindless index

#define RT_INST_TRUNK			0x1u		// accepted by occlusion rays only near the root
#define RT_INST_TREE			0x2u		// hemisphere from c_scale / c_bias

#define RT_TC_NONE				0u
#define RT_TC_SHORT2_FRAC		1u			// level: ( s + T.w, B.w ) * 32 / 32768
#define RT_TC_SHORT4_TREE		2u			// multiple-use models: s * 1 / 2048
#define RT_TC_FLOAT2			3u
#define RT_TC_SHORT2			4u			// s * 32 / 32768
#define RT_N_NONE				0u
#define RT_N_COLOR				1u			// D3DCOLOR, bgra, bx2; alpha = hemisphere
#define RT_N_FLOAT3				2u
#define RT_N_OCT16				3u			// two snorm16 octahedral (the scene's own vertices)

StructuredBuffer<RtGeometry>	rt_geometry			: register( t0, space3 );
StructuredBuffer<RtInstance>	rt_instances		: register( t1, space3 );
StructuredBuffer<RtGeometry>	rt_geometry_dyn		: register( t2, space3 );
StructuredBuffer<RtInstance>	rt_instances_dyn	: register( t3, space3 );

RtGeometry rt_geometry_of( uint instance_id, uint geometry_index )
{
	[branch] if ( instance_id & RT_INSTANCE_DYNAMIC )
		return rt_geometry_dyn[ ( instance_id & ~RT_INSTANCE_DYNAMIC ) + geometry_index ];
	return rt_geometry[ instance_id + geometry_index ];
}

RtInstance rt_instance_of( uint instance_index, RtView v )
{
	[branch] if ( instance_index >= v.scene.x )
		return rt_instances_dyn[ instance_index - v.scene.x ];
	return rt_instances[ instance_index ];
}

uint rt_texture_index( RtGeometry g, RtView v )
{
	if ( g.albedo == 0xFFFFFFFFu )
		return 0xFFFFFFFFu;
	return ( g.flags & RT_GEOM_ABSOLUTE ) ? g.albedo : v.scene.y + g.albedo;
}

// ---- vertices --------------------------------------------------------------------------------

uint rt_load_u16( ByteAddressBuffer b, uint offset )
{
	const uint w = b.Load( offset & ~3u );
	return ( offset & 2u ) ? ( w >> 16 ) : ( w & 0xFFFFu );
}

uint3 rt_triangle( RtGeometry g, uint primitive )
{
	ByteAddressBuffer ib = rt_buffers[ NonUniformResourceIndex( g.ib ) ];
	const uint o = g.ib_offset + primitive * 6u;
	return uint3( rt_load_u16( ib, o ), rt_load_u16( ib, o + 2u ), rt_load_u16( ib, o + 4u ) );
}

int2 rt_short2( uint w )
{
	return int2( int( w << 16 ) >> 16, int( w ) >> 16 );
}

struct RtVertex
{
	float3	position;
	float3	normal;
	float2	tc;
	float	hemi;
};

RtVertex rt_vertex( RtGeometry g, uint index )
{
	ByteAddressBuffer vb = rt_buffers[ NonUniformResourceIndex( g.vb ) ];
	const uint stride = g.layout & 0xFFu;
	const uint base = g.vb_offset + index * stride;
	const uint n_off = ( g.layout >> 8 ) & 0xFFu;
	const uint tc_off = ( g.layout >> 16 ) & 0xFFu;
	const uint tc_kind = ( g.layout >> 24 ) & 0xFu;
	const uint n_kind = ( g.layout >> 28 ) & 0xFu;

	RtVertex o;
	o.position = asfloat( vb.Load3( base ) );
	o.normal = float3( 0.0f, 1.0f, 0.0f );
	o.hemi = 1.0f;
	o.tc = 0.0f;

	[branch] if ( n_kind == RT_N_COLOR )
	{
		const uint c = vb.Load( base + n_off );
		o.normal = float3( ( c >> 16 ) & 0xFFu, ( c >> 8 ) & 0xFFu, c & 0xFFu ) * ( 2.0f / 255.0f ) - 1.0f;
		o.hemi = float( c >> 24 ) * ( 1.0f / 255.0f );
	}
	else if ( n_kind == RT_N_FLOAT3 )
		o.normal = asfloat( vb.Load3( base + n_off ) );
	else if ( n_kind == RT_N_OCT16 )
	{
		const uint c = vb.Load( base + n_off );
		o.normal = rt_oct_decode( float2( rt_short2( c ) ) * ( 1.0f / 32767.0f ) );
	}

	[branch] if ( tc_kind == RT_TC_SHORT2_FRAC )
	{
		const float2 s = float2( rt_short2( vb.Load( base + tc_off ) ) );
		const uint t_off = g.layout2 & 0xFFu;
		const uint b_off = ( g.layout2 >> 8 ) & 0xFFu;
		const float du = t_off != 0xFFu ? float( vb.Load( base + t_off ) >> 24 ) * ( 1.0f / 255.0f ) : 0.0f;
		const float dv = b_off != 0xFFu ? float( vb.Load( base + b_off ) >> 24 ) * ( 1.0f / 255.0f ) : 0.0f;
		o.tc = ( s + float2( du, dv ) ) * ( 32.0f / 32768.0f );
	}
	else if ( tc_kind == RT_TC_SHORT4_TREE )
		o.tc = float2( rt_short2( vb.Load( base + tc_off ) ) ) * ( 1.0f / 2048.0f );
	else if ( tc_kind == RT_TC_FLOAT2 )
		o.tc = asfloat( vb.Load2( base + tc_off ) );
	else if ( tc_kind == RT_TC_SHORT2 )
		o.tc = float2( rt_short2( vb.Load( base + tc_off ) ) ) * ( 32.0f / 32768.0f );
	return o;
}

float3 rt_bary3( float2 bary )
{
	return float3( 1.0f - bary.x - bary.y, bary.x, bary.y );
}

struct RtTriangle
{
	RtVertex	v0, v1, v2;
};

RtTriangle rt_fetch( RtGeometry g, uint primitive )
{
	const uint3 i = rt_triangle( g, primitive );
	RtTriangle t;
	t.v0 = rt_vertex( g, i.x );
	t.v1 = rt_vertex( g, i.y );
	t.v2 = rt_vertex( g, i.z );
	return t;
}

// ---- texture level of detail (ray cones) ------------------------------------------------------

// The mip a cone of spread `cone_w` metres at the hit covers on this triangle's texture.
float rt_cone_lod( RtTriangle t, float3x4 o2w, float cone_w, uint2 tex_size )
{
	const float3 e1 = mul( (float3x3)o2w, t.v1.position - t.v0.position );
	const float3 e2 = mul( (float3x3)o2w, t.v2.position - t.v0.position );
	const float world_area = length( cross( e1, e2 ) );
	const float2 u1 = t.v1.tc - t.v0.tc;
	const float2 u2 = t.v2.tc - t.v0.tc;
	const float uv_area = abs( u1.x * u2.y - u1.y * u2.x ) * float( tex_size.x ) * float( tex_size.y );
	const float texels_per_metre = sqrt( uv_area / max( world_area, 1e-8f ) );
	return max( log2( max( cone_w * texels_per_metre, 1e-6f ) ), 0.0f );
}

// ---- the alpha test --------------------------------------------------------------------------

// A candidate on a non-opaque triangle: true when the texel there is solid against the reference the
// G-buffer clips with (def_aref = da_aref_u, which the presets move). Opaque geometry that reaches the
// loop (trunks, forced non-opaque for the occlusion rule) always is. A cut-out whose mask has no
// texture here is not there at all: taken as solid it would be a whole card, a flat grey rectangle.
bool rt_alpha_solid( uint instance_id, uint geometry_index, uint primitive, float2 bary, float3x4 o2w, float cone_w, RtView v )
{
	const RtGeometry g = rt_geometry_of( instance_id, geometry_index );
	[branch] if ( !( g.flags & RT_GEOM_ALPHA ) )
		return true;
	const uint tex = rt_texture_index( g, v );
	[branch] if ( tex == 0xFFFFFFFFu )
		return false;
	const RtTriangle t = rt_fetch( g, primitive );
	const float3 w = rt_bary3( bary );
	const float2 tc = t.v0.tc * w.x + t.v1.tc * w.y + t.v2.tc * w.z;
	uint tw, th, levels;
	rt_textures[ NonUniformResourceIndex( tex ) ].GetDimensions( 0, tw, th, levels );
	const float lod = min( rt_cone_lod( t, o2w, cone_w, uint2( tw, th ) ), float( levels ) - 1.0f );
	return rt_textures[ NonUniformResourceIndex( tex ) ].SampleLevel( rt_linear_wrap, tc, lod ).a >= v.frame.z;
}

#endif	//	rt_geometry_hlsli_included
