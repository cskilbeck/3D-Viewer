// Grid fragment shader - antialiased lines, every 10th one stronger, fading out with distance

cbuffer Uniforms : register(b0, space3)
{
    float4 color;
    float4 grid;    // x = spacing, yz = offset so lines land on multiples of spacing in file coordinates, w = unused
    float4 fade;    // xy = middle, z = radius where it's gone
};

struct PSInput
{
    float4 pos : SV_Position;
    float2 plane_pos : TEXCOORD0;
};

// 1 on a line, 0 away from it (about a pixel wide), fades out when the cells get too small to see
float lines(float2 coord)
{
    float2 width = fwidth(coord);
    float2 g = abs(frac(coord - 0.5f) - 0.5f) / width;
    float line_alpha = 1.0f - min(min(g.x, g.y), 1.0f);
    return line_alpha * (1.0f - smoothstep(0.15f, 0.5f, max(width.x, width.y)));
}

float4 main(PSInput input) : SV_Target
{
    float2 p = input.plane_pos + grid.yz;
    float minor = lines(p / grid.x);
    float major = lines(p / (grid.x * 10.0f));
    float alpha = max(minor * 0.5f, major);
    float distance_fade = 1.0f - smoothstep(fade.z * 0.4f, fade.z, length(input.plane_pos - fade.xy));
    return float4(color.rgb, color.a * alpha * distance_fade);
}
