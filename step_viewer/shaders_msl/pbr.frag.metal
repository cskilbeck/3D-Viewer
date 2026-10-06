// Realistic (PBR) model fragment shader
//
// glTF metallic/roughness materials lit by a built in studio environment (no
// cubemaps): a sky gradient over a darker floor plus a few soft boxes, all in
// model space with Z up. Reflections get blurrier with roughness, then it's
// tone mapped (Khronos PBR Neutral) and converted to sRGB

#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float4 tint;        // rgb = tint color (sRGB), a = how much (0 = none)
    float4 eye;         // xyz = camera position, w = exposure
    float4 emissive;    // rgb = emissive factor, w = alpha mode: 0 = blend, < 0 = opaque (ignore alpha), > 0 = mask cutoff
    float4 material;    // x = metallic, y = roughness, z = 1 if there's a normal map
};

struct PSInput
{
    float4 pos [[position]];
    float3 world_pos;
    float3 normal;
    float2 uv;
    float4 color;
};

//////////////////////////////////////////////////////////////////////

static float3 srgb_to_linear(float3 c)
{
    return mix(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, float3(0.04045)));
}

static float3 linear_to_srgb(float3 c)
{
    c = saturate(c);
    return mix(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, float3(0.0031308)));
}

//////////////////////////////////////////////////////////////////////
// The studio

constant float3 zenith_color = float3(0.25, 0.28, 0.33);
constant float3 horizon_color = float3(0.50, 0.50, 0.50);
constant float3 floor_color = float3(0.06, 0.057, 0.054);

// soft boxes: key (front right, above), fill (left), rim (behind), the default
// view looks from the front (-Y) right (+X) and above
constant int num_boxes = 3;
constant float3 box_directions[3] = { float3(0.45, -0.55, 0.70), float3(-0.85, -0.25, 0.45), float3(0.15, 0.90, 0.40) };
constant float box_sizes[3] = { 0.35, 0.45, 0.30 };    // angular radius (radians)
constant float3 box_colors[3] = { float3(6.0, 6.0, 5.85), float3(2.0, 2.05, 2.2), float3(3.0, 3.0, 3.0) };

// what you'd see looking in a direction (without the boxes)
static float3 sky(float3 d)
{
    float3 above = mix(horizon_color, zenith_color, sqrt(saturate(d.z)));
    float3 below = mix(horizon_color * 0.6, floor_color, saturate(-d.z * 4.0));
    return mix(below, above, smoothstep(-0.03, 0.03, d.z));
}

// the sky averaged over the hemisphere around n (irradiance / pi)
static float3 sky_irradiance(float3 n)
{
    float3 sky_average = (zenith_color + horizon_color) * 0.5;
    float3 floor_average = (horizon_color * 0.6 + floor_color) * 0.5;
    return mix(floor_average, sky_average, saturate(n.z * 0.5 + 0.5));
}

// light reflected in direction r from a surface with this roughness (blurred environment)
static float3 environment_specular(float3 r, float roughness)
{
    float a = roughness * roughness;
    float3 color = mix(sky(r), sky_irradiance(r), saturate(roughness * 1.3));
    for(int i = 0; i < num_boxes; ++i) {
        float3 l = normalize(box_directions[i]);
        float size = box_sizes[i];
        float width = size + a * 2.0;
        float angle = acos(clamp(dot(r, l), -1.0, 1.0));
        float k = 1.0 - smoothstep(0.75, 1.0, angle / width);
        color += box_colors[i] * k * (size * size) / (width * width);
    }
    return color;
}

// diffuse light arriving at a surface facing n (irradiance / pi)
static float3 environment_diffuse(float3 n)
{
    float3 color = sky_irradiance(n);
    for(int i = 0; i < num_boxes; ++i) {
        float3 l = normalize(box_directions[i]);
        color += box_colors[i] * box_sizes[i] * box_sizes[i] * saturate(dot(n, l));
    }
    return color;
}

