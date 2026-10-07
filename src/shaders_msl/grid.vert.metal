// Grid vertex shader - a big square on the XY plane

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4x4 view;
    float4x4 projection;
    float4 plane;    // xy = middle of the square, z = height of the plane, w = half the size of the square
};

struct VSInput
{
    float2 corner [[attribute(0)]];    // -1..1
};

struct VSOutput
{
    float4 pos [[position]];
    float2 plane_pos;
};

vertex VSOutput main0(
    VSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    VSOutput out;
    float2 xy = u.plane.xy + in.corner * u.plane.w;
    out.pos = u.projection * (u.view * float4(xy, u.plane.z, 1.0));
    out.plane_pos = xy;
    return out;
}
