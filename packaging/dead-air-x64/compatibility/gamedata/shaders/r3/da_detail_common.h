#ifndef DA_DETAIL_COMMON_H
#define DA_DETAIL_COMMON_H

// The grass maths of the detail vertex shaders (deffer_detail_w_flat.vs, deffer_detail_s_flat.vs),
// shared with the DirectX 12 mesh-shader path, which computes the terms of a tuft once per tuft
// instead of once per vertex. Only functions: every constant arrives as a parameter, so an
// including shader keeps its own constant layout. Needs common.h first (v2p_flat, calc_xz_wave,
// the transforms); the waving-grass part needs da_wind_field.h first as well (the still grass does
// not include it: its constants must not move). Compiles under FXC and DXC (HLSL 2018 and 2021).
//
// A tuft is the three rows m0..m2 of its 3x4 transform (w = the root in world space, y = the
// height column, scaled by the fade) and its colour row c0 (x = sun, w = hemi / baked sky
// openness). FXC schedules in source order: the vertex shaders call these pieces in the order of
// their original code, which keeps their bytecode unchanged.
//
// The mesh-shader path is used only while both detail vertex shaders include this file: a
// replacement that brings its own wind keeps its look on the vertex path.

#include "da_wind_motors.h"
#include "da_snow_sample.h"

// The season of the stock detail classes (CDetailManager::StockSeasonConstants, docs/dead-air/GRASS.md):
// no CPU rebuild and no reload - an item is drawn iff the stable hash of its root is below its class's
// keep, its height column scales by the class's height, and it is buried where the snow depth reaches
// max(hide_abs, hide_rel x top - hide_margin). The vertex, mesh and amplification shaders all apply it,
// so the shadow passes follow for free; *_prev drive the motion vectors while a day glides.
uniform float4 stock_season;		// x keep, y height, z hide_rel, w hide_margin (this frame)
uniform float4 stock_season_prev;	// the same on the previous frame
uniform float4 stock_obj;			// x model top (model space y), y hide_abs, z 1 = snow lies, w 1 = the season applies

uint	stock_pcg	(uint v)
{
	v = v * 747796405u + 2891336453u;
	const uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
	return (w >> 22u) ^ w;
}

// Stable per item: the root is its translation, and x 64 is exact in float (the CPU twin is
// detail_stock_u in DetailManager.cpp).
float	stock_u	(float3 root)
{
	const int2 q = int2(floor(root.xz * 64.0f));
	return float(stock_pcg(asuint(q.x) ^ stock_pcg(asuint(q.y) + 0x9E3779B9u)) & 0xFFFFFFu) / 16777216.0f;
}

// The keep test alone (the amplification shader, before any snow is read).
bool	stock_season_kept	(float3 root, float4 s)
{
	return stock_obj.w < 0.5f || stock_u(root) < s.x;
}

// The factor of the height column (hs: its current length) under the season s; 0 = not drawn. prev reads
// the previous frame's snow.
float	stock_season_k	(float3 root, float hs, float4 s, bool prev)
{
	[branch] if (stock_obj.w < 0.5f)
		return 1.0f;
	if (stock_u(root) >= s.x)
		return 0.0f;
	[branch] if (stock_obj.z > 0.5f)
	{
		const float hide = max(stock_obj.y, s.z * stock_obj.x * hs * s.y - s.w);
		const float depth = prev ? da_snow_depth_prev(root.xz) : da_snow_depth(root.xz);
		if (depth >= hide)
			return 0.0f;
	}
	return s.y;
}

// Rows m0..m2 under the factor k: the height column scaled, or every vertex on the root (zero area,
// the rasteriser drops it) when the item is not drawn.
void	stock_season_rows	(inout float4 m0, inout float4 m1, inout float4 m2, float k)
{
	if (k <= 0.0f)
	{
		m0.xyz = 0.0f; m1.xyz = 0.0f; m2.xyz = 0.0f;
	}
	else
	{
		m0.y *= k; m1.y *= k; m2.y *= k;
	}
}

