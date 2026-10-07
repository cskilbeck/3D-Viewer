// Realistic (PBR) model vertex shader - everything stays in model space for lighting

cbuffer Uniforms : register(b0, space1)
{
    float4x4 view;
    float4x4 projection;
};

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;    // UBYTE4_NORM, sRGB
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.pos = mul(projection, mul(view, float4(input.position, 1.0f)));
    output.world_pos = input.position;
    output.normal = input.normal;
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
