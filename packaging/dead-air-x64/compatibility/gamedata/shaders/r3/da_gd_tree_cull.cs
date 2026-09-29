#include "da_gd_frustum.h"

// GPU-driven trees (gpudriven.md 3.6): the cull of one view. It selects, per tree instance, what
// insert_static (r__dsgraph_build.cpp) would: the element, the LOD window and every pass of the element,
// and counts the instance into the bin (prototype, pass, window) of each; da_gd_tree_args.cs turns the
// counts into DrawIndexedInstanced records, da_gd_tree_scatter.cs writes the instance references.
//
// Entries of the view list (gd_list, uploaded per view):
// - node list (the main view): one node per thread, its instances only: the CPU walk made every node
//   decision (frustum, HOM, value filter, FLOD).
// - cells (shadow views): one cell per group, (first node, nodes, first frustum, frustums); the group
//   tests each node against the frustums of its sector (testSAABB), the value filter of its
//   hierarchies and its own (stamps per context), the FLOD switch, then its instances.

#define GD_NONE 0xFFFFFFFFu
#define GD_CONTEXTS 9u
#define GD_EPS 0.00001f

#define GD_SMAP 1u
#define GD_RT_SKIP 2u
#define GD_NODE_LIST 4u
#define GD_FILTER 8u

// xyz = the device camera (every view measures SSA and the value filter from it), w = (fov / 67)^2.
uniform float4 gd_cam;
// r_ssaDISCARD, r_ssaLOD_A, r_ssaVEG_DISCARD, 0
uniform float4 gd_ssa;
// r_ssaGLOD_start, r_ssaGLOD_end, r_dtex_range, 0
uniform float4 gd_lod;
// x = flags (GD_*), y = context id, z = frame (24 bits), w = statistics slot
uniform float4 gd_view;
// x = entries, y = plane offset, z = frustum offset, w = entry offset (bytes into gd_list)
uniform float4 gd_layout;
// The value filter's thresholds (staticGeometryThresholds of the quality): volumes 0-3, distances 0-3,
// and x = volume 4, y = distance 4, z = bins, w = capacity of gd_vis in entries.
uniform float4 gd_filter_v;
uniform float4 gd_filter_d;
uniform float4 gd_filter_e;

// 32 bytes: sphere; kind, flags (bit 0: rt_static), 0, 0
ByteAddressBuffer gd_inst : register(t0);
// 64 bytes: box min, first instance; box max, instances; sphere; lod_factor (negative: a single tree),
// first ancestor, ancestors, 0
ByteAddressBuffer gd_node : register(t1);
// 16 bytes per value-filter slot (nodes, then the hierarchies above them): sphere
ByteAddressBuffer gd_filter : register(t2);
// value-filter slots of each node's hierarchies, root first
ByteAddressBuffer gd_anc : register(t3);
// 32 bytes: first bin of window 0 for (HQ, LQ, shadow) x (pass 0, pass 1), windows, 0
ByteAddressBuffer gd_kind : register(t4);
ByteAddressBuffer gd_list : register(t5);

// bins x u32 instance counts, then the gd_vis count
RWByteAddressBuffer gd_count : register(u0);
// (instance, bin, slot)
RWByteAddressBuffer gd_vis : register(u1);
// [slot][context] frame the slot was last kept (is_static_geometry_valuable's valuable_frame)
RWByteAddressBuffer gd_valuable : register(u2);
// 4 frames x 8 counters: instances main, shadow, views, entries main, shadow, nodes, 0, 0
RWByteAddressBuffer gd_stats : register(u3);

groupshared uint g_instances;
groupshared uint g_entries;
groupshared uint g_nodes;

bool gd_valuable_test(uint slot, uint flags)
{
	if (!(flags & GD_FILTER))
		return true;
	const float4 s = asfloat(gd_filter.Load4(slot * 16));
	const float volume = (12.566370614359172f / 3.0f) * (s.w * s.w * s.w);
	const float3 d = gd_cam.xyz - s.xyz;
	const float dist2 = (dot(d, d) + GD_EPS) * gd_cam.w;
	const uint address = (slot * GD_CONTEXTS + (uint)gd_view.y) * 4;
	const uint frame = (uint)gd_view.z;
	// Kept last frame: it has to travel 15 % further out before it is dropped (the dead band).
	const bool sticky = ((frame - gd_valuable.Load(address)) & 0xFFFFFFu) <= 1u;
	const float scale = sticky ? 1.15f : 1.0f;
	const float4 lv = gd_filter_d * scale;
	const float le = gd_filter_e.y * scale;
	if (volume < gd_filter_v.x && dist2 > lv.x * lv.x)
		return false;
	if (volume < gd_filter_v.y && dist2 > lv.y * lv.y)
		return false;
	if (volume < gd_filter_v.z && dist2 > lv.z * lv.z)
		return false;
	if (volume < gd_filter_v.w && dist2 > lv.w * lv.w)
		return false;
	if (volume < gd_filter_e.x && dist2 > le * le)
		return false;
	gd_valuable.Store(address, frame);
	return true;
}

void gd_emit(uint bin, uint instance)
{
	uint slot, index;
	gd_count.InterlockedAdd(bin * 4, 1, slot);
	gd_count.InterlockedAdd((uint)gd_filter_e.z * 4, 1, index);
	if (index < (uint)gd_filter_e.w)
		gd_vis.Store3(index * 12, uint3(instance, bin, slot));
}

