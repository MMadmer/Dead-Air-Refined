#ifndef DA_GD_FRUSTUM_H
#define DA_GD_FRUSTUM_H

// GPU-driven trees (gpudriven.md 3.6): the visibility test of the cull.

// CFrustum::testSAABB (Frustum.h) of a node against one frustum: planes (normal, d), classify(p) =
// dot(n, p) + d, positive outside. Only the "none" verdict matters: a node partly or fully inside is
// visible (the CPU's plane mask only skips planes that cannot say "none" any more).
bool da_gd_saabb(ByteAddressBuffer planes, uint address, uint count, float3 c, float r, float3 bmin, float3 bmax)
{
	for (uint i = 0; i < count; ++i)
	{
		const float4 p = asfloat(planes.Load4(address + i * 16));
		const float cls = dot(p.xyz, c) + p.w;
		if (cls > r)
			return false;
		if (abs(cls) < r)
		{
			// The box's corner farthest against the normal (AABB_OverlapPlane's "Neg").
			const float3 neg = float3(p.x > 0 ? bmin.x : bmax.x, p.y > 0 ? bmin.y : bmax.y, p.z > 0 ? bmin.z : bmax.z);
			if (dot(p.xyz, neg) + p.w > 0)
				return false;
		}
	}
	return true;
}

#endif
