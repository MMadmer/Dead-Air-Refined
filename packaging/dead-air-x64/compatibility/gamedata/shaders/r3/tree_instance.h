#ifndef TREE_INSTANCE_H
#define TREE_INSTANCE_H

#define TREE_INSTANCE_VECTOR_COUNT 9
#define TREE_INSTANCE_MAX_COUNT 64

cbuffer tree_instance_data_buffer
{
    float4 tree_instance_data[TREE_INSTANCE_MAX_COUNT * TREE_INSTANCE_VECTOR_COUNT];
}

cbuffer tree_instance_control_buffer
{
    float4 tree_instance_control;
}

uint tree_instance_index(uint instance_id)
{
    return instance_id + (uint)tree_instance_control.y;
}

float3x4 tree_instance_xform(uint instance_id)
{
    uint base = tree_instance_index(instance_id) * TREE_INSTANCE_VECTOR_COUNT;
    return float3x4(tree_instance_data[base], tree_instance_data[base + 1], tree_instance_data[base + 2]);
}

float3x4 tree_instance_xform_v(uint instance_id)
{
    uint base = tree_instance_index(instance_id) * TREE_INSTANCE_VECTOR_COUNT + 3;
    return float3x4(tree_instance_data[base], tree_instance_data[base + 1], tree_instance_data[base + 2]);
}

float4 tree_instance_scale(uint instance_id)
{
    return tree_instance_data[tree_instance_index(instance_id) * TREE_INSTANCE_VECTOR_COUNT + 6];
}

float4 tree_instance_bias(uint instance_id)
{
    return tree_instance_data[tree_instance_index(instance_id) * TREE_INSTANCE_VECTOR_COUNT + 7];
}

float2 tree_instance_sun(uint instance_id)
{
    return tree_instance_data[tree_instance_index(instance_id) * TREE_INSTANCE_VECTOR_COUNT + 8].xy;
}

#endif
