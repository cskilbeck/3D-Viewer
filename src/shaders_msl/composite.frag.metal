// The peeled transparent layers (premultiplied alpha), blended over the opaque image

#include <metal_stdlib>
using namespace metal;

struct PSInput
{
    float4 pos [[position]];
};

fragment float4 main0(
    PSInput in [[stage_in]],
    texture2d<float> layers [[texture(0)]],
    sampler layers_sampler [[sampler(0)]])
{
    return layers.read(uint2(in.pos.xy));
}
