// Edge (line) vertex shader - each line segment is an instance of 6 vertices (two
// triangles) making a strip a few pixels wide on the screen, so the edges can be any
// width and anti-aliased the same way everywhere (the fragment shader fades the sides)

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4x4 view;
    float4x4 projection;
    float4 viewport;    // xy = size in pixels, z = line width in pixels
};

struct VSInput
{
    float3 a [[attribute(0)]];    // the ends of the segment (per instance)
    float3 b [[attribute(1)]];
};

struct VSOutput
{
    float4 pos [[position]];
    float across [[center_no_perspective]];    // pixels from the middle of the line (in screen space)
};

vertex VSOutput main0(
    VSInput in [[stage_in]],
    uint id [[vertex_id]],
    constant Uniforms& u [[buffer(0)]])
{
    float4 ca = u.projection * (u.view * float4(in.a, 1.0));
    float4 cb = u.projection * (u.view * float4(in.b, 1.0));

    // the part in front of the near plane (or behind the camera) can't go on the screen, cut it
    // off: depth is reversed so the near plane is where z = w. Very long lines (the axes) are
    // too big for floats to find a point that close to the eye, so the cut is a little further
    // away, relative to the size of the line
    float margin = max(abs(ca.w), abs(cb.w)) * 1e-4;
    float da = ca.w - ca.z - margin;
    float db = cb.w - cb.z - margin;
    if(da <= 0.0 && db <= 0.0) {
        VSOutput nothing;
        nothing.pos = float4(0, 0, 0, 1);
        nothing.across = 0;
        return nothing;
    }
    if(da < 0.0) {
        ca = mix(ca, cb, da / (da - db));
    } else if(db < 0.0) {
        cb = mix(cb, ca, db / (db - da));
    }

    // in pixels
    float2 half_size = u.viewport.xy * 0.5;
    float2 sa = ca.xy / ca.w * half_size;
    float2 sb = cb.xy / cb.w * half_size;
    float2 along = sb - sa;
    float length_px = length(along);
    along = length_px > 1e-6 ? along / length_px : float2(1, 0);
    float2 side = float2(-along.y, along.x);

    // half the width plus a pixel to fade out in, and the ends pushed out as much (so joins overlap)
    float extent = u.viewport.z * 0.5 + 1.0;

    // corners: 0 a- 1 b- 2 b+ 3 a- 4 b+ 5 a+
    uint corner = id % 6;
    bool at_b = corner == 1 || corner == 2 || corner == 4;
    float side_sign = (corner == 2 || corner == 4 || corner == 5) ? 1.0 : -1.0;

    float4 c = at_b ? cb : ca;
    float2 offset = side * side_sign * extent + along * (at_b ? extent : -extent);

    VSOutput out;
    out.pos = float4(c.xy + offset / half_size * c.w, c.z, c.w);
    out.across = side_sign * extent;
    return out;
}
