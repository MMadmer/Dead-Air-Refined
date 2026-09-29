#ifndef DA_GD_TREE_H
#define DA_GD_TREE_H

// GPU-driven trees (gpudriven.md 3.3-3.4): a tree vertex shader compiled with DA_TREE_GD reads its
// instance rows from the registry instead of the per-object constants or tree_instance_data. The
// instance comes per instance in GDREF (IA slot 1); the rows equal FillInstanceData's.
//
// da_gd_inst, 96 bytes per instance: world rows 0-2 (as tree_instance_xform), c_scale, c_bias
// (rgb, hemi, before ps_r__Tree_SBC), then (sun scale, sun bias, root index, flags; bit 0 = the
// instance is its root's wind sample point).
// da_gd_root, 48 bytes per root: (state, frequency factor, gust, deviation) of this frame, the same
// of the previous frame, (height, foliage gate, integration state x2).
ByteAddressBuffer da_gd_inst : register(t2);
ByteAddressBuffer da_gd_root : register(t3);
// x = ps_r__Tree_SBC * 1.3333
uniform float4 da_gd_params;

#define DA_GD_INST_STRIDE 96
#define DA_GD_ROOT_STRIDE 48

struct da_gd_rows
{
    float3x4 xform;
    float3x4 xform_v;
    float4 c_scale;
    float4 c_bias;
    float4 c_sun;
    float4 c_tree;
    float4 c_prev;
};

da_gd_rows da_gd_tree_rows(uint ref)
{
    const uint a = ref * DA_GD_INST_STRIDE;
    const float4 r0 = asfloat(da_gd_inst.Load4(a));
    const float4 r1 = asfloat(da_gd_inst.Load4(a + 16));
    const float4 r2 = asfloat(da_gd_inst.Load4(a + 32));
    const float4 r3 = asfloat(da_gd_inst.Load4(a + 48));
    const float4 r4 = asfloat(da_gd_inst.Load4(a + 64));
    const uint4 r5 = da_gd_inst.Load4(a + 80);

    const uint b = r5.z * DA_GD_ROOT_STRIDE;
    const float4 now = asfloat(da_gd_root.Load4(b));
    const float4 prev = asfloat(da_gd_root.Load4(b + 16));
    const float2 height_gate = asfloat(da_gd_root.Load2(b + 32));

    da_gd_rows o;
    o.xform = float3x4(r0, r1, r2);
    // Only the rotation of the world-view rows is read (normals and tangent frames).
    const float3x3 xv = mul((float3x3)m_V, (float3x3)o.xform);
    o.xform_v = float3x4(float4(xv[0], 0), float4(xv[1], 0), float4(xv[2], 0));
    const float sbc = da_gd_params.x;
    o.c_scale = sbc * r3;
    o.c_bias = sbc * r4;
    o.c_sun = float4(sbc * asfloat(r5.x), sbc * asfloat(r5.y), now.x, now.y);
    o.c_tree = float4(height_gate.x, now.z, now.w, ((r5.w & 1u) ? 3.0f : 1.0f) + height_gate.y);
    o.c_prev = prev;
    return o;
}

#endif
