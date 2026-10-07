// Shaded model vertex shader

cbuffer Uniforms : register(b0, space1)
{
    float4x4 view;
    float4x4 projection;
};

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : TEXCOORD2;    // UBYTE4_NORM
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 view_pos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 view_pos = mul(view, float4(input.position, 1.0f));
    output.pos = mul(projection, view_pos);
    output.view_pos = view_pos.xyz;
    output.normal = mul((float3x3)view, input.normal);
    output.color = input.color;
    return output;
}
