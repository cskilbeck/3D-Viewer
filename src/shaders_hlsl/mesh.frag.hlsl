// Shaded model fragment shader - headlight, two sided

cbuffer Uniforms : register(b0, space3)
{
    float4 tint;    // rgb = tint color, a = how much (0 = none)
};

struct PSInput
{
    float4 pos : SV_Position;
    float3 view_pos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target
{
    float3 n = normalize(input.normal);
    float3 v = normalize(-input.view_pos);

    // STEP shells aren't always consistently oriented, light both sides
    if(dot(n, v) < 0.0f) {
        n = -n;
    }

    // light comes from just above and to the right of the camera
    float3 l = normalize(float3(0.3f, 0.4f, 1.0f));
    float diffuse = saturate(dot(n, l));
    float specular = pow(saturate(dot(n, normalize(l + v))), 48.0f) * 0.25f;

    float3 base = lerp(input.color.rgb, tint.rgb, tint.a);
    float3 color = base * (0.3f + 0.7f * diffuse) + specular;
    // premultiplied alpha
    return float4(color * input.color.a, input.color.a);
}
