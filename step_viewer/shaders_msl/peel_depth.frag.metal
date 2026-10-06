// Depth peeling: finds the next layer of transparent surfaces. The depth test keeps the
// nearest of what's left after throwing away what's been peeled already (at or in front
// of the previous layer) and what's hidden by the opaque surfaces. Depth is reversed,
// nearer is bigger

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 layer;    // x = 1 for the first layer (nothing peeled yet)
};

struct PSInput
{
    float4 pos [[position]];
};

fragment void main0(
    PSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]],
    depth2d<float> previous_depth [[texture(0)]],
    sampler previous_sampler [[sampler(0)]],
    depth2d<float> opaque_depth [[texture(1)]],
    sampler opaque_sampler [[sampler(1)]])
{
    uint2 pixel = uint2(in.pos.xy);
    if(in.pos.z <= opaque_depth.read(pixel)) {
        discard_fragment();    // behind something opaque
    }
    if(u.layer.x == 0.0 && in.pos.z >= previous_depth.read(pixel)) {
        discard_fragment();    // peeled already
    }
}