// Grass shadow fade band (sfade: x = start, y = end in metres, zero outside the sun shadow pass;
// eye: the CAMERA's world position - in the sun pass m_WV belongs to the sun). The shadow cull
// edge reads as a cone travelling with the player, so the caller fades before the cut by scaling
// the height column (m*.y) with this - the blade lies down, its shadow shortens and vanishes
// (opacity would need dithered alpha test, per-pixel work). Distance is taken from the instance
// ROOT so a blade never shrinks unevenly along its own height.
float	detail_sfade_k	(float3 root, float4 sfade, float3 eye)
{
	const float  d  = distance( root, eye );
	return saturate( ( sfade.y - d ) / max( sfade.y - sfade.x, 0.001f ) );
}

// A model-space vertex (w = 1) in world space.
float4	detail_world_pos	(float4 m0, float4 m1, float4 m2, float4 p)
{
	float4 	pos;
 	pos.x 		= dot	(m0, p);
 	pos.y 		= dot	(m1, p);
 	pos.z 		= dot	(m2, p);
	pos.w 		= 1;
	return	pos;
}

// Height above the root per unit of model height, measured along the instance basis rather than
// world Y minus base: with ground-tilted instances that picked up cos(tilt) and cross terms,
// weakening and skewing the sway on slopes. Inherits the shadow-fade scaling through m*.y.
float	detail_height_scale	(float4 m0, float4 m1, float4 m2)
{
	return	length(float3(m0.y, m1.y, m2.y));
}

float	detail_height	(float4 m0, float4 m1, float4 m2, float y)
{
	return	y * detail_height_scale(m0, m1, m2);
}

// Wind sheltering: the tuft's baked sky openness (c0.w, from the level lightmap) already encodes
// the room around it - bright near a doorway or a broken roof, dark in a corner. Scaling the sway
// by it gives draughts for free: grass by an opening stirs, grass deep inside stands still, and
// the gradient across a hangar follows the actual holes in it. The users add a small floor
// (0.05 + 0.95 x shelter) so sheltered air is never perfectly dead.
float	detail_shelter	(float hemi)
{
	return	saturate((hemi - 0.10f) * 1.8f);
}

// The wind terms of one tuft that no vertex changes.
struct detail_wind
{
	float	dp;			// sway value (detail_sway)
	float	shelter;	// detail_shelter
	float3	flow;		// travelling gust field at the root (da_wind_field_eval_at)
	float2	wdir;		// local wind heading
};

// The bend of one vertex from its wave intensity inten (H x sway x shelter floor x flow.x); H is
// the height above the root, frac the vertex's flexibility, prev picks the previous frame's motors.
float2	detail_bend_tail	(float3 root, detail_wind w, float H, float frac, float inten, bool prev)
{
	// Wind motors first: press motors also report their footprint weight, and grass held down
	// by a boot must not keep waving mid-air - the wave and the gust lean are suppressed
	// inside the press. Not scaled by shelter or wind: a boot presses grass in a windless
	// hangar just the same.
	float	press_w;
	float2	bend	= da_wind_motors_bend_ex(root, H, prev, press_w);
	const float wind_free = 1.0f - press_w;
	float2 	result	= calc_xz_wave	(w.wdir*inten*wind_free,frac);
	// Gust lean: inside a passing tongue the grass does not just wave harder - it lies DOWN
	// along the wind, and the front of that flattening visibly rolls across the meadow.
	// Scaled by height and shelter like the wave itself; the arc-length drop then pulls the tip
	// down instead of stretching the blade.
	result	+= w.wdir * (H * w.flow.y * (0.05f + 0.95f * w.shelter) * 1.4f * wind_free);
	result	+= bend;
	// A strong blast can ask for more bend than the blade has length - the arc-drop then pins
	// the tip to the ground and the excess would pure-STRETCH the blade. The cap is slightly
	// SHORT of lying flat: at the full length the tip sits exactly on the ground plane and
	// visibly buries itself on any slope or bumpy terrain.
	result	*= min(1.0f, 0.92f * H / max(length(result), 0.001f));
	return	result;
}

