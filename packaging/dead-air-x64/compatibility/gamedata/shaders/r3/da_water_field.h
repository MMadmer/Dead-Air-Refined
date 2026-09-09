#ifndef DA_WATER_FIELD_H
#define DA_WATER_FIELD_H

//	The baked water field, and the ripple window that rides on top of it (DESIGN2.md 1 and 2).
//	This is the shared reader: every consumer - the surface, the underwater pass, the caustics,
//	the puddles - asks the same four questions here and gets the same answers, so they cannot
//	disagree about where the water is.
//
//	The field is baked once per level into a 1024x1024 RGBA16F map of the level's AABB:
//		R = water surface world Y
//		G = coverage, 1 where a liquid triangle covers this texel
//		B = bed world Y
//		A = metres to the nearest bank, clamped to 32
//	Nothing writes it per frame, so a read is a texture fetch and never a march.
//
//	The ripple field is the interactive half: a 32 m window around the camera, snapped to whole
//	texels, holding the surface the spectral solver steps (da_water_ripple.ps and the
//	da_ripple_*.cs passes: Tessendorf's eWave, every wavelength at its own speed). R = height
//	in METRES of surface displacement, G = its vertical velocity in m/s. It runs on every tier;
//	the preset only sets how many texels the 32 m are cut into.
//
//	Include AFTER common.h - the samplers come from there, as they do for da_clouds.h. Any .s
//	script whose shader includes this file has to bind the two textures:
//		shader:dx10texture ("s_water_field",  "$user$water_field")
//		shader:dx10texture ("s_water_ripple", "$user$water_ripple0")
//	That is ALWAYS the finished step - the solver's last pass is copied back into the name,
//	precisely so a reader can bind one name and be right (a ping-pong that alternated the name
//	would strobe the field at the step rate).

//	xy = the level AABB's min corner (world X, Z), zw = 1/extent, so
//	uv = (wp.xz - da_water_map.xy) * da_water_map.zw. All zero when the level has no field.
uniform float4 da_water_map;
//	x = metres per texel, y/z = the level's min/max Y, w = 1 when the field is valid.
uniform float4 da_water_map2;
//	xy = the ripple window's centre (world XZ, snapped), z = its size in metres, w = one texel
//	in uv (1/texels). All zero when the tier runs no field at all.
uniform float4 da_water_rip;

//	The absorbing band inside the window's rim, metres. The sim damps a wave out over it, every
//	reader lets go of the field over the same band, and the analytic rings fade in over it - so
//	the three agree by construction. The CPU twin is CEnvironment::water_ripple_edge.
#define DA_WF_RIM_M	4.0f

//	What a reader tilts its normal by, per unit of the field's real slope, and where that stops.
//	The field is physical - a pistol's ring is a centimetre high two metres out - and the eye
//	reads such a ring off the reflection of a structured world: a sky with a gradient, a
//	treeline, sun glitter. The game reflects a blurred cube and lights a bright bed through the
//	water, and against that a centimetre reads as nothing (it did: the rig showed the field a
//	perfect train and the frame nothing at all). Interaction ripples get their normals amplified
//	in every engine that draws them for the same reason; the limiter keeps a crest that would
//	fold the normal over as a crest, since the foam it would really break into is not drawn.
#define DA_WF_SLOPE_GAIN	3.5f
#define DA_WF_SLOPE_LIMIT	1.5f

Texture2D s_water_field;
Texture2D s_water_ripple;

//	---- The baked field -----------------------------------------------------------------------

float2 da_wf_uv(float2 wxz)
{
	return (wxz - da_water_map.xy) * da_water_map.zw;
}

//	The raw texel: x = surface Y, y = coverage, z = bed Y, w = shore distance.
//
//	Point-sampled on purpose. Coverage is a rasterised mask and bilinear across a bank invents
//	a half-covered texel where the bake says there is either water or ground; the CPU queries
//	(CEnvironment::water_at) take the nearest texel too, so the two sides agree by construction.
float4 da_wf_sample(float2 wxz)
{
	[branch]
	if (da_water_map2.w < 0.5f)
		return 0.0f;
	const float2 uv = da_wf_uv(wxz);
	//	Outside the level's footprint there is no field and no water. Zero rather than a clamp:
	//	clamping smears the edge texel over the whole world beyond the map.
	[branch]
	if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		return 0.0f;
	return s_water_field.SampleLevel(smp_nofilter, uv, 0);
}

