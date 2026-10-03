// Edge (line) vertex shader

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
};

struct VSOutput
{
    float4 pos [[position]];
};

vertex VSOutput main0(
    VSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    VSOutput out;
    out.pos = u.projection * (u.view * float4(in.position, 1.0));
    return out;
}