#ifdef DA_WIND_FIELD_H
// Wave shape: mean lean + smooth oscillation (da_sway), replacing the symmetric wave that swung
// blades as far upwind as downwind and ricocheted off its peaks. Real grass under wind holds a
// downwind lean proportional to the wind and oscillates around it: light air = shallow lean with
// wide swings almost back upright, steady strong wind = deep lean with only an elastic spring.
//
// The phase comes from the instance ROOT, not the vertex. The wave still travels across a meadow
// - the roots are metres apart - but no longer across ONE clump: the field runs a cycle every
// 19..44 m depending on the axis, nothing on a hand-high tuft and about 39 degrees across a
// two-metre stand of marsh reed. That spread is what made tall grass writhe instead of lean.
// field, wv (wave) and, below, dir (dir2D) pick the wind: this frame's, or the previous frame's
// for the motion vectors.
float	detail_sway	(float3 root, float4 field, float4 wv)
{
	const float wind_k = saturate(field.z);
	const float sway_mean = 0.42f + 0.38f * wind_k;
	return	sway_mean + (1.0f - sway_mean) * da_sway(dot(float4(root, 1.0f), wv));
}

// Local flow from the travelling gust field is evaluated at the TUFT ROOT so one tuft always moves
// as a whole: each tuft sways with the flow that is passing over IT right now. The field's z
// channel also turns the LOCAL wind heading - patches of the meadow genuinely blow different
// ways, and the swirl pattern travels downwind like a real eddy.
detail_wind	detail_wind_at	(float3 root, float hemi, float4 field, float4 wv, float4 dir)
{
	detail_wind	w;
	w.dp		= detail_sway(root, field, wv);
	w.shelter	= detail_shelter(hemi);
	w.flow		= da_wind_field_eval_at(root.xz, field);
	w.wdir		= da_wind_local_dir_at(dir.xz, w.flow.z, field.z);
	return	w;
}

// One vertex of a waving tuft from its precomputed terms (the mesh shader).
float2	detail_bend_v	(float3 root, detail_wind w, float H, float frac, bool prev)
{
	float 	inten 	= H * w.dp;
	inten	*= 0.05f + 0.95f * w.shelter;
	inten	*= w.flow.x;
	return	detail_bend_tail(root, w, H, frac, inten, prev);
}

// The same bend with the tuft's terms computed on the spot, in the vertex shader's order.
float2	detail_bend	(float3 root, float hemi, float H, float frac, float4 field, float4 wv, float4 dir, bool prev)
{
	detail_wind	w;
	w.dp		= detail_sway(root, field, wv);
	float 	inten 	= H * w.dp;
	w.shelter	= detail_shelter(hemi);
	inten	*= 0.05f + 0.95f * w.shelter;
	w.flow		= da_wind_field_eval_at(root.xz, field);
	w.wdir		= da_wind_local_dir_at(dir.xz, w.flow.z, field.z);
	inten	*= w.flow.x;
	return	detail_bend_tail(root, w, H, frac, inten, prev);
}
#endif

// Arc-length correction: the stock bend slides the tip sideways at constant height, stretching
// the blade up to +34% at storm amplitude (rubber-hose look). Dropping the tip to keep the
// length restores a bend.
float4	detail_bent_pos	(float4 pos, float H, float2 result)
{
	float	drop	= H - sqrt(max(H * H - dot(result, result), 0.0f));
	return	float4(pos.x+result.x, pos.y-drop, pos.z+result.y, 1);
}

