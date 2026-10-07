// A triangle which covers the viewport, for compositing (no vertex buffer, draw 3 vertices)

#include <metal_stdlib>
using namespace metal;

vertex float4 main0(uint id [[vertex_id]])
{
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}
