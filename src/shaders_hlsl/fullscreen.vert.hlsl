// A triangle which covers the viewport, for compositing (no vertex buffer, draw 3 vertices)

float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
}