// split sum approximation of the specular BRDF integrated over the environment (Karis)
static float2 environment_brdf(float n_dot_v, float roughness)
{
    float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

//////////////////////////////////////////////////////////////////////
// Khronos PBR Neutral tone mapping, keeps base colors as they are up to ~0.8

static float3 tone_map(float3 color)
{
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if(peak < start_compression) {
        return color;
    }
    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, float3(new_peak), g);
}

//////////////////////////////////////////////////////////////////////

fragment float4 main0(
    PSInput in [[stage_in]],
    constant Uniforms& u [[buffer(0)]],
    texture2d<float> base_color_texture [[texture(0)]],
    sampler base_color_sampler [[sampler(0)]],
    texture2d<float> metallic_roughness_texture [[texture(1)]],
    sampler metallic_roughness_sampler [[sampler(1)]],
    texture2d<float> normal_texture [[texture(2)]],
    sampler normal_sampler [[sampler(2)]],
    texture2d<float> occlusion_texture [[texture(3)]],
    sampler occlusion_sampler [[sampler(3)]],
    texture2d<float> emissive_texture [[texture(4)]],
    sampler emissive_sampler [[sampler(4)]])
{
    float4 base_sample = base_color_texture.sample(base_color_sampler, in.uv);
    float3 base_color = srgb_to_linear(in.color.rgb) * base_sample.rgb;
    float alpha = in.color.a * base_sample.a;

    if(u.emissive.w > 0.0) {
        if(alpha < u.emissive.w) {
            discard_fragment();
        }
        alpha = 1.0;
    } else if(u.emissive.w < 0.0) {
        alpha = 1.0;
    }

    float4 mr = metallic_roughness_texture.sample(metallic_roughness_sampler, in.uv);
    float metallic = saturate(u.material.x * mr.b);
    float roughness = clamp(u.material.y * mr.g, 0.03, 1.0);
    float occlusion = occlusion_texture.sample(occlusion_sampler, in.uv).r;
    float3 emitted = u.emissive.rgb * emissive_texture.sample(emissive_sampler, in.uv).rgb;

    // selected: towards the tint color, and not metallic so the color shows
    base_color = mix(base_color, srgb_to_linear(u.tint.rgb), u.tint.a);
    metallic *= 1.0 - u.tint.a;
    emitted *= 1.0 - u.tint.a;

    float3 v = normalize(u.eye.xyz - in.world_pos);
    float3 n = normalize(in.normal);

    // shells aren't always consistently oriented, light both sides
    if(dot(n, v) < 0.0) {
        n = -n;
    }

    // normal map, tangent frame from the screen space derivatives (no tangents needed)
    if(u.material.z > 0.0) {
        float3 dp1 = dfdx(in.world_pos);
        float3 dp2 = dfdy(in.world_pos);
        float2 duv1 = dfdx(in.uv);
        float2 duv2 = dfdy(in.uv);
        float3 dp2_perp = cross(dp2, n);
        float3 dp1_perp = cross(n, dp1);
        float3 t = dp2_perp * duv1.x + dp1_perp * duv2.x;    // along +u
        float3 b = dp2_perp * duv1.y + dp1_perp * duv2.y;    // along +v (down the image)
        float scale = max(dot(t, t), dot(b, b));
        if(scale > 1e-20) {
            scale = rsqrt(scale);
            float3 m = normal_texture.sample(normal_sampler, in.uv).xyz * 2.0 - 1.0;
            // normal maps have +Y up the image
            float3 mapped = t * scale * m.x - b * scale * m.y + n * m.z;
            if(dot(mapped, mapped) > 1e-12) {
                n = normalize(mapped);
            }
        }
    }

    float n_dot_v = max(dot(n, v), 1e-4);
    float3 r = reflect(-v, n);

    float3 f0 = mix(float3(0.04), base_color, metallic);
    float2 brdf = environment_brdf(n_dot_v, roughness);
    float3 specular = environment_specular(r, roughness) * (f0 * brdf.x + brdf.y);
    float3 diffuse = base_color * (1.0 - metallic) * environment_diffuse(n);

    float3 color = (diffuse + specular) * occlusion + emitted;
    color = tone_map(color * u.eye.w);
    return float4(linear_to_srgb(color), alpha);
}
