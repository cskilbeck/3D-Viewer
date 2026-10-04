// Grid fragment shader - antialiased lines, every 10th one stronger, fading out with distance

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 color;
    float4 grid;    // x = spacing, yz = offset so lines land on multiples of spacing in file coordinates, w = unused
    float4 fade;    // xy = middle, z = radius where it's gone
};

struct PSInput
{
    float4 pos [[position]];
    float2 plane_pos;
};

// 1 on a line, 0 away from it (about a pixel wide), fades out when the cells get too small to see
static float lines(float2 coord)
{
    float2 width = fwidth(coord);
    float2 g = abs(fract(coord - 0.5) - 0.5) / width;
    float line_alpha = 1.0 - min(min(g.x, g.y), 1.0);
    return line_alpha * (1.0 - smoothstep(0.15, 0.5, max(width.x, width.y)));
}

fragment float4 main0(
    PSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    float2 p = in.plane_pos + u.grid.yz;
    float minor = lines(p / u.grid.x);
    float major = lines(p / (u.grid.x * 10.0));
    float alpha = max(minor * 0.5, major);
    float distance_fade = 1.0 - smoothstep(u.fade.z * 0.4, u.fade.z, length(in.plane_pos - u.fade.xy));
    return float4(u.color.rgb, u.color.a * alpha * distance_fade);
}
