// The peeled transparent layers (premultiplied alpha), blended over the opaque image

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> layers : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState layers_sampler : register(s0, space2);

struct PSInput
{
    float4 pos : SV_Position;
};

float4 main(PSInput input) : SV_Target
{
    return layers.Load(int3(input.pos.xy, 0));
}
