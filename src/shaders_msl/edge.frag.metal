// Edge (line) fragment shader - flat color, the sides fading out over a pixel (premultiplied alpha)

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 color;
    float4 line_width;    // x = width in pixels
};

struct PSInput
{
    float4 pos [[position]];
    float across [[center_no_perspective]];    // pixels from the middle of the line (in screen space)
};

fragment float4 main0(
    PSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    float coverage = saturate(u.line_width.x * 0.5 + 0.5 - abs(in.across));
    if(coverage <= 0.0) {
        discard_fragment();
    }
    float alpha = u.color.a * coverage;
    return float4(u.color.rgb * alpha, alpha);
}
