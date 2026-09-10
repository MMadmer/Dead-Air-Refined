#ifndef DA_VISOR_H
#define DA_VISOR_H

//	The water standing on the actor's visor, drawn. The field it reads is solved elsewhere
//	(da_visor_drops.ps, one fixed step per 1/30 s into $user$visor_drops0); this is the optics of
//	looking through it.
//
//	A drop on a plate two centimetres from the eye is a LENS, and that one fact decides most of
//	what is below:
//
//	  * it INVERTS. Trace the sight line backwards and it leaves the drop through a curved
//	    water-air interface; a spherical cap of radius r and height r/2 reaches forty-five degrees
//	    of tilt at its rim, and a ray bent that far crosses the axis. Every photograph of rain on
//	    a window shows the world upside down inside the drops, and no amount of "pinch the UV
//	    toward the drop centre" - which is what the effect this replaces did - can produce it,
//	    because a pinch is a magnifier and a magnifier does not cross the axis.
//	  * its RIM GOES DARK. That same interface is water on the inside and air on the outside, so
//	    past the critical angle - 48.6 degrees, which the rim of a real drop reaches - there is no
//	    refracted ray at all. HLSL's refract returns zero there, and the dark ring every drop
//	    carries is that zero.
//	  * it is BRIGHTER than what it covers. Garg and Nayar measured a raindrop's field of view at
//	    about 165 degrees and its transmission at 94 per cent: the drop gathers a whole
//	    hemisphere - the sky included - into the solid angle it hides. That is why a drop on a
//	    window reads as a bright bead against a dark street and not as a dark blob, and it is the
//	    other half of what the effect this replaces got wrong.
//	  * it CATCHES THE SUN. A drop is a tiny curved mirror as well as a lens, and the pinpoint
//	    glint is most of what makes a wet visor read as wet rather than as a dirty texture.
//	  * it is OUT OF FOCUS, and badly. The blur circle of something at the visor subtends
//	    D_pupil / distance, and the drop subtends d_drop / distance - the distance cancels, so a
//	    drop is resolvable only when it is WIDER THAN THE PUPIL, four millimetres or so. Ordinary
//	    visor drops are one to three, which is why a rainy visor in a photograph is a field of
//	    soft bright blobs and only the big merged runners have any structure at all. The physical
//	    blur is about a seventh of the screen's height; a third of that is taken here, because
//	    the honest number erases the effect the player asked to see.
//
//	The film - what a trail or a hand leaves behind - is not a lens. It is a few microns of water
//	with a shape, so it bends the sight line a little and scatters a little, and that is all: a
//	wiped visor is not clean glass and must not read as one.

Texture2D s_visor;

//	x = 1 when the field is live at all, y = millimetres of glass one texel covers, zw = one texel
//	in uv.
uniform float4 da_visor;
//	xy = the projection's scales (m_P._11, m_P._22), so a refracted direction can be turned back
//	into a screen position; z = how strong the whole thing is, w = spare.
uniform float4 da_visor2;

//	Water against air, from the inside: the ratio the exit refraction uses, and the reflectance
//	at normal incidence that goes with it.
#define DA_VS_ETA		1.333f
#define DA_VS_F0		0.0204f
//	Total internal reflection, as a straight line. Past the critical angle - asin(1/1.333) =
//	48.61 degrees, so the surface tilted that far from the eye - nothing gets out. cos of it is
//	0.6612, and You et al. (2016) note that the transmission near it linearises to 7.68 times
//	the excess, which is two constants and a saturate for the dark ring every drop carries.
#define DA_VS_TIR_COS	0.6612f
#define DA_VS_TIR_K		7.68f
//	What a drop gathers that the pixel behind it does not: a whole hemisphere, most of it sky.
//	Not a look knob - it is why drops are bright - but the amount is one, since the pass has no
//	sky probe to integrate and takes the frame's own upper half as a stand-in.
#define DA_VS_LIFT		0.45f
//	The thickness at which a drop is opaque enough to have its own optics, millimetres, and the
//	one where it starts to.
#define DA_VS_ON		0.035f
#define DA_VS_FULL		0.160f
//	The disc the drop's image is gathered over, in uv. See the note at the top: the physical
//	figure is around 0.14 of the screen's height and this is a third of it.
#define DA_VS_BLUR		0.045f
//	How much the film bends and scatters, against a drop.
#define DA_VS_FILM_BEND	0.60f
#define DA_VS_FILM_HAZE	0.30f

float4 da_visor_read(float2 uv)
{
	return s_visor.SampleLevel(smp_rtlinear, uv, 0);
}

//	The scene as the drop gathers it: a small disc rather than a point, because the eye cannot
//	focus this close. Four taps in a rotated square, which at this blur radius is enough - the
//	thing being blurred is already an inverted, heavily distorted image.
float3 da_visor_gather(Texture2D img, float2 uv, float r)
{
	const float2 a = float2(0.7071f, 0.7071f) * r;
	const float2 b = float2(0.7071f, -0.7071f) * r;
	float3 c = img.SampleLevel(smp_rtlinear, uv, 0).rgb;
	c += img.SampleLevel(smp_rtlinear, uv + a, 0).rgb;
	c += img.SampleLevel(smp_rtlinear, uv - a, 0).rgb;
	c += img.SampleLevel(smp_rtlinear, uv + b, 0).rgb;
	c += img.SampleLevel(smp_rtlinear, uv - b, 0).rgb;
	return c * 0.2f;
}

