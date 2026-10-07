// Realistic (PBR) model fragment shader
//
// glTF metallic/roughness materials lit by a built in studio environment (no
// cubemaps): a sky gradient over a darker floor plus a few soft boxes, all in
// model space with Z up. Reflections get blurrier with roughness, then it's
// tone mapped (Khronos PBR Neutral) and converted to sRGB

cbuffer Uniforms : register(b0, space3)
{
    float4 tint;        // rgb = tint color (sRGB), a = how much (0 = none)
    float4 eye;         // xyz = camera position, w = exposure
    float4 emissive;    // rgb = emissive factor, w = alpha mode: 0 = blend, < 0 = opaque (ignore alpha), > 0 = mask cutoff
    float4 material;    // x = metallic, y = roughness, z = 1 if there's a normal map
};

// SDL wants combined image samplers for Vulkan

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> base_color_texture : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState base_color_sampler : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] Texture2D<float4> metallic_roughness_texture : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState metallic_roughness_sampler : register(s1, space2);
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] Texture2D<float4> normal_texture : register(t2, space2);
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] SamplerState normal_sampler : register(s2, space2);
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] Texture2D<float4> occlusion_texture : register(t3, space2);
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] SamplerState occlusion_sampler : register(s3, space2);
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] Texture2D<float4> emissive_texture : register(t4, space2);
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] SamplerState emissive_sampler : register(s4, space2);

