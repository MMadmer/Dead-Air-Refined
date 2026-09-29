#ifndef DA_DETAIL_MESH_H
#define DA_DETAIL_MESH_H

// What the grass amplification shader (deffer_detail_cull.as) and mesh shader
// (deffer_detail_mesh.ms) share: the DirectX 12 mesh path of the detail grass. The grass maths is
// da_detail_common.h, the same as the vertex shaders'. Needs common.h and da_detail_common.h first.
//
// The engine keeps the instance rows of every visible part GPU-resident in a pool
// (DetailManager_MS.cpp) and writes, every frame, the list of pool rows the visible parts occupy
// (grass_index), part after part in draw order. A dispatch covers a run of that list; one
// amplification group takes 32 consecutive entries, drops the rows it can prove draw nothing and
// launches mesh groups of up to grass_obj.w tufts.

#define GRASS_AS_THREADS	32
#define GRASS_MAX_IPG		32
#define GRASS_MS_THREADS	64
#define GRASS_MS_MAX_VERTS	64
#define GRASS_MS_MAX_PRIMS	126
#define GRASS_MAX_PLANES	12

// One vertex of a detail model (hw_Load_Geom's numbers): position, then u,v and frac,matrix-id as
// signed 16-bit pairs - what the vertex path reads through its SHORT4 input.
struct detail_vtx
{
	float3	pos;
	uint	uv;
	uint	tmid;
};

// CDetailManager::InstanceRows: three rows of the tuft's 3x4 transform (w = root), then colour.
struct detail_rows
{
	float4	m0;
	float4	m1;
	float4	m2;
	float4	c0;
};

// The pool rows an amplification group kept, in list order.
struct grass_payload
{
	uint	count;
	uint	rows[GRASS_AS_THREADS];
};

uniform float4 		consts;			// {1/quant,1/quant,diffusescale,ambient}
// Shadow fade band and the camera position, see deffer_detail_w_flat.vs.
uniform float4 		grass_sfade;
uniform float4 		grass_sfade_eye;
uniform float4 		grass_tint;
// x = first vertex of the model, y = first index, z = vertex count + triangle count * 65536,
// w = tufts per mesh group. Floats: the constant table takes no uint.
uniform float4 		grass_obj;
// x = first entry of this dispatch in the frame's row list, y = its entry count, z = 0, w = bend
// margin: how far the bend can carry a vertex per unit of height scale (1.103 of the highest model
// point above the root, 2.2 of the lowest below it).
uniform float4 		grass_dispatch;
// The model's bounding sphere in model space.
uniform float4 		grass_bound;
// The pass frustum: xyz normal, w distance; a point is outside when dot > radius (CFrustum).
// Planes the pass does not use are (0,0,0,-1e30).
uniform float4 		grass_planes[GRASS_MAX_PLANES];

// Vertex count, triangle count and tufts per group of the model.
uint	grass_nv	()	{ return uint(grass_obj.z) & 0xffff; }
uint	grass_nt	()	{ return uint(grass_obj.z) >> 16; }
uint	grass_ipg	()	{ return max(uint(grass_obj.w), 1u); }

// Index i of the model's u16 index list.
uint	detail_index	(ByteAddressBuffer ib, uint i)
{
	const uint w = ib.Load((i >> 1) << 2);
	return (i & 1) ? (w >> 16) : (w & 0xffff);
}

// The rows of a tuft as the vertex shaders see them after the shadow fade band.
detail_rows	detail_rows_faded	(detail_rows d)
{
	[branch] if ( grass_sfade.y > 0.001f )
	{
		const float k = detail_sfade_k( float3( d.m0.w, d.m1.w, d.m2.w ), grass_sfade, grass_sfade_eye.xyz );
		d.m0.y *= k; d.m1.y *= k; d.m2.y *= k;
	}
	return d;
}

#endif