// insert_static for one tree instance of a visible node.
void gd_instance(uint i, uint flags)
{
	const float4 sphere = asfloat(gd_inst.Load4(i * 32));
	// kind, flags
	const uint2 b = gd_inst.Load2(i * 32 + 16);
	// Ray traced sun shadows cover this caster in the near and middle cascades (PLAN C16).
	if ((flags & GD_RT_SKIP) && (b.y & 1u))
		return;
	const float3 d = gd_cam.xyz - sphere.xyz;
	const float dist2 = dot(d, d) + GD_EPS;
	const float ssa = sphere.w / dist2;
	if (ssa <= gd_ssa.x)
		return;
	// rimp_select_sh_static: HQ inside r_dtex_range, LQ beyond it, the shadow element in shadow views.
	uint element = 2;
	if (!(flags & GD_SMAP))
		element = (sqrt(dist2) - sphere.w) < gd_lod.z ? 0 : 1;
	const uint2 bins = gd_kind.Load2(b.x * 32 + element * 8);
	if (bins.x == GD_NONE && bins.y == GD_NONE)
		return;
	const uint windows = gd_kind.Load(b.x * 32 + 24);
	uint window = 0;
	if (windows > 1)
	{
		// FTreeVisual_PM::SelectLOD over calcLOD (r__dsgraph_render.cpp).
		const float lod = sqrt(saturate((ssa - gd_lod.y) / (gd_lod.x - gd_lod.y)));
		window = (uint)clamp((int)floor((1.0f - lod) * (float)(windows - 1) + 0.5f), 0, (int)windows - 1);
	}
	uint emitted = 0;
	if (bins.x != GD_NONE)
	{
		gd_emit(bins.x + window, i);
		++emitted;
	}
	if (bins.y != GD_NONE)
	{
		gd_emit(bins.y + window, i);
		++emitted;
	}
	InterlockedAdd(g_instances, 1);
	InterlockedAdd(g_entries, emitted);
}

// add_static of a shadow view for a node against its sector's frustums, then its instances.
void gd_node_cull(uint n, uint first_frustum, uint frustums, uint flags)
{
	const uint4 n0 = gd_node.Load4(n * 64);
	const uint4 n1 = gd_node.Load4(n * 64 + 16);
	const float4 sphere = asfloat(gd_node.Load4(n * 64 + 32));
	const uint3 n3 = gd_node.Load3(n * 64 + 48);
	const float3 bmin = asfloat(n0.xyz);
	const float3 bmax = asfloat(n1.xyz);

	bool visible = false;
	const uint planes = (uint)gd_layout.y;
	for (uint f = 0; f < frustums && !visible; ++f)
	{
		const uint2 fr = gd_list.Load2((uint)gd_layout.z + (first_frustum + f) * 8);
		visible = da_gd_saabb(gd_list, planes + fr.x * 16, fr.y, sphere.xyz, sphere.w, bmin, bmax);
	}
	if (!visible)
		return;
	// The value filter of the hierarchies the node hangs under (the sector root first).
	for (uint k = 0; k < n3.z; ++k)
		if (!gd_valuable_test(gd_anc.Load((n3.y + k) * 4), flags))
			return;
	if (!gd_valuable_test(n, flags))
		return;
	const float lod_factor = asfloat(n3.x);
	if (lod_factor >= 0.0f)
	{
		// The FLOD switch of a shadow view: no impostor goes into it, the children always do unless the
		// family is below both r_ssaLOD_A and r_ssaVEG_DISCARD.
		const float3 d = gd_cam.xyz - sphere.xyz;
		const float ssa = sphere.w / (dot(d, d) + GD_EPS) * lod_factor;
		if (ssa < gd_ssa.y && ssa < gd_ssa.z)
			return;
	}
	InterlockedAdd(g_nodes, 1);
	for (uint i = 0; i < n1.w; ++i)
		gd_instance(n0.w + i, flags);
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
	if (index == 0)
	{
		g_instances = 0;
		g_entries = 0;
		g_nodes = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	const uint flags = (uint)gd_view.x;
	const uint entries = (uint)gd_layout.x;
	if (flags & GD_NODE_LIST)
	{
		const uint e = group.x * 64 + index;
		if (e < entries)
		{
			const uint n = gd_list.Load((uint)gd_layout.w + e * 4);
			const uint4 n0 = gd_node.Load4(n * 64);
			const uint count = gd_node.Load(n * 64 + 28);
			InterlockedAdd(g_nodes, 1);
			for (uint i = 0; i < count; ++i)
				gd_instance(n0.w + i, flags);
		}
	}
	else if (group.x < entries)
	{
		const uint4 entry = gd_list.Load4((uint)gd_layout.w + group.x * 16);
		for (uint k = index; k < entry.y; k += 64)
			gd_node_cull(entry.x + k, entry.z, entry.w, flags);
	}

	GroupMemoryBarrierWithGroupSync();
	if (index == 0)
	{
		const uint stats = ((uint)gd_view.w & 3u) * 32;
		const bool smap = (flags & GD_SMAP) != 0;
		gd_stats.InterlockedAdd(stats + (smap ? 4 : 0), g_instances);
		gd_stats.InterlockedAdd(stats + (smap ? 16 : 12), g_entries);
		gd_stats.InterlockedAdd(stats + 20, g_nodes);
		if (group.x == 0)
			gd_stats.InterlockedAdd(stats + 8, 1);
	}
}
