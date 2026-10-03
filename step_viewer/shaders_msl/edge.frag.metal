// Edge (line) fragment shader - flat color

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 color;
};

struct PSInput
{
    float4 pos [[position]];
};

fragment float4 main0(
    PSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    return u.color;
}
