#include "common.h"

// The cloud dome is only a way to run a pixel shader over the sky: each vertex hands the
// pixel its world-space view direction and the weather's cloud colour. Everything else -
// where the cloud is, how it moves, how it is lit - happens per pixel in clouds.ps against
// the world-anchored deck.
struct vi
{
	float4	p		: POSITION;
	float4	dir		: COLOR0;	// unused now: the deck drifts in world space
	float4	color	: COLOR1;	// rgb = weather cloud colour, w = its opacity
};

struct vf
{
	float4	color	: COLOR0;
	float3	wdir	: TEXCOORD0;	// world-space direction from the eye, unnormalised
	float4 	hpos	: SV_Position;
};

vf main (vi v)
{
	vf 		o;
	o.hpos 		= mul		(m_WVP, v.p);
	// The dome sits on the camera (translate_over): world position minus the eye is the ray.
	const float3 wp = mul(m_W, v.p).xyz;
	o.wdir		= wp - eye_position;
	// D3DCOLOR arrives as RGBA on DX11 and the CPU packs it for that order (RenderClouds).
	o.color		= v.color;
	return o;
}
