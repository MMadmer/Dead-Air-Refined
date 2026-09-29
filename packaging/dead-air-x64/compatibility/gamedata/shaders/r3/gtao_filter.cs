// Dead Air: Refined - guided GTAO filter as a compute shader (the DirectX 12 async path,
// phase_gtao_cs). The body is gtao_filter_pixel (gtao.h), shared with gtao_filter.ps, so both
// write the same final AO into $user$ssao_temp.

#define DA_COMPUTE
#include "gtao.h"

RWTexture2D<float> u_ao : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    [branch] if (any(id.xy >= uint2(pos_decompression_params2.xy)))
        return;
    DA_STAGE_EXACT float r = gtao_filter_pixel(id.xy);
    u_ao[id.xy] = r;
}
