// Edge (line) fragment shader - flat color

cbuffer Uniforms : register(b0, space3)
{
    float4 color;
};

struct PSInput
{
    float4 pos : SV_Position;
};

float4 main(PSInput input) : SV_Target
{
    return color;
}