//	scene is what the frame holds at this pixel; the return is what it holds seen through whatever
//	water is standing on the glass in front of it.
float3 da_visor_water(Texture2D img, float2 uv, float3 scene)
{
	[branch]
	if (da_visor.x < 0.5f)
		return scene;

	const float4 st = da_visor_read(uv);
	const float h = st.x;
	const float film = st.y;
	[branch]
	if (h + film < 0.004f)
		return scene;

	//	The surface: the drop's own thickness plus half of the film, differenced over two texels
	//	and divided by the millimetres they span, so the gradient is a real slope and not a
	//	number that changes with the preset's grid.
	const float2 e = da_visor.zw;
	const float inv2mm = 1.0f / (2.0f * da_visor.y);
	const float4 tx1 = da_visor_read(uv + float2(e.x, 0.0f));
	const float4 tx0 = da_visor_read(uv - float2(e.x, 0.0f));
	const float4 ty1 = da_visor_read(uv + float2(0.0f, e.y));
	const float4 ty0 = da_visor_read(uv - float2(0.0f, e.y));
	const float sx1 = tx1.x + tx1.y * 0.5f, sx0 = tx0.x + tx0.y * 0.5f;
	const float sy1 = ty1.x + ty1.y * 0.5f, sy0 = ty0.x + ty0.y * 0.5f;
	//	uv.y runs down the screen and the eye's y runs up it, hence the sign on the second.
	const float2 slope = float2(-(sx1 - sx0) * inv2mm, (sy1 - sy0) * inv2mm);

	//	The sight line through this pixel, in eye space, and the drop's normal facing back along
	//	it. The glass is the screen plane, so the flat normal is exactly -z.
	const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
	const float3 vdir = normalize(float3(ndc.x / max(da_visor2.x, 1e-4f), ndc.y / max(da_visor2.y, 1e-4f), 1.0f));
	const float3 N = normalize(float3(slope.x, slope.y, -1.0f));

	//	Out of the water and into the air, and how much of the light makes it: the straight line
	//	through the critical angle. The drop's rim tilts past it and goes dark - that ring is not
	//	a drawn outline, it is the light that never left the water.
	const float3 R = refract(vdir, N, DA_VS_ETA);
	const float trans = saturate(DA_VS_TIR_K * (abs(N.z) - DA_VS_TIR_COS));
	const float tir = (dot(R, R) < 1e-6f || R.z <= 0.01f) ? 1.0f : (1.0f - trans);

	float3 drop;
	{
		//	Back to a screen position. The far scene is what matters, so the direction alone
		//	decides where to look - a drop this close to the eye has no parallax worth the name.
		const float2 rndc = float2(R.x / max(R.z, 1e-4f) * da_visor2.x, R.y / max(R.z, 1e-4f) * da_visor2.y);
		float2 ruv = float2(rndc.x * 0.5f + 0.5f, 0.5f - rndc.y * 0.5f);
		//	A sight line bent off the edge of the frame has nothing to show; fold it back rather
		//	than clamping, which would smear one edge texel across every steep drop rim.
		ruv = abs(frac(ruv * 0.5f) * 2.0f - 1.0f);

		//	Bigger drops hold their image better; the small ones are pure blur. The thickness is
		//	the only size this pass has, and it is the right one - a cap's height goes with its
		//	radius.
		const float big = saturate(h * 1.2f);
		drop = da_visor_gather(img, ruv, DA_VS_BLUR * (1.0f - 0.55f * big));

		//	Fresnel at the same interface, and the sun on the drop's own curve. The glint is a
		//	specular lobe of a very small roughness: a drop is smooth.
		const float ct = saturate(-dot(vdir, N));
		const float f = DA_VS_F0 + (1.0f - DA_VS_F0) * pow(1.0f - ct, 5.0f);
		const float3 L = -normalize(L_sun_dir_e.xyz);
		const float3 H = normalize(L - vdir);
		const float spec = pow(saturate(dot(N, -H)), 220.0f);
		//	The drop is brighter than what it covers because it gathers a hemisphere into the
		//	solid angle it hides. With no sky probe in this pass, the frame's own top edge is the
		//	stand-in - it is where the sky is - and the lift is scaled by how much of it a drop of
		//	this size can actually gather.
		const float3 sky = img.SampleLevel(smp_rtlinear, float2(uv.x, 0.06f), 0).rgb;
		drop = lerp(drop, max(drop, sky), DA_VS_LIFT);
		//	Past the critical angle the sight line never leaves the water, and the rim goes dark;
		//	the Fresnel sliver on top of it is the sky reflected off the drop's own curve.
		//	Not to black: what is trapped inside is the drop's own interior, lit by everything
		//	else that got in, and a real drop's dark ring is a grey one.
		drop = lerp(drop * 0.65f, drop, saturate(1.0f - tir));
		drop = lerp(drop, sky, f * 0.5f);
		drop += L_sun_color.rgb * (spec * 2.2f * saturate(1.0f - tir));
	}

	//	The film. Not a lens - a few microns with a shape - so it bends the sight line by a
	//	fraction of what a drop does and hazes what it lets through.
	float3 wiped = scene;
	[branch]
	if (film > 0.0005f)
	{
		const float2 foff = slope * (DA_VS_FILM_BEND * 0.004f);
		const float3 through = da_visor_gather(img, uv + foff, 0.0015f);
		const float haze = saturate(film / 0.06f) * DA_VS_FILM_HAZE;
		const float lum = dot(through, float3(0.30f, 0.59f, 0.11f));
		wiped = lerp(through, lerp(through, (float3)lum, 0.35f) + 0.02f, haze);
	}

	const float cov = smoothstep(DA_VS_ON, DA_VS_FULL, h) * saturate(da_visor2.z);
	return lerp(wiped, drop, cov);
}

#endif	// DA_VISOR_H