struct PSInput
{
    float4 pos : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

//////////////////////////////////////////////////////////////////////

float3 srgb_to_linear(float3 c)
{
    return lerp(pow((c + 0.055f) / 1.055f, 2.4f), c / 12.92f, step(c, 0.04045f));
}

float3 linear_to_srgb(float3 c)
{
    c = saturate(c);
    return lerp(1.055f * pow(c, 1.0f / 2.4f) - 0.055f, c * 12.92f, step(c, 0.0031308f));
}

//////////////////////////////////////////////////////////////////////
// The studio

static const float3 zenith_color = float3(0.25f, 0.28f, 0.33f);
static const float3 horizon_color = float3(0.50f, 0.50f, 0.50f);
static const float3 floor_color = float3(0.06f, 0.057f, 0.054f);

// soft boxes: key (front right, above), fill (left), rim (behind), the default
// view looks from the front (-Y) right (+X) and above
static const int num_boxes = 3;
static const float3 box_directions[3] = { float3(0.45f, -0.55f, 0.70f), float3(-0.85f, -0.25f, 0.45f), float3(0.15f, 0.90f, 0.40f) };
static const float box_sizes[3] = { 0.35f, 0.45f, 0.30f };    // angular radius (radians)
static const float3 box_colors[3] = { float3(6.0f, 6.0f, 5.85f), float3(2.0f, 2.05f, 2.2f), float3(3.0f, 3.0f, 3.0f) };

// what you'd see looking in a direction (without the boxes)
float3 sky(float3 d)
{
    float3 above = lerp(horizon_color, zenith_color, sqrt(saturate(d.z)));
    float3 below = lerp(horizon_color * 0.6f, floor_color, saturate(-d.z * 4.0f));
    return lerp(below, above, smoothstep(-0.03f, 0.03f, d.z));
}

// the sky averaged over the hemisphere around n (irradiance / pi)
float3 sky_irradiance(float3 n)
{
    float3 sky_average = (zenith_color + horizon_color) * 0.5f;
    float3 floor_average = (horizon_color * 0.6f + floor_color) * 0.5f;
    return lerp(floor_average, sky_average, saturate(n.z * 0.5f + 0.5f));
}

// light reflected in direction r from a surface with this roughness (blurred environment)
float3 environment_specular(float3 r, float roughness)
{
    float a = roughness * roughness;
    float3 color = lerp(sky(r), sky_irradiance(r), saturate(roughness * 1.3f));
    for(int i = 0; i < num_boxes; ++i) {
        float3 l = normalize(box_directions[i]);
        float size = box_sizes[i];
        float width = size + a * 2.0f;
        float angle = acos(clamp(dot(r, l), -1.0f, 1.0f));
        float k = 1.0f - smoothstep(0.75f, 1.0f, angle / width);
        color += box_colors[i] * k * (size * size) / (width * width);
    }
    return color;
}

// diffuse light arriving at a surface facing n (irradiance / pi)
float3 environment_diffuse(float3 n)
{
    float3 color = sky_irradiance(n);
    for(int i = 0; i < num_boxes; ++i) {
        float3 l = normalize(box_directions[i]);
        color += box_colors[i] * box_sizes[i] * box_sizes[i] * saturate(dot(n, l));
    }
    return color;
}

// split sum approximation of the specular BRDF integrated over the environment (Karis)
float2 environment_brdf(float n_dot_v, float roughness)
{
    float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * n_dot_v)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

//////////////////////////////////////////////////////////////////////
// Khronos PBR Neutral tone mapping, keeps base colors as they are up to ~0.8

float3 tone_map(float3 color)
{
    float const start_compression = 0.8f - 0.04f;
    float const desaturation = 0.15f;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if(peak < start_compression) {
        return color;
    }
    float const d = 1.0f - start_compression;
    float new_peak = 1.0f - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0f - 1.0f / (desaturation * (peak - new_peak) + 1.0f);
    return lerp(color, new_peak.xxx, g);
}

//////////////////////////////////////////////////////////////////////

float4 main(PSInput input) : SV_Target
{
    float4 base_sample = base_color_texture.Sample(base_color_sampler, input.uv);
    float3 base_color = srgb_to_linear(input.color.rgb) * base_sample.rgb;
    float alpha = input.color.a * base_sample.a;

    if(emissive.w > 0.0f) {
        if(alpha < emissive.w) {
            discard;
        }
        alpha = 1.0f;
    } else if(emissive.w < 0.0f) {
        alpha = 1.0f;
    }

    float4 mr = metallic_roughness_texture.Sample(metallic_roughness_sampler, input.uv);
    float metallic = saturate(material.x * mr.b);
    float roughness = clamp(material.y * mr.g, 0.03f, 1.0f);
    float occlusion = occlusion_texture.Sample(occlusion_sampler, input.uv).r;
    float3 emitted = emissive.rgb * emissive_texture.Sample(emissive_sampler, input.uv).rgb;

    // selected: towards the tint color, and not metallic so the color shows
    base_color = lerp(base_color, srgb_to_linear(tint.rgb), tint.a);
    metallic *= 1.0f - tint.a;
    emitted *= 1.0f - tint.a;

    float3 v = normalize(eye.xyz - input.world_pos);
    float3 n = normalize(input.normal);

    // shells aren't always consistently oriented, light both sides
    if(dot(n, v) < 0.0f) {
        n = -n;
    }

    // normal map, tangent frame from the screen space derivatives (no tangents needed)
    if(material.z > 0.0f) {
        float3 dp1 = ddx(input.world_pos);
        float3 dp2 = ddy(input.world_pos);
        float2 duv1 = ddx(input.uv);
        float2 duv2 = ddy(input.uv);
        float3 dp2_perp = cross(dp2, n);
        float3 dp1_perp = cross(n, dp1);
        float3 t = dp2_perp * duv1.x + dp1_perp * duv2.x;    // along +u
        float3 b = dp2_perp * duv1.y + dp1_perp * duv2.y;    // along +v (down the image)
        float scale = max(dot(t, t), dot(b, b));
        if(scale > 1e-20f) {
            scale = rsqrt(scale);
            float3 m = normal_texture.Sample(normal_sampler, input.uv).xyz * 2.0f - 1.0f;
            // normal maps have +Y up the image
            float3 mapped = t * scale * m.x - b * scale * m.y + n * m.z;
            if(dot(mapped, mapped) > 1e-12f) {
                n = normalize(mapped);
            }
        }
    }

    float n_dot_v = max(dot(n, v), 1e-4f);
    float3 r = reflect(-v, n);

    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), base_color, metallic);
    float2 brdf = environment_brdf(n_dot_v, roughness);
    float3 specular = environment_specular(r, roughness) * (f0 * brdf.x + brdf.y);
    float3 diffuse = base_color * (1.0f - metallic) * environment_diffuse(n);

    float3 color = (diffuse + specular) * occlusion + emitted;
    color = tone_map(color * eye.w);
    // premultiplied alpha
    return float4(linear_to_srgb(color) * alpha, alpha);
}
