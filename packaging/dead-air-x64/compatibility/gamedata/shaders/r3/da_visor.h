#ifndef DA_VISOR_H
#define DA_VISOR_H

//	The water standing on the actor's visor, drawn. The field it reads is solved elsewhere
//	(da_visor_drops.ps, a fixed step into $user$visor_drops0); this is the optics of looking
//	through it - from the INSIDE of the glass, which decides more than it seems to.
//
//	A drop on a plate two centimetres from the eye is a LENS, and everything below follows from
//	tracing the sight line out through it:
//
//	  * it INVERTS. The line leaves the drop through a curved water-air surface and bends away
//	    from the normal as it goes into the thinner medium, so at the drop's bottom rim, where the
//	    surface tilts down, the line is thrown UP, and at the top rim it is thrown down. The
//	    bottom of every drop looks at the sky and the top at the ground: an upside-down world in
//	    each, which is what every photograph of rain on a window shows, and the contrast between
//	    those two halves is most of what makes a drop read as a volume rather than a stain.
//	  * how far it bends is bounded by the CONTACT ANGLE. Water meets glass at forty-odd degrees
//	    at most, and a line leaving through a face tilted that far is turned by about twenty. The
//	    field's own cells are steeper than that - a bead a few cells wide has edges a cell wide -
//	    so the surface the eye is given is the field smoothed over a bead's footprint with its
//	    slope capped at the contact angle. Read raw, every bead was rim: past the critical angle
//	    everywhere but its middle pixel, and so a flat dark blot with no image in it at all.
//	  * its RIM IS DARK, and thin. The exit surface reflects part of the sight line back into the
//	    mask - Fresnel, rising steeply toward the critical angle at the contact line - and what it
//	    reflects is the inside of a helmet, which is dark. It is a line and not a band, so it is
//	    taken from the field's own steep edge, one texel wide, and not from the smoothed surface.
//	  * there is no highlight to MIRROR. From inside, nothing bright is on this side of the glass
//	    to be reflected in the drop's curve. What a photograph from inside shows as a bright arc
//	    is the refracted sky, brightest where the drop bends the view toward the sun behind the
//	    cloud - so the sun is looked for along the REFRACTED line, not in a specular lobe.
//	  * it is BRIGHTER than what it covers, a little. Garg and Nayar: a drop gathers about 165
//	    degrees of the world into the solid angle it hides, most of it sky.
//	  * it is OUT OF FOCUS - the eye cannot focus at two centimetres - but taken honestly that
//	    erases the effect; a small blur softens what is seen through the drop and no more.
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

//	Water against air, from the inside: the ratio the exit refraction uses.
#define DA_VS_ETA		1.333f
//	The steepest the water's surface is allowed to be, as a slope: tan of the contact angle.
//	Forty-six degrees - dirty glass, the advancing edge - and just under the critical angle for a
//	sight line square to the glass, so a drop looked at straight on has no dead ring, only the
//	thin Fresnel line at its contact line. Toward the edges of the screen the sight line itself
//	tilts and the far side of a drop can still go past it, which is real.
#define DA_VS_SLOPE_MAX	1.035f
//	The footprint the eye's surface is read over: a ring of eight at this radius and a cross of
//	four at half of it, millimetres. About a small bead: enough to turn the field's cell-wide
//	edges into a cap and its corners into curves, not enough to merge neighbours that do not
//	touch. Round, because a square stencil at this size drew the grid's own axes into every drop.
#define DA_VS_SMOOTH_MM	0.60f
//	The contact line, where water meets glass: how dark it is drawn, and the thickness under
//	which glass counts as dry. It is not a Fresnel term. The meniscus at a contact line is far
//	under a texel and steeper than anything the field holds, and every photograph of a drop has
//	the thin dark line whatever the drop's angle - so it is found as the EDGE OF THE WET MASK, the
//	line where water becomes glass, and drawn as one - and only within the band where the
//	SMOOTHED coverage is itself crossing, so that a running track thin enough to flicker across
//	the mask threshold inside is not hatched with false edges. Found as a step in thickness, it
//	fired on every ripple inside a track and dotted it; as the bare mask edge, it hatched it.
#define DA_VS_RIM		0.45f
#define DA_VS_WET_MM	0.03f
//	What the inside of the mask is, as a share of the sky: what the reflected part of the sight
//	line sees. Not black - the glass lights the face a little.
#define DA_VS_INTERIOR	0.18f
//	The gather of a hemisphere: how far the drop is lifted toward the sky it mostly looks at.
//	Weighted by the surface's own slope, because it is the CURVED water that gathers - a flat
//	channel or a sheet looks at what is behind it and nothing else. Applied flat, every track
//	across the sky was painted the colour of the top of the frame, a light stripe with no inside.
#define DA_VS_LIFT		0.35f
//	The thickness of the SMOOTHED surface at which water starts having a drop's optics and the
//	one where it has all of them, millimetres. Under the first it is film. The smoothing carries
//	a bead's height a fraction of a millimetre past its edge, so the window sits high enough that
//	the beads do not all grow by that much.
#define DA_VS_ON		0.040f
#define DA_VS_FULL		0.180f
//	The disc the drop's image is gathered over, in uv. The physical figure is about 0.14 of the
//	screen's height; a twentieth of it is taken, so the drops keep their edges and their sizes
//	and the blur only softens what is seen through them.
#define DA_VS_BLUR		0.006f
//	The sun along the refracted line: a broad lobe for the glow of it behind cloud and a narrow
//	one for the glint of it in the clear, both scaled by the sun's own colour, which the weather
//	turns down under an overcast.
#define DA_VS_SUN_SOFT	8.0f
#define DA_VS_SUN_HARD	220.0f
//	How much the film bends and scatters. Microns thick, so on the physics it should do almost
//	nothing - but it is the whole visible difference between wiped glass and clean glass, and a
//	wipe that leaves nothing behind is the delete this feature was rebuilt to stop being. Not
//	louder than the drops: at three times this the tracks were broad dark bands.
#define DA_VS_FILM_BEND	2.20f
#define DA_VS_FILM_HAZE	0.26f
//	The film thickness the haze starts at and the one it is all there at, millimetres. A trail is
//	a few hundredths and a hand's smear a couple of tenths, and the two must not read alike: the
//	trail is a whisper behind a running drop, the smear is what the player has to SEE after the
//	hand goes across, or the wipe reads as a delete. The water the hand leaves as water is
//	regrouped by surface tension into beads too small to draw within a second, so the smear is
//	the film and nothing else - it had better show.
#define DA_VS_FILM_ON	0.03f
#define DA_VS_FILM_FULL	0.15f

