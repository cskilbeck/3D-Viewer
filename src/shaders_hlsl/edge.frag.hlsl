// Edge (line) fragment shader - flat color, the sides fading out over a pixel (premultiplied alpha)

cbuffer Uniforms : register(b0, space3)
{
    float4 color;
    float4 line_width;    // x = width in pixels
};

struct PSInput
{
    float4 pos : SV_Position;
    noperspective float across : TEXCOORD0;    // pixels from the middle of the line (in screen space)
};

float4 main(PSInput input) : SV_Target
{
    float coverage = saturate(line_width.x * 0.5f + 0.5f - abs(input.across));
    if(coverage <= 0.0f) {
        discard;
    }
    float alpha = color.a * coverage;
    return float4(color.rgb * alpha, alpha);
}
