// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The local lamps' volumes, instanced (da_light_volumes.h): instance n is the draw's n-th record, its unit cone, sphere
// or cube-face cap (the stock du_* meshes) placed by the lamp's own transform.

#include "common.h"
#include "da_light_volumes.h"

DA_RAW(da_lv_lights, t16);

uniform float4 da_lv;	// the draw's first record, -, -, -

struct v2p_lv
{
	float4 hpos : SV_Position;
	nointerpolation uint light : TEXCOORD1;
};

v2p_lv main(float4 P : POSITION, uint iid : SV_InstanceID)
{
	const uint light = (uint)da_lv.x + iid;
	const uint a = light * DA_LV_STRIDE + DA_LV_XFORM_OFFSET;
	const float4 x0 = asfloat(DA_LOAD4(da_lv_lights, a));
	const float4 x1 = asfloat(DA_LOAD4(da_lv_lights, a + 16u));
	const float4 x2 = asfloat(DA_LOAD4(da_lv_lights, a + 32u));
	const float4 p = float4(P.xyz, 1.0f);
	v2p_lv O;
	O.hpos = mul(m_VP, float4(dot(x0, p), dot(x1, p), dot(x2, p), 1.0f));
	O.light = light;
	return O;
}
