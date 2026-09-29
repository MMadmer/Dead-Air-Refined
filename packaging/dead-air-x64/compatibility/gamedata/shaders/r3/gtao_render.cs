// Dead Air: Refined - GTAO render pass as a compute shader (the DirectX 12 async path,
// phase_gtao_cs). The body is gtao_render_pixel (gtao.h), shared with gtao_render.ps, so both
// write the same (view-z, raw occlusion) into $user$gtao_0.

#define DA_COMPUTE
#include "gtao.h"

RWTexture2D<float2> u_gtao : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    [branch] if (any(id.xy >= uint2(pos_decompression_params2.xy)))
        return;
    DA_STAGE_EXACT float2 r = gtao_render_pixel(id.xy);
    u_gtao[id.xy] = r;
}
