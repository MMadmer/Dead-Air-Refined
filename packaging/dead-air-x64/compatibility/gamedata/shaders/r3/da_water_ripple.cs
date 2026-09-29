#define DA_COMPUTE
#include "da_water_ripple.h"

//	One fixed step of the ripple field's real-space half as a compute shader (the DirectX 12 async
//	path, phase_water_ripple_cs): the body is da_water_ripple_pixel (da_water_ripple.h), shared with
//	da_water_ripple.ps. Writes the solver's first scratch buffer.
RWTexture2D<float2> u_ripple : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (any(id.xy >= uint2(da_ripple_sim.zz)))
		return;
	u_ripple[id.xy] = da_water_ripple_pixel(id.xy);
}
