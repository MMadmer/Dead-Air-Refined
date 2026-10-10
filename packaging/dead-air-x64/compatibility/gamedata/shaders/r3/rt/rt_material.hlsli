#ifndef	rt_material_hlsli_included
#define	rt_material_hlsli_included

// Shading of a reflection hit the screen does not show (dxr.md 3.7): albedo, the vertex normal and
// hemisphere, the sun with a static shadow ray, the combine's hemisphere and ambient terms; then
// the combine's haze and its tonemap with this frame's exposure, so an off-screen hit lands in the
// same LDR space as the frame copy on-screen hits are taken from (combine_1.ps, hmodel.h,
// da_fog_common.h, common_functions.h tonemap/da_grade).

#include "rt_geometry.hlsli"

struct RtShade
{
	float4	sun_color;		// the sun light's colour (Ldynamic_color)
	float4	env_color;		// combine_1's env_color: rgb * 2 * r2_sun_lumscale_hemi, w = weather blend
	float4	ambient;		// combine_1's L_ambient: ambient * 2 (floored) * r2_sun_lumscale_amb
	float4	fog_params;		// x -near / ( far - near ), y near, z far, w 1 / ( far - near )
	float4	fog_color;
	float4	fog;			// da_fog: x sky share, y sky mip, z height density, w height falloff
	float4	fog2;			// da_fog2: x density ceiling, y layer base, z horizon flattening
	float4	sky_tint;		// da_sky_tint
	float4	rain2;			// da_rain2: y extinction per kilometre
	float4	tonemap;		// da_tonemap_params
	float4	grade;			// da_grade_params
};

// ---- the haze (da_fog_common.h, with the constants passed in) ---------------------------------

float rt_fog_rain( float linear_fog, float dist, RtShade s )
{
	[branch] if ( s.rain2.y <= 0.001f )
		return linear_fog;
	const float rainf = 1.0f - exp( -s.rain2.y * 0.001f * max( dist, 0.0f ) );
	return saturate( linear_fog + rainf * ( 1.0f - linear_fog ) );
}

float rt_fog_height( float linear_fog, float3 wp, float3 cam, float dist, RtShade s )
{
	linear_fog = rt_fog_rain( linear_fog, dist, s );
	[branch] if ( s.fog.z <= 0.001f )
		return linear_fog;
	const float b = max( s.fog.w, 0.0001f );
	const float ry = ( wp.y - cam.y ) / max( dist, 0.001f );
	const float base = exp( -clamp( cam.y - s.fog2.y, -500.0f, 500.0f ) * b );
	const float integ = ( abs( ry ) > 0.0001f ) ? base * ( 1.0f - exp( -dist * ry * b ) ) / ( ry * b ) : base * dist;
	const float fogH = 1.0f - exp( -s.fog.z * 0.001f * max( integ, 0.0f ) );
	return saturate( linear_fog + fogH * ( 1.0f - linear_fog ) );
}

float rt_fog_ceiling( float fog, RtShade s )
{
	[branch] if ( s.fog2.x > 0.001f && s.fog2.x < 0.999f )
		return min( fog, s.fog2.x );
	return fog;
}

bool rt_fog_extras_on( RtShade s )
{
	return s.fog.x > 0.001f || s.fog.z > 0.001f || s.rain2.y > 0.001f;
}

// combine_1: the fogged colour of a point at `wp` seen from `cam`.
float3 rt_fog_apply( float3 color, float3 wp, float3 cam, RtShade s, TextureCube sky0, TextureCube sky1 )
{
	const float dist = length( wp - cam );
	float fog = saturate( dist * s.fog_params.w + s.fog_params.x );
	float3 fog_rgb = s.fog_color.rgb;
	[branch] if ( rt_fog_extras_on( s ) )
	{
		fog = rt_fog_height( fog, wp, cam, dist, s );
		[branch] if ( s.fog.x > 0.001f )
		{
			float3 vd = normalize( wp - cam );
			vd.y *= saturate( 1.0f - s.fog2.z );
			vd = normalize( vd );
			const float3 s0 = sky0.SampleLevel( rt_linear_clamp, vd, s.fog.y ).rgb;
			const float3 s1 = sky1.SampleLevel( rt_linear_clamp, vd, s.fog.y ).rgb;
			const float3 sky = s.sky_tint.rgb * lerp( s0, s1, s.env_color.w ) * 0.66f;
			fog_rgb = lerp( fog_rgb, sky, saturate( s.fog.x ) );
		}
	}
	fog = rt_fog_ceiling( fog, s );
	return lerp( color, fog_rgb, fog );
}

// ---- the tonemap (common_functions.h) ---------------------------------------------------------

float3 rt_grade( float3 c, RtShade s )
{
	[branch] if ( s.grade.x < 0.001f )
		return c;
	const float3 LUM = float3( 0.2126f, 0.7152f, 0.0722f );
	const float g = saturate( ( c.g - max( c.r, c.b ) ) / max( c.g, 1e-4f ) );
	c.r = lerp( c.r, c.g, s.grade.z * g );
	const float l = dot( c, LUM );
	const float k = s.grade.x * lerp( 1.0f, s.grade.y, g );
	c = lerp( l.xxx, c, k );
	return max( 0.0f, ( c - 0.18f ) * s.grade.w + 0.18f );
}