float4 da_visor_read(float2 uv)
{
	return s_visor.SampleLevel(smp_rtlinear, uv, 0);
}

//	Reflectance of the water-air surface for a sight line inside the water meeting it at this
//	cosine, unpolarised and exact. It has to be: this is the rim, and Schlick is fitted to the
//	air side - it does not rise to one at the critical angle.
float da_vs_fresnel(float ci)
{
	const float st2 = DA_VS_ETA * DA_VS_ETA * (1.0f - ci * ci);
	[flatten]
	if (st2 >= 1.0f)
		return 1.0f;
	const float ct = sqrt(1.0f - st2);
	const float rs = (DA_VS_ETA * ci - ct) / (DA_VS_ETA * ci + ct);
	const float rp = (DA_VS_ETA * ct - ci) / (DA_VS_ETA * ct + ci);
	return 0.5f * (rs * rs + rp * rp);
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
	[branch]
	if (st.x + st.y < 0.004f)
		return scene;

	const float2 e = da_visor.zw;
	const float mm = da_visor.y;

	//	The surface the eye is given: the field over a bead's footprint, thirteen taps on two rings,
	//	and its slope from the same taps - each ring's directional sum is the gradient of the field
	//	smoothed over that ring, exactly for a plane. +x is right; y is flipped because uv.y runs
	//	down the screen and the eye's y runs up it.
	const float2 d2 = e * (DA_VS_SMOOTH_MM / mm);
	const float2 d1 = d2 * 0.5f;
	float hsum = 3.0f * st.x;
	float2 g1 = (float2)0.0f, g2 = (float2)0.0f;
	{
		const float a = da_visor_read(uv + float2(d1.x, 0.0f)).x, b = da_visor_read(uv - float2(d1.x, 0.0f)).x;
		const float c = da_visor_read(uv + float2(0.0f, d1.y)).x, f = da_visor_read(uv - float2(0.0f, d1.y)).x;
		hsum += 2.0f * (a + b + c + f);
		g1 = float2(a - b, c - f) * (1.0f / DA_VS_SMOOTH_MM);
	}
	[unroll]
	for (int i = 0; i < 8; ++i)
	{
		const float ang = float(i) * 0.7854f;
		const float2 dir = float2(cos(ang), sin(ang));
		const float hk = da_visor_read(uv + dir * d2).x;
		hsum += hk;
		g2 += dir * hk;
	}
	g2 *= 2.0f / (8.0f * DA_VS_SMOOTH_MM);
	const float h_s = hsum * (1.0f / 19.0f);
	float2 slope = 0.5f * (g1 + g2);
	slope.y = -slope.y;
	//	Capped at the contact angle: the field's cells are steeper than water ever is.
	const float sl = length(slope);
	slope *= (sl > DA_VS_SLOPE_MAX) ? (DA_VS_SLOPE_MAX / sl) : 1.0f;
	const float curved = saturate(sl * 2.5f);
	//	How much of a drop's optics this pixel gets, and the band round the drop's edge where its
	//	contact line can be.
	const float cov = smoothstep(DA_VS_ON, DA_VS_FULL, h_s) * saturate(da_visor2.z);
	const float band = saturate((1.0f - abs(2.0f * cov - 1.0f)) * 2.5f);

	//	The sight line through this pixel, in eye space, and the surface's normal on the water
	//	side - facing back at the eye, which is the side refract wants it on. For a surface that
	//	bulges away from the eye that is (dh/dx, dh/dy, -1). With the xy of it flipped, the bottom
	//	of every drop looked at the ground: a magnifier, not a lens.
	const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
	const float3 vdir = normalize(float3(ndc.x / max(da_visor2.x, 1e-4f), ndc.y / max(da_visor2.y, 1e-4f), 1.0f));
	const float3 N = normalize(float3(slope.x, slope.y, -1.0f));

	//	Out of the water into the air. Past the critical angle the line never leaves, and what it
	//	sees is the inside of the mask.
	float3 R = refract(vdir, N, DA_VS_ETA);
	float F = da_vs_fresnel(saturate(-dot(vdir, N)));
	[flatten]
	if (dot(R, R) < 1e-6f || R.z <= 0.01f)
	{
		R = vdir;
		F = 1.0f;
	}

	//	The contact line: a step in the field with dry glass on one side of it. A texel and a half
	//	each way, so it is a line and not the grid. The same four taps carry the film's shape.
	const float2 e1 = e * 1.5f;
	const float4 tx1 = da_visor_read(uv + float2(e1.x, 0.0f));
	const float4 tx0 = da_visor_read(uv - float2(e1.x, 0.0f));
	const float4 ty1 = da_visor_read(uv + float2(0.0f, e1.y));
	const float4 ty0 = da_visor_read(uv - float2(0.0f, e1.y));
	const float inv2mm = 1.0f / (3.0f * mm);
	{
		const float4 wm = saturate(float4(tx1.x, tx0.x, ty1.x, ty0.x) * (1.0f / DA_VS_WET_MM));
		const float edge = max(abs(wm.x - wm.y), abs(wm.z - wm.w));
		F = max(F, edge * band * DA_VS_RIM);
	}

	//	The sky, near the top of the frame: what a drop is mostly looking at, and what lights the
	//	inside of the mask.
	const float3 sky = img.SampleLevel(smp_rtlinear, float2(uv.x, 0.06f), 0).rgb;

	float3 drop;
	{
		//	Back to a screen position: the direction alone decides where to look, since a drop this
		//	close to the eye has no parallax worth the name. Off the frame it looks at the frame's
		//	edge, which is the best guess there is - beyond the top row is more of that sky, beyond
		//	the bottom row more of that ground. Handed to the top row whichever way it left, a
		//	drop's top half showed sky where it should have shown ground.
		const float2 rndc = float2(R.x / max(R.z, 1e-4f) * da_visor2.x, R.y / max(R.z, 1e-4f) * da_visor2.y);
		const float2 ruv = saturate(float2(rndc.x * 0.5f + 0.5f, 0.5f - rndc.y * 0.5f));
		//	Bigger drops hold their image better; the small ones are pure blur.
		const float big = saturate(h_s * 2.5f);
		drop = da_visor_gather(img, ruv, DA_VS_BLUR * (1.0f - 0.5f * big));
		drop = lerp(drop, max(drop, sky), DA_VS_LIFT * curved);
		//	The sun, along the refracted line.
		const float3 L = -normalize(L_sun_dir_e.xyz);
		const float s = saturate(dot(R, L));
		drop += L_sun_color.rgb * (pow(s, DA_VS_SUN_SOFT) * 0.35f + pow(s, DA_VS_SUN_HARD) * 2.0f);
		//	What the surface throws back is the inside of the mask.
		drop = lerp(drop, sky * DA_VS_INTERIOR, F);
	}

	//	The film. Not a lens - a few microns with a shape - so it bends the sight line by a
	//	fraction of what a drop does and hazes what it lets through.
	float3 wiped = scene;
	const float film = st.y;
	[branch]
	if (film > 0.0005f)
	{
		//	The film's own shape, not the drop's: the streaks a hand leaves are ridges a couple of
		//	millimetres apart and they are all the smear has to show for itself.
		const float2 fg = float2(-(tx1.y - tx0.y), (ty1.y - ty0.y)) * inv2mm;
		const float2 foff = fg * (DA_VS_FILM_BEND * 0.004f);
		const float3 through = da_visor_gather(img, saturate(uv + foff), 0.004f);
		const float haze = smoothstep(DA_VS_FILM_ON, DA_VS_FILM_FULL, film) * DA_VS_FILM_HAZE;
		const float lum = dot(through, float3(0.30f, 0.59f, 0.11f));
		//	Wet glass scatters forward: the blacks lift a little and the colour goes toward the sky
		//	it is scattering, which is what a smeared visor looks like against a bright sky. Against
		//	the sky itself it is nearly nothing, which is right - what shows a smear there is the
		//	bend of its ridges, not a tint.
		wiped = lerp(through, lerp(lerp(through, (float3)lum, 0.45f), sky, 0.20f) + 0.006f, haze);
	}

	return lerp(wiped, drop, cov);
}

#endif	// DA_VISOR_H