//	Is there water at this XZ at all. Every other query is only meaningful where this is true.
bool da_wf_here(float2 wxz)
{
	return da_wf_sample(wxz).y > 0.5f;
}

//	World Y of the surface, or far below the world where there is none - mirroring the CPU's
//	-FLT_MAX, so "eye.y < da_wf_surface(p)" is false on dry land instead of accidentally true.
float da_wf_surface(float2 wxz)
{
	const float4 f = da_wf_sample(wxz);
	return f.y > 0.5f ? f.x : -1e6f;
}

//	How deep the water is HERE - a property of the place, view-independent, and the thing that
//	drives wave attenuation, foam and debris. Zero where there is no water.
float da_wf_depth(float2 wxz)
{
	const float4 f = da_wf_sample(wxz);
	return f.y > 0.5f ? max(f.x - f.z, 0.0f) : 0.0f;
}

//	Metres to the nearest bank, up to 32. Defined on dry texels too: "how far to the water" is
//	a question the bank side asks as well. Bilinear here and point everywhere else is not an
//	inconsistency - a distance transform is a smooth scalar field and interpolating it is exact
//	to the metre, while interpolating the mask it came from would not be.
float da_wf_shore(float2 wxz)
{
	[branch]
	if (da_water_map2.w < 0.5f)
		return 0.0f;
	const float2 uv = da_wf_uv(wxz);
	[branch]
	if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		return 0.0f;
	return s_water_field.SampleLevel(smp_rtlinear, uv, 0).w;
}

//	---- The ripple window ----------------------------------------------------------------------

float2 da_wf_ripple_uv(float2 wxz)
{
	return (wxz - da_water_rip.xy) / max(da_water_rip.z, 0.001f) + 0.5f;
}

float2 da_wf_ripple_grad(Texture2D field, float2 uv, float e)
{
	const float hx1 = field.SampleLevel(smp_rtlinear, uv + float2(e, 0.0f), 0).r;
	const float hx0 = field.SampleLevel(smp_rtlinear, uv - float2(e, 0.0f), 0).r;
	const float hz1 = field.SampleLevel(smp_rtlinear, uv + float2(0.0f, e), 0).r;
	const float hz0 = field.SampleLevel(smp_rtlinear, uv - float2(0.0f, e), 0).r;
	return float2(hx1 - hx0, hz1 - hz0);
}

//	The world-XZ slope of the ripple field, ready to be added to the surface's own wave slope.
//	Zero outside the window - and NOT the clamped edge texel, which would paint the window's
//	rim across every pond on the level.
float2 da_wf_ripple_slope(float2 wxz)
{
	[branch]
	if (da_water_rip.z <= 0.0f)
		return 0.0f;
	const float2 uv = da_wf_ripple_uv(wxz);
	[branch]
	if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		return 0.0f;

	const float e = da_water_rip.w;	// one texel, in uv
	//	Central difference over two texels, in metres; then the reader's gain and its limiter.
	float2 grad = da_wf_ripple_grad(s_water_ripple, uv, e) * (DA_WF_SLOPE_GAIN / (2.0f * e * da_water_rip.z));
	grad /= 1.0f + length(grad) * (1.0f / DA_WF_SLOPE_LIMIT);

	//	The sim damps hard over its outer metres, so the field is already near zero at the rim;
	//	this last fade only guarantees there is no step at the boundary when something large is
	//	still ringing as it crosses out of the window. Same metres as the sim's own absorbing
	//	band, so the surface lets go of the field over the stretch the field lets go of the wave.
	const float2 d = min(uv, 1.0f - uv) * da_water_rip.z;
	return grad * saturate(min(d.x, d.y) * (1.0f / DA_WF_RIM_M));
}

#endif	// DA_WATER_FIELD_H
