// Shaded model fragment shader - headlight, two sided

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 tint;    // rgb = tint color, a = how much (0 = none)
};

struct PSInput
{
    float4 pos [[position]];
    float3 view_pos;
    float3 normal;
    float4 color;
};

fragment float4 main0(
    PSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]])
{
    float3 n = normalize(in.normal);
    float3 v = normalize(-in.view_pos);

    // STEP shells aren't always consistently oriented, light both sides
    if(dot(n, v) < 0.0) {
        n = -n;
    }

    // light comes from just above and to the right of the camera
    float3 l = normalize(float3(0.3, 0.4, 1.0));
    float diffuse = saturate(dot(n, l));
    float specular = pow(saturate(dot(n, normalize(l + v))), 48.0) * 0.25;

    float3 base = mix(in.color.rgb, u.tint.rgb, u.tint.a);
    float3 color = base * (0.3 + 0.7 * diffuse) + specular;
    // premultiplied alpha
    return float4(color * in.color.a, in.color.a);
}
