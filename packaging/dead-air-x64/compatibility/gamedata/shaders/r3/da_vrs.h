#ifndef DA_VRS_H
#define DA_VRS_H

//	Variable rate shading (docs/dead-air/dx12-features/vrs.md 3.2). A rate is D3D12_SHADING_RATE:
//	(log2 of the coarse width) << 2 | log2 of the coarse height, so 1x1 = 0, 2x2 = 5, 4x4 = 10.

//	size: pixels per axis, 1, 2 or 4.
uint da_vrs_encode(uint2 size)
{
	return (firstbithigh(size.x) << 2) | firstbithigh(size.y);
}

uint2 da_vrs_decode(uint rate)
{
	return uint2(1u << ((rate >> 2) & 3u), 1u << (rate & 3u));
}

//	The luminance the contrast is measured in: of the gamma-encoded LDR frame, roughly perceptual.
float da_vrs_luma(float3 rgb)
{
	return dot(rgb, float3(0.299, 0.587, 0.114));
}

#endif
