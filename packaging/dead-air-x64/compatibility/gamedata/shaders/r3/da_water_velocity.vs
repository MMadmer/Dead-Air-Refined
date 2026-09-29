// Motion vectors of the water surface (phase_water_velocity, docs/dead-air/DX12_RENDERER.md 3.4).
//
// The forward water geometry again, into $user$velocity: the G-buffer under it holds the bed,
// but what the eye (and the upscaler's history) sees there is the surface. The deformation is
// water.vs's, exactly - the stock bob of watermove(); nothing else in the water moves geometry.
//
// The input is POSITION alone. The level's water vertex carries six elements; declaring only the
// one used keeps the input layout trivially valid for whichever declaration the mesh has.

#include "common.h"
#include "shared\waterconfig.h"
#include "shared\watermove.h"

// x: timers.x as the previous frame drew the water (the bob's clock one frame back).
uniform float4 da_water_velocity;

struct v2p
{
	float4	hpos_cur	: TEXCOORD0;	// unjittered clip position, this frame
	float4	hpos_prev	: TEXCOORD1;	// unjittered clip position, previous frame
	float4	hpos		: SV_Position;
};

// watermove() with the previous frame's clock.
float4 da_watermove_prev(float4 P)
{
	float3	wave1	= float3(0.11f, 0.13f, 0.07f) * W_POSITION_SHIFT_SPEED;
	float	dh		= sin(da_water_velocity.x + dot((float3)P, wave1));
			P.y		+= dh * W_POSITION_SHIFT_HEIGHT;
	return	P;
}

v2p main(float4 P_in : POSITION)
{
	v2p o;
	const float4 P = watermove(P_in);
	const float4 P_prev = da_watermove_prev(P_in);
	// The same jittered position water.vs rasterises at, so the depth test against the scene
	// keeps exactly the pixels the water covers.
	o.hpos		= mul(m_VP, P);
	o.hpos_cur	= mul(m_VP_nojit, P);
	o.hpos_prev	= mul(m_VP_prev, P_prev);
	return o;
}