// A vertex of a still tuft (rooted at root, H_s above it) bent by this frame's motors or, for
// the motion vectors, by the previous frame's: still details do not wave, but a boot or a
// blast still bends them.
float4	detail_motor_pos	(float4 pos, float3 root, float H_s, bool prev)
{
	float press_w;
	float2 mb = da_wind_motors_bend_ex(root, H_s, prev, press_w);
	// Cap short of lying flat - excess bend would pure-stretch the blade, and a tip on
	// the exact ground plane buries itself on slopes (see detail_bend_tail).
	mb *= min(1.0f, 0.92f * H_s / max(length(mb), 0.001f));
	// Same arc-length correction as the waving grass: a full-strength press LAYS the
	// tuft down instead of stretching it sideways.
	const float drop_s = H_s - sqrt(max(H_s * H_s - dot(mb, mb), 0.0f));
	pos.x += mb.x;
	pos.y -= drop_s;
	pos.z += mb.y;
	return pos;
}

// Ground-matched brightness noise -1..1 of a tuft (root_xz: its world XZ, inv_patch: 1 / patch
// size). Keyed to the WORLD position, so the pattern stays put as the camera moves, like real
// soil unevenness - screen-keyed noise would ride along and read as dirt on glass.
float	detail_tint_n	(float2 root_xz, float inv_patch)
{
	const float2 wxz = root_xz * inv_patch;
	const float2 c   = floor( wxz );
	const float2 f   = smoothstep( 0.0f, 1.0f, wxz - c );
	float4 h   = sin( float4(
		dot( c + float2(0,0), float2(127.1f, 311.7f) ),
		dot( c + float2(1,0), float2(127.1f, 311.7f) ),
		dot( c + float2(0,1), float2(127.1f, 311.7f) ),
		dot( c + float2(1,1), float2(127.1f, 311.7f) ) ) ) * 43758.5453f;
	h = h - floor( h );
	return lerp( lerp(h.x,h.y,f.x), lerp(h.z,h.w,f.x), f.y ) * 2.0f - 1.0f;
}

// The hemi of a vertex (y: its model height) varied by its tuft's noise n. We vary HEMI, not
// colour: hemi already flows to the (shared) pixel shader. Stronger near the BASE (tint.z): the
// stem takes soil properties at the roots and stays itself at the tip, killing the hard line
// where grass meets ground. tint: x = strength, y = 1/patch size, z = base boost.
float	detail_tint_hemi	(float hemi, float n, float4 tint, float y)
{
	const float up = saturate( y * 2.0f );
	const float k  = lerp( 1.0f + tint.z, 1.0f, up );
	return saturate( hemi * ( 1.0f + n * tint.x * k ) );
}

// The interpolants of a grass vertex (pos / pos_prev: world space, this and the previous frame;
// misc x qc: the texture coordinates; sun: c0.x). position.w (hemi) is the caller's.
v2p_flat	detail_v2p	(float3 root, float4 pos, float4 pos_prev, int4 misc, float4 qc, float sun)
{
	v2p_flat 		O;
	// Normal in world coords: from the root up through the vertex, lifted to avoid zero.
	float3 	norm;
		norm.x 	= pos.x - root.x	;
		norm.y 	= pos.y - root.y	+ .75f;
		norm.z	= pos.z - root.z	;

	float4	Pp 	= mul		(m_WVP,	pos				);
	O.hpos 		= Pp;
	O.hpos_cur	= mul		(m_WVP_nojit,	pos		);
	O.hpos_prev	= mul		(m_WVP_prev,	pos_prev	);
	O.N 		= mul		(m_WV,  normalize(norm)	);
	float3	Pe	= mul		(m_WV,  pos				);
	O.tcdh 		= float4	((misc * qc).xyyy );

# if defined(USE_R2_STATIC_SUN)
	O.tcdh.w	= sun;								// (,,,dir-occlusion)
# endif

	O.position	= float4	(Pe, 		0		);
	return	O;
}

#endif