float3 rt_tonemap( float3 rgb, float scale, RtShade s )
{
	rgb *= scale;
	const float white = ( s.tonemap.y > 0.01f ) ? s.tonemap.y : 1.7f;
	const float white2 = white * white;
	float3 tm = ( rgb * ( 1.0f + rgb / white2 ) ) / ( rgb + 1.0f );
	[branch] if ( s.tonemap.z > 0.001f )
	{
		const float3 LUM = float3( 0.2126f, 0.7152f, 0.0722f );
		const float l = dot( rgb, LUM );
		const float lt = ( l * ( 1.0f + l / white2 ) ) / ( l + 1.0f );
		float3 hue = rgb * ( lt / max( l, 1e-4f ) );
		hue = lerp( hue, lt.xxx, pow( saturate( lt ), max( s.tonemap.w, 1.0f ) ) );
		tm = lerp( tm, hue, saturate( s.tonemap.z ) );
	}
	return rt_grade( tm, s );
}

// ---- the hit ---------------------------------------------------------------------------------

struct RtHitSurface
{
	float3	albedo;
	float3	normal;		// world, facing the ray
	float	hemi;
	bool	emissive;
};

RtHitSurface rt_hit_surface( RtGeometry g, RtInstance inst, RtTriangle t, float2 bary, float3x4 o2w, float3 ray_dir,
	float cone_w, RtView v )
{
	const float3 w = rt_bary3( bary );
	RtHitSurface h;
	const float3 n_obj = t.v0.normal * w.x + t.v1.normal * w.y + t.v2.normal * w.z;
	float3 n = mul( (float3x3)o2w, n_obj );
	const float3 face = cross( mul( (float3x3)o2w, t.v1.position - t.v0.position ), mul( (float3x3)o2w, t.v2.position - t.v0.position ) );
	n = dot( n, n ) > 1e-8f ? normalize( n ) : normalize( face );
	// Two-sided: the side the ray arrived on.
	if ( dot( n, ray_dir ) > 0.0f )
		n = -n;
	h.normal = n;

	const float vertex_hemi = t.v0.hemi * w.x + t.v1.hemi * w.y + t.v2.hemi * w.z;
	h.hemi = saturate( vertex_hemi * inst.hemi_scale + inst.hemi_bias );

	h.albedo = 0.5f;
	const uint tex = rt_texture_index( g, v );
	[branch] if ( tex != 0xFFFFFFFFu )
	{
		const float2 tc = t.v0.tc * w.x + t.v1.tc * w.y + t.v2.tc * w.z;
		uint tw, th, levels;
		rt_textures[ NonUniformResourceIndex( tex ) ].GetDimensions( 0, tw, th, levels );
		const float lod = min( rt_cone_lod( t, o2w, cone_w, uint2( tw, th ) ), float( levels ) - 1.0f );
		h.albedo = rt_textures[ NonUniformResourceIndex( tex ) ].SampleLevel( rt_linear_wrap, tc, lod ).rgb;
		// A stand-in crown: its species' leaf colour of the day over the card's own detail (the card over its mean).
		[branch] if ( rt_veg_on( inst, v ) )
		{
			const float3 mean = rt_textures[ NonUniformResourceIndex( tex ) ].SampleLevel( rt_linear_wrap, tc, float( levels ) - 1.0f ).rgb;
			h.albedo = rt_veg_colour( inst, v ) * h.albedo / max( mean, 0.02f );
		}
	}
	// The snow on the trees' upper faces, as the procedural crowns hold it.
	const float snow = asfloat( v.veg.w );
	[branch] if ( ( inst.flags & RT_INST_TREE ) != 0u && snow > 0.0f )
		h.albedo = lerp( h.albedo, 0.85f, snow * saturate( h.normal.y ) );
	h.emissive = ( g.flags & RT_GEOM_EMISSIVE ) != 0u;
	return h;
}

// The lit, unfogged colour of the hit: sun by the static shadow ray, the combine's hemisphere
// model (hmodel.h without the material table) and ambient.
float3 rt_hit_light( RtHitSurface h, float sun_visibility, RtShade s, TextureCube env0, TextureCube env1, RtView v )
{
	const float3 sun = s.sun_color.rgb * saturate( dot( h.normal, v.sun.xyz ) ) * sun_visibility;
	const float3 e0 = env0.SampleLevel( rt_linear_clamp, h.normal, 0 ).rgb;
	const float3 e1 = env1.SampleLevel( rt_linear_clamp, h.normal, 0 ).rgb;
	float3 env_d = s.env_color.rgb * lerp( e0, e1, s.env_color.w );
	env_d *= env_d;
	const float3 hdiffuse = env_d * h.hemi + s.ambient.rgb;
	float3 color = h.albedo * ( sun + hdiffuse );
	if ( h.emissive )
		color += h.albedo;
	return color;
}

#endif	//	rt_material_hlsli_included
