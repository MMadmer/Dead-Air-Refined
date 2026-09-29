#ifndef DA_VELOCITY_H
#define DA_VELOCITY_H

// Motion vectors (docs/dead-air/DX12_RENDERER.md, 3.4). Every G-buffer shader writes
// NDC(current) - NDC(previous), both unjittered, y up, into SV_Target2 of the packed G-buffer;
// the target is bound only while TAA or an upscaler runs.
//
// The vertex shader hands its pixel shader two clip positions (hpos_cur, hpos_prev in the
// v2p/p structs): hpos_cur = mul(m_WVP_nojit, P), hpos_prev = mul(m_WVP_prev, P_prev), where
// P_prev is the vertex as it was on the previous frame (the previous bone palette, the previous
// wind) and m_WVP_prev already carries the previous world matrix of the draw. The pixel shader
// writes da_velocity(I.hpos_cur, I.hpos_prev).
//
// The constants live in named cbuffers: a shader that does not reference them does not carry
// them. Set by R_xforms per draw; while the HUD is drawn the view-projections are the HUD's.

cbuffer da_motion_view
{
	uniform float4x4	m_VP_nojit;		// this frame's view-projection, without the jitter
	uniform float4x4	m_VP_prev;		// the previous frame's, without the jitter
}

cbuffer da_motion_xform
{
	uniform float4x4	m_WVP_nojit;	// m_VP_nojit x the draw's world
	uniform float4x4	m_WVP_prev;		// m_VP_prev x the draw's world on the previous frame
	uniform float3x4	m_W_prev;		// the draw's world on the previous frame
}

float2 da_velocity(float4 hpos_cur, float4 hpos_prev)
{
	// Behind the previous camera there is no previous screen position: report no motion.
	if (hpos_prev.w <= 1e-5 || hpos_cur.w <= 1e-5)
		return float2(0, 0);
	// A previous w just above zero overflows the 16-bit target to infinity, which the temporal
	// resolvers keep in their history for good; 16 NDC units is far off screen already.
	return clamp(hpos_cur.xy / hpos_cur.w - hpos_prev.xy / hpos_prev.w, -16.0, 16.0);
}

#endif // DA_VELOCITY_H
