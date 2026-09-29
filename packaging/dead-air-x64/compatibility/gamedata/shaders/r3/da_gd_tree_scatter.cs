// GPU-driven trees (gpudriven.md 3.6 step 3): every instance the cull kept goes to its slot of its bin in
// the refs stream (the per-instance GDREF of the draws): refs[record.StartInstance + slot] = instance.

// x = bins (the entry count sits after the bin counts), y = capacity of gd_vis
uniform float4 gd_scatter_size;

RWByteAddressBuffer gd_count : register(u0);
RWByteAddressBuffer gd_vis : register(u1);
RWByteAddressBuffer gd_args : register(u2);
RWByteAddressBuffer gd_refs : register(u3);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const uint entries = min(gd_count.Load((uint)gd_scatter_size.x * 4), (uint)gd_scatter_size.y);
	if (id.x >= entries)
		return;
	const uint3 v = gd_vis.Load3(id.x * 12);
	const uint start = gd_args.Load(v.y * 20 + 16);
	gd_refs.Store((start + v.z) * 4, v.x);
}
