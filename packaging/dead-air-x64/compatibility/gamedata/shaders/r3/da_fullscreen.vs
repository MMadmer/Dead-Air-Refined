#include "common.h"

// [DA_PORT] Full-screen quad for the passes that filter a buffer (see da_reactive.ps,
// da_velocity_guard.ps). Written rather than borrowed, and that is the point of it.
//
// Both of those passes first used stub_notransform_2uv, which expects a vertex carrying TWO sets of
// texture coordinates. The geometry they are drawn with, g_combine, is FVF::F_TL - position, colour
// and ONE set. A vertex layout that does not match what the vertex shader declares makes D3D11 drop
// the draw, and it does so in silence: no error, no warning, the target simply keeps whatever it held.
// Both passes ran every frame for hours, measured as if they were working, and wrote nothing at all.
// The velocity guard then copied its untouched buffer over the motion vectors and zeroed them, which
// is what "the whole picture swims when the camera moves" actually was.
//
// So this takes exactly the vertex g_combine supplies, and nothing else can drift out of step with it.
// The layout follows combine_1.vs, the engine's own full-screen shader: xy of the position is already
// in clip space, zw carries the texture coordinate, and y is flipped because the quad is built with
// the texture's row order rather than clip space's.

struct _in
{
	float4	P	: POSITIONT;	// xy = clip-space position, zw = texture coordinate
	float2	tcJ	: TEXCOORD0;	// unused here; the vertex carries it, so it must be declared
};

struct v2p
{
	float2	tc0		: TEXCOORD0;
	float4	hpos	: SV_Position;
};

v2p main(_in I)
{
	v2p O;
	O.hpos	= float4(I.P.x, -I.P.y, 0, 1);
	O.tc0	= I.P.zw;
	return O;
}

FXVS;
