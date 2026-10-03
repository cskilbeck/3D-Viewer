// Shaded model vertex shader

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4x4 view;
    float4x4 projection;
};

struct VSInput
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float4 color [[attribute(2)]];    // UBYTE4_NORM
};

struct VSOutput
{
    float4 pos [[position]];
    float3 view_pos;
    float3 normal;
    float4 color;
};

vertex VSOutput main0(
    VSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    VSOutput out;
    float4 view_pos = u.view * float4(in.position, 1.0);
    out.pos = u.projection * view_pos;
    out.view_pos = view_pos.xyz;
    out.normal = float3x3(u.view[0].xyz, u.view[1].xyz, u.view[2].xyz) * in.normal;
    out.color = in.color;
    return out;
}
