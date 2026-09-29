// GPU-driven trees (gpudriven.md 3.6 step 2): one DrawIndexedInstanced record (20 bytes) per bin from the
// cull's counts. A bin's instances start in the refs region of its (prototype, pass) after the lower
// windows of the same pair.

// x = bins
uniform float4 gd_args_size;

// 32 bytes: index count, start index, base vertex, refs region, window, bin of window 0, 0, 0
ByteAddressBuffer gd_bin : register(t0);

RWByteAddressBuffer gd_count : register(u0);
RWByteAddressBuffer gd_args : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const uint r = id.x;
	if (r >= (uint)gd_args_size.x)
		return;
	const uint4 b0 = gd_bin.Load4(r * 32);
	const uint2 b1 = gd_bin.Load2(r * 32 + 16);
	uint start = b0.w;
	for (uint w = 0; w < b1.x; ++w)
		start += gd_count.Load((b1.y + w) * 4);
	gd_args.Store4(r * 20, uint4(b0.x, gd_count.Load(r * 4), b0.y, b0.z));
	gd_args.Store(r * 20 + 16, start);
}
