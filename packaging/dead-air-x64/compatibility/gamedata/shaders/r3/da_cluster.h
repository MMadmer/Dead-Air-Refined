// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy's records and its cut (docs/dead-air/CLUSTERS.md; da_clusters.cpp): a cluster's 80 bytes, the
// view's constants, and the test that picks a cluster for the view - its own error on the screen within the pixel
// threshold, its parents' past it (a root's parents always past it), so siblings of one group switch together and the
// cut has no cracks.
#ifndef DA_CLUSTER_H
#define DA_CLUSTER_H

// A cluster: its box (centre, half extent), its own error and its parents' (the generating group's and the group it is
// a child of), their spheres, its triangles (the first, in the triangle bytes) and its vertex table (the first entry,
// in the table's 16-bit words), its counts (triangles in the low byte, vertices less one in the next).
#define DA_CL_BYTES 80u

struct DaCluster
{
	float3	box_c;
	float	lod_error;
	float3	box_e;
	float	parent_error;
	float4	lod_sphere;
	float4	parent_sphere;
	uint	tri_first;
	uint	vert_first;
	uint	tris;
	uint	verts;
};

DaCluster	da_cl_load	(ByteAddressBuffer b, uint i)
{
	const uint a = i * DA_CL_BYTES;
	DaCluster c;
	const float4 r0 = asfloat(b.Load4(a));
	const float4 r1 = asfloat(b.Load4(a + 16u));
	c.box_c = r0.xyz;
	c.lod_error = r0.w;
	c.box_e = r1.xyz;
	c.parent_error = r1.w;
	c.lod_sphere = asfloat(b.Load4(a + 32u));
	c.parent_sphere = asfloat(b.Load4(a + 48u));
	const uint4 d = b.Load4(a + 64u);
	c.tri_first = d.x;
	c.vert_first = d.y;
	c.tris = d.z & 0xFFu;
	c.verts = ((d.z >> 8) & 0xFFu) + 1u;
	return c;
}

uniform float4	da_cl_eye;		// xyz the eye, w 1 = a perspective view (0: orthographic, the sun's cascades)
uniform float4	da_cl_fwd;		// xyz the view's forward, w the near plane
uniform float4	da_cl_lod;		// x pixels per unit of error at a unit of depth over the threshold (per unit: orthographic)

// The least of the depth times the cosine off the axis over a sphere: the scale a world length projects by is the
// inverse of it (a length across the ray at depth z and angle t off the axis covers f / (z cos t) of the screen). Bounded
// below by the nearest depth squared over the farthest distance, so a sphere inside another never projects larger.
float	da_cl_edge_scale	(float4 s)
{
	if (da_cl_eye.w < 0.5f)
		return 1.0f;
	const float3 d = s.xyz - da_cl_eye.xyz;
	const float z = max(dot(d, da_cl_fwd.xyz) - s.w, da_cl_fwd.w);
	return z * z / (length(d) + s.w);
}

// The pixels an error over a sphere covers, over the threshold (1 = at it).
float	da_cl_pixels	(float error, float4 s)
{
	return error * da_cl_lod.x / da_cl_edge_scale(s);
}

// The cut: its own simplification fine enough, its parents' not.
bool	da_cl_in_cut	(DaCluster c)
{
	return da_cl_pixels(c.lod_error, c.lod_sphere) <= 1.0f && da_cl_pixels(c.parent_error, c.parent_sphere) > 1.0f;
}

#endif
