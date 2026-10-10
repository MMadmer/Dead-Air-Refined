// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Top-down raster of the level's collision mesh into the near ground window (da_ground.cpp). One
// thread per work item: a triangle and a 32 x 32 chunk of the rectangle being rebuilt. Coverage is
// conservative (a texel the triangle touches anywhere is in) and so is the height: the plane's maximum
// over the texel square, clamped to the triangle, so bilinear reads never dip under the real surface.
// Non-passable triangles go to the solid map, water (liquid and passable) to the water map; the other
// passable materials (crowns, bushes, volumes) are skipped. Heights are stored order-preserving
// (see da_ground_ord) and merged with InterlockedMax: the topmost surface wins. The low map keeps the
// lowest surface facing up at each texel centre (the plane's value there, the complement of its order so
// that InterlockedMax picks the least): the ground under a roof, a deck or a vehicle.

ByteAddressBuffer s_gr_verts : register(t8); // float3 per vertex
ByteAddressBuffer s_gr_tris : register(t9);  // uint4 per triangle: i0, i1, i2, flags (da_ground.h)
ByteAddressBuffer s_gr_work : register(t10); // uint2 per item: triangle, chunk origin (x | z << 16) in the rect

RWTexture2D<uint> u_solid : register(u0);
RWTexture2D<uint> u_water : register(u1);
RWTexture2D<uint> u_low : register(u2);

uniform float4 da_gr_rect; // world texels x0, z0, x1, z1 (exclusive) being rebuilt
uniform float4 da_gr_win;  // x texel (m), y texels per metre, z texels per side, w item count
uniform float4 da_gr_base; // x first item of this dispatch

#define DA_GR_CHUNK 32
#define DA_GR_PASSABLE (1u << 16)
#define DA_GR_WATER (1u << 17)

// Floats as uints with the same order; 0 stays free for "nothing here".
uint da_ground_ord(float f)
{
	const uint u = asuint(f);
	return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

float da_gr_cross(float2 a, float2 b)
{
	return a.x * b.y - a.y * b.x;
}

float da_gr_segment_dist(float2 p, float2 a, float2 b)
{
	const float2 ab = b - a;
	const float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-12));
	return length(p - (a + ab * t));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const uint item = id.x + uint(da_gr_base.x);
	[branch] if (item >= uint(da_gr_win.w))
		return;
	const uint2 work = s_gr_work.Load2(item * 8);
	const uint4 tri = s_gr_tris.Load4(work.x * 16);
	const bool water = (tri.w & DA_GR_WATER) != 0;
	[branch] if ((tri.w & DA_GR_PASSABLE) != 0 && !water)
		return;

	const float3 a = asfloat(s_gr_verts.Load3(tri.x * 12));
	const float3 b = asfloat(s_gr_verts.Load3(tri.y * 12));
	const float3 c = asfloat(s_gr_verts.Load3(tri.z * 12));
	const float texel = da_gr_win.x;
	const float inv = da_gr_win.y;
	const int n = int(da_gr_win.z);

	// The chunk, inside the rectangle, inside the footprint grown by half a texel diagonal.
	const float grow = 0.70711 * texel;
	const float2 fmin = min(a.xz, min(b.xz, c.xz)) - grow;
	const float2 fmax = max(a.xz, max(b.xz, c.xz)) + grow;
	const int2 r1 = int2(da_gr_rect.zw);
	const int2 c0 = int2(da_gr_rect.xy) + int2(work.y & 0xFFFFu, work.y >> 16);
	const int2 lo = max(c0, int2(floor(fmin * inv)));
	const int2 hi = min(min(c0 + DA_GR_CHUNK, r1), int2(floor(fmax * inv)) + 1);
	[branch] if (lo.x >= hi.x || lo.y >= hi.y)
		return;

	const float ymax = max(a.y, max(b.y, c.y));
	const float ymin = min(a.y, min(b.y, c.y));
	const float2 e0 = b.xz - a.xz;
	const float2 e1 = c.xz - b.xz;
	const float2 e2 = a.xz - c.xz;
	const float area = da_gr_cross(e0, c.xz - a.xz);
	// A triangle seen edge-on from above (a wall): its footprint is its edges, its height the top.
	const bool edge_on = abs(area) < 1e-4 * max(dot(e0, e0) + dot(e1, e1) + dot(e2, e2), 1e-6);

	// Inside test against the edge lines moved out by the grow distance (normalised, orientation-free).
	const float s = (area < 0.0) ? -1.0 : 1.0;
	const float3 il = float3(rsqrt(max(dot(e0, e0), 1e-12)), rsqrt(max(dot(e1, e1), 1e-12)), rsqrt(max(dot(e2, e2), 1e-12))) * s;
	// The plane y = a.y + g.x (x - a.x) + g.y (z - a.z) and its rise over half a texel in each axis.
	const float2 ab = b.xz - a.xz;
	const float2 ac = c.xz - a.xz;
	const float inv_area = edge_on ? 0.0 : 1.0 / area;
	const float2 g = float2((b.y - a.y) * ac.y - (c.y - a.y) * ab.y, (c.y - a.y) * ab.x - (b.y - a.y) * ac.x) * inv_area;
	const float rise = (abs(g.x) + abs(g.y)) * texel * 0.5;
	const uint top = da_ground_ord(ymax);
	// A floor for the low map: facing up (under 72 degrees), solid. Walls and steep banks are no floor.
	const bool floor_face = !water && !edge_on && dot(g, g) < 9.0;

	[loop] for (int z = lo.y; z < hi.y; ++z)
	{
		[loop] for (int x = lo.x; x < hi.x; ++x)
		{
			const float2 p = (float2(x, z) + 0.5) * texel;
			uint h = top;
			[branch] if (edge_on)
			{
				const float d = min(da_gr_segment_dist(p, a.xz, b.xz), min(da_gr_segment_dist(p, b.xz, c.xz), da_gr_segment_dist(p, c.xz, a.xz)));
				[branch] if (d > grow)
					continue;
			}
			else
			{
				const float3 ed = float3(da_gr_cross(e0, p - a.xz), da_gr_cross(e1, p - b.xz), da_gr_cross(e2, p - c.xz)) * il;
				[branch] if (min(ed.x, min(ed.y, ed.z)) < -grow)
					continue;
				const float y = a.y + dot(g, p - a.xz) + rise;
				h = da_ground_ord(clamp(y, ymin, ymax));
				// the centre inside the triangle (a quarter texel of slack): its plane there
				[branch] if (floor_face && min(ed.x, min(ed.y, ed.z)) > -0.25 * texel)
				{
					const int2 la = int2(uint2(int2(x, z) + n * 256) % uint(n));
					InterlockedMax(u_low[la], ~da_ground_ord(clamp(y - rise, ymin, ymax)));
				}
			}
			const int2 addr = int2(uint2(int2(x, z) + n * 256) % uint(n));
			[branch] if (water)
				InterlockedMax(u_water[addr], h);
			else
				InterlockedMax(u_solid[addr], h);
		}
	}
}
