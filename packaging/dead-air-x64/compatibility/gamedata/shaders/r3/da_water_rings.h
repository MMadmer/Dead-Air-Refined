#ifndef DA_WATER_RINGS_H
#define DA_WATER_RINGS_H

// Water impact spots (Environment::water_hit), packed like the wind motors, pre-transposed:
// a pos row = (xyz, radius), a par row = (amplitude, ring-front radius, 0 ring / 1 drain, 0),
// info.x = the live count - a quiet world walks nothing. The envelopes are computed on the
// CPU, so every spot dies at zero amplitude by construction. Shared by the rain puddles
// (da_puddles.h) and open water (water.ps): a ring is a ring on any water.
uniform float4x4 da_wh_pos0;
uniform float4x4 da_wh_pos1;
uniform float4x4 da_wh_par0;
uniform float4x4 da_wh_par1;
uniform float4 da_wh_info;

// The ripple gradient (world XZ) of every live ring at this point: a crest at the front with
// a decaying train of rings behind it (lambda = 0.30 m). Drains are the puddles' own business.
float2 da_water_rings(float3 pos_w)
{
	float2 ripple = 0.0f;
	const int wh_count = int(da_wh_info.x);
	[loop]
	for (int ri = 0; ri < wh_count; ++ri)
	{
		const int rlo = min(ri, 3);
		const int rhi = max(ri - 4, 0);
		const float4 RP = (ri < 4) ? da_wh_pos0[rlo] : da_wh_pos1[rhi];
		const float4 RA = (ri < 4) ? da_wh_par0[rlo] : da_wh_par1[rhi];
		[branch]
		if (RP.w <= 0.0f || RA.z > 0.5f || RA.x <= 0.001f)
			continue;
		float2 rd = pos_w.xz - RP.xz;
		const float rwd = length(rd);
		[branch]
		if (rwd > RA.y + 0.4f || rwd < 0.02f || abs(pos_w.y - RP.y) > 2.5f)
			continue;
		const float behind = RA.y - rwd;
		const float wave = sin(behind * 20.9f) * exp(-behind * 1.7f) * RA.x;
		ripple += (rd / rwd) * (wave * 0.16f);
	}
	return ripple;
}

#endif // DA_WATER_RINGS_H
