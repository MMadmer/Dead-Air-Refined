#ifndef TREE_WIND_H
#define TREE_WIND_H

// Hierarchical tree wind (the GPU Gems 3 ch.16 / SpeedTree scheme, fitted to the assets we
// actually have). The stock shader bends the WHOLE crown as one body - one phase per tree,
// amplitude by height - which is exactly why the trees read as cardboard the moment they
// move. Two layers are added on top of that stock bend, both scaled by the same weather
// wind vector, so a calm evening stays calm and a storm goes wild:
//
//   branch layer - a slow oscillation whose phase drifts across the crown with the OBJECT
//   position: vertices of one branch move together, opposite branches desync;
//
//   leaf layer - a fast small flutter, phase unique per leaf card (the tc.z frac channel
//   the bake already carries plus a position hash), directed along the card NORMAL so the
//   card actually tilts like a leaf instead of sliding sideways.
//
// The assets carry no painted wind weights, so "how leafy is this vertex" is derived from
// geometry: crowns live away from the trunk axis and above the root - bark near the axis
// stays put, branch tips and leaf cards take the full motion. Thick-branch bark ends up
// with a few millimetres of creep, which reads as organic rather than wrong.

// lpos = object-space position, H = height above the root (world units),
// n_w = world-space normal (zero = skip the leaf layer, e.g. the shadow pass),
// leaf_frac = the per-vertex frac channel (I.tc.z * consts.x, same one the stock bend uses).
// Returns a world-space displacement to ADD to the stock-bent position.
float3 tree_wind_extra(float3 lpos, float H, float3 n_w, float leaf_frac)
{
	float wind_amp = length(wind.xz);
	[branch] if (wind_amp < 0.0001f)
		return float3(0, 0, 0);

	// Leafiness from geometry: radial distance from the trunk axis, gated by height.
	float radial = length(lpos.xz);
	float w_leaf = saturate(radial * 0.35f) * saturate(H * 0.4f);
	[branch] if (w_leaf < 0.001f)
		return float3(0, 0, 0);

	float2 dir = wind.xz / wind_amp;

	// Branch sway: ~0.3 Hz plus a half-speed harmonic so the motion never loops visibly.
	// The phase term varies over metres - about the span of one branch.
	float ph_b = dot(lpos, float3(0.9f, 0.4f, 0.7f));
	float s_b = sin(timers.x * 1.7f + ph_b) + 0.4f * sin(timers.x * 0.9f + ph_b * 1.3f);
	// Slight downward dip on the sway - real branches sag into a gust, not just sideways.
	float3 d_branch = float3(dir.x, -0.22f, dir.y) * (s_b * wind_amp * 0.8f * w_leaf);

	// Leaf flutter: 3-5 Hz, a few centimetres, along the card normal.
	float3 d_leaf = float3(0, 0, 0);
	[branch] if (dot(n_w, n_w) > 0.01f)
	{
		float ph_l = leaf_frac * 17.0f + dot(lpos, float3(5.3f, 8.1f, 6.7f));
		float s_l = sin(timers.x * 21.0f + ph_l) + 0.5f * sin(timers.x * 33.0f + ph_l * 1.7f);
		d_leaf = n_w * (s_l * wind_amp * 0.30f * w_leaf);
	}

	return d_branch + d_leaf;
}

// Crown normal rounding (the SpeedTree lighting trick): leaf-card normals point wherever
// the flat card happens to face, so a lit crown shades like a heap of plates. Bending the
// normals outward from the trunk axis makes deferred lighting treat the crown as one
// convex volume - lit side bright, far side shading itself. Bark (small radius) keeps its
// real normal, so trunks stay crisp.
float3 tree_round_normal(float3 n_obj, float3 lpos)
{
	float radial = length(lpos.xz);
	float k = 0.35f * saturate(radial * 0.35f);
	[branch] if (k < 0.001f)
		return n_obj;
	float3 out_dir = normalize(float3(lpos.x, radial * 0.4f, lpos.z));
	return normalize(lerp(n_obj, out_dir, k));
}

#endif
