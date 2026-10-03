// Edge (line) vertex shader

cbuffer Uniforms : register(b0, space1)
{
    float4x4 view;
    float4x4 projection;
};

struct VSInput
{
    float3 position : TEXCOORD0;
};

struct VSOutput
{
    float4 pos : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.pos = mul(projection, mul(view, float4(input.position, 1.0f)));
    return output;
}
