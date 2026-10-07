// Depth peeling: finds the next layer of transparent surfaces. The depth test keeps the
// nearest of what's left after throwing away what's been peeled already (at or in front
// of the previous layer) and what's hidden by the opaque surfaces. Depth is reversed,
// nearer is bigger

cbuffer Uniforms : register(b0, space3)
{
    float4 layer;    // x = 1 for the first layer (nothing peeled yet)
};

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float> previous_depth : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState previous_sampler : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] Texture2D<float> opaque_depth : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState opaque_sampler : register(s1, space2);

struct PSInput
{
    float4 pos : SV_Position;
};

void main(PSInput input)
{
    int3 pixel = int3(input.pos.xy, 0);
    if(input.pos.z <= opaque_depth.Load(pixel)) {
        discard;    // behind something opaque
    }
    if(layer.x == 0.0f && input.pos.z >= previous_depth.Load(pixel)) {
        discard;    // peeled already
    }
}
