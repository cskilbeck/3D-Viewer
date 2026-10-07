// Grid vertex shader - a big square on the XY plane

cbuffer Uniforms : register(b0, space1)
{
    float4x4 view;
    float4x4 projection;
    float4 plane;    // xy = middle of the square, z = height of the plane, w = half the size of the square
};

struct VSInput
{
    float2 corner : TEXCOORD0;    // -1..1
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 plane_pos : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float2 xy = plane.xy + input.corner * plane.w;
    output.pos = mul(projection, mul(view, float4(xy, plane.z, 1.0f)));
    output.plane_pos = xy;
    return output;
}
