// Realistic (PBR) model vertex shader - everything stays in model space for lighting

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
    float2 uv [[attribute(2)]];
    float4 color [[attribute(3)]];    // UBYTE4_NORM, sRGB
};

struct VSOutput
{
    float4 pos [[position]];
    float3 world_pos;
    float3 normal;
    float2 uv;
    float4 color;
};

vertex VSOutput main0(
    VSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    VSOutput out;
    out.pos = u.projection * (u.view * float4(in.position, 1.0));
    out.world_pos = in.position;
    out.normal = in.normal;
    out.uv = in.uv;
    out.color = in.color;
    return out;
}
