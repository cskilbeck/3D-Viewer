// Edge (line) vertex shader - each line segment is an instance of 6 vertices (two
// triangles) making a strip a few pixels wide on the screen, so the edges can be any
// width and anti-aliased the same way everywhere (the fragment shader fades the sides)

cbuffer Uniforms : register(b0, space1)
{
    float4x4 view;
    float4x4 projection;
    float4 viewport;    // xy = size in pixels, z = line width in pixels
};

struct VSInput
{
    float3 a : TEXCOORD0;    // the ends of the segment (per instance)
    float3 b : TEXCOORD1;
    uint id : SV_VertexID;    // which corner, 0..5
};

struct VSOutput
{
    float4 pos : SV_Position;
    noperspective float across : TEXCOORD0;    // pixels from the middle of the line (in screen space)
};

VSOutput main(VSInput input)
{
    float4 ca = mul(projection, mul(view, float4(input.a, 1.0f)));
    float4 cb = mul(projection, mul(view, float4(input.b, 1.0f)));

    // the part in front of the near plane (or behind the camera) can't go on the screen, cut it
    // off: depth is reversed so the near plane is where z = w. Very long lines (the axes) are
    // too big for floats to find a point that close to the eye, so the cut is a little further
    // away, relative to the size of the line
    float margin = max(abs(ca.w), abs(cb.w)) * 1e-4f;
    float da = ca.w - ca.z - margin;
    float db = cb.w - cb.z - margin;
    if(da <= 0.0f && db <= 0.0f) {
        VSOutput nothing;
        nothing.pos = float4(0, 0, 0, 1);
        nothing.across = 0;
        return nothing;
    }
    if(da < 0.0f) {
        ca = lerp(ca, cb, da / (da - db));
    } else if(db < 0.0f) {
        cb = lerp(cb, ca, db / (db - da));
    }

    // in pixels
    float2 half_size = viewport.xy * 0.5f;
    float2 sa = ca.xy / ca.w * half_size;
    float2 sb = cb.xy / cb.w * half_size;
    float2 along = sb - sa;
    float length_px = length(along);
    along = length_px > 1e-6f ? along / length_px : float2(1, 0);
    float2 side = float2(-along.y, along.x);

    // half the width plus a pixel to fade out in, and the ends pushed out as much (so joins overlap)
    float extent = viewport.z * 0.5f + 1.0f;

    // corners: 0 a- 1 b- 2 b+ 3 a- 4 b+ 5 a+
    uint corner = input.id;
    bool at_b = corner == 1 || corner == 2 || corner == 4;
    float side_sign = (corner == 2 || corner == 4 || corner == 5) ? 1.0f : -1.0f;

    float4 c = at_b ? cb : ca;
    float2 offset = side * side_sign * extent + along * (at_b ? extent : -extent);

    VSOutput output;
    output.pos = float4(c.xy + offset / half_size * c.w, c.z, c.w);
    output.across = side_sign * extent;
    return output;
}
