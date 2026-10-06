//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>

#include "log.h"
#include "step_model.h"
#include "model_renderer.h"

LOG_CONTEXT("model_renderer", info);

namespace
{
    struct vertex_uniforms
    {
        gpu::mat4 view;
        gpu::mat4 projection;
    };

    struct color_uniforms
    {
        float color[4];
    };

    struct pbr_fragment_uniforms
    {
        float tint[4];
        float eye[4];    // w = exposure
        float emissive[4];
        float params[4];
    };

    float const edge_color[4] = { 0.08f, 0.08f, 0.08f, 1.0f };

    float const axis_colors[3][4] = { { 0.9f, 0.2f, 0.2f, 1.0f }, { 0.2f, 0.8f, 0.2f, 1.0f }, { 0.25f, 0.45f, 1.0f, 1.0f } };

    struct grid_vertex_uniforms
    {
        gpu::mat4 view;
        gpu::mat4 projection;
        float plane[4];
    };

    struct grid_fragment_uniforms
    {
        float color[4];
        float lines[4];
        float fade[4];
    };

    struct grid_vertex
    {
        float corner[2];
    };

    //////////////////////////////////////////////////////////////////////

    SDL_GPUSampleCount best_sample_count(SDL_GPUDevice *gpu, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format)
    {
        for(SDL_GPUSampleCount count : { SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_2 }) {
            if(SDL_GPUTextureSupportsSampleCount(gpu, color_format, count) && SDL_GPUTextureSupportsSampleCount(gpu, depth_format, count)) {
                return count;
            }
        }
        return SDL_GPU_SAMPLECOUNT_1;
    }

    //////////////////////////////////////////////////////////////////////
    // draw(first, count) for everything in 0..total except the hidden parts' ranges,
    // so it's one draw when nothing's hidden. range(part) gets a part's {first, count},
    // parts are in order so the ranges are too. Anything which isn't in a part is drawn

    template <typename range_fn, typename draw_fn>
    void draw_visible(uint32_t total, std::vector<step_part> const *parts, range_fn range, draw_fn draw)
    {
        uint32_t start = 0;
        if(parts != nullptr) {
            for(step_part const &part : *parts) {
                if(!part.visible) {
                    auto [first, count] = range(part);
                    if(first > start) {
                        draw(start, first - start);
                    }
                    start = std::max(start, first + count);
                }
            }
        }
        if(total > start) {
            draw(start, total - start);
        }
    }

}    // namespace

//////////////////////////////////////////////////////////////////////

bool model_renderer::init(gpu::device &device, SDL_GPUTextureFormat swapchain_format)
{
    dev = &device;
    SDL_GPUDevice *gpu = dev->gpu;

    color_format = swapchain_format;

    depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    if(!SDL_GPUTextureSupportsFormat(gpu, depth_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    }

    sample_count = best_sample_count(gpu, color_format, depth_format);

    LOG_INFO("Render targets: {} samples, depth format {}", 1 << (int)sample_count, (int)depth_format);

    SDL_GPUShader *mesh_vert = dev->load_shader("mesh.vert", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader *mesh_frag = dev->load_shader("mesh.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUShader *edge_vert = dev->load_shader("edge.vert", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader *edge_frag = dev->load_shader("edge.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUShader *grid_vert = dev->load_shader("grid.vert", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader *grid_frag = dev->load_shader("grid.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUShader *pbr_vert = dev->load_shader("pbr.vert", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader *pbr_frag = dev->load_shader("pbr.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, num_material_textures);

    bool ok = mesh_vert && mesh_frag && edge_vert && edge_frag && grid_vert && grid_frag && pbr_vert && pbr_frag;

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = color_format;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.color_target_descriptions = &color_target;
    target_info.num_color_targets = 1;
    target_info.depth_stencil_format = depth_format;
    target_info.has_depth_stencil_target = true;

    SDL_GPUMultisampleState multisample{};
    multisample.sample_count = sample_count;

    // shaded triangles - pushed back a bit so the edges drawn on top win the depth test
    // (depth is reversed so further away is smaller, hence the negative bias)
    // transparent ones are blended, don't write depth and are drawn back faces then front faces

    auto create_mesh_pipeline = [&](char const *name, SDL_GPUCullMode cull_mode, bool transparent, bool pbr) -> SDL_GPUGraphicsPipeline * {
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(mesh_vertex);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        // CAD shading doesn't need the texture coordinates
        SDL_GPUVertexAttribute attributes[4]{};
        uint32_t num_attributes = 0;
        auto add_attribute = [&](SDL_GPUVertexElementFormat format, uint32_t offset) {
            attributes[num_attributes] = { num_attributes, 0, format, offset };
            num_attributes += 1;
        };
        add_attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(mesh_vertex, position));
        add_attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(mesh_vertex, normal));
        if(pbr) {
            add_attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(mesh_vertex, uv));
        }
        add_attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(mesh_vertex, color));

        SDL_GPUGraphicsPipelineCreateInfo ci{};
        ci.vertex_shader = pbr ? pbr_vert : mesh_vert;
        ci.fragment_shader = pbr ? pbr_frag : mesh_frag;
        ci.vertex_input_state.vertex_buffer_descriptions = &buffer;
        ci.vertex_input_state.num_vertex_buffers = 1;
        ci.vertex_input_state.vertex_attributes = attributes;
        ci.vertex_input_state.num_vertex_attributes = num_attributes;
        ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        ci.rasterizer_state.cull_mode = cull_mode;
        ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        ci.rasterizer_state.enable_depth_bias = true;
        ci.rasterizer_state.depth_bias_constant_factor = -2.0f;
        ci.rasterizer_state.depth_bias_slope_factor = -1.5f;
        ci.depth_stencil_state.enable_depth_test = true;
        ci.depth_stencil_state.enable_depth_write = !transparent;
        ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;    // selected parts get drawn twice
        ci.multisample_state = multisample;

        SDL_GPUColorTargetDescription blended_target = color_target;
        if(transparent) {
            SDL_GPUColorTargetBlendState &blend = blended_target.blend_state;
            blend.enable_blend = true;
            blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
            blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        ci.target_info = target_info;
        ci.target_info.color_target_descriptions = &blended_target;

        SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
        if(pipeline == nullptr) {
            LOG_ERROR("Can't create {} pipeline: {}", name, SDL_GetError());
            ok = false;
        }
        return pipeline;
    };

    if(ok) {
        mesh_pipeline = create_mesh_pipeline("mesh", SDL_GPU_CULLMODE_NONE, false, false);
        transparent_back_pipeline = create_mesh_pipeline("transparent back", SDL_GPU_CULLMODE_FRONT, true, false);
        transparent_front_pipeline = create_mesh_pipeline("transparent front", SDL_GPU_CULLMODE_BACK, true, false);
        pbr_pipeline = create_mesh_pipeline("pbr", SDL_GPU_CULLMODE_NONE, false, true);
        pbr_transparent_back_pipeline = create_mesh_pipeline("pbr transparent back", SDL_GPU_CULLMODE_FRONT, true, true);
        pbr_transparent_front_pipeline = create_mesh_pipeline("pbr transparent front", SDL_GPU_CULLMODE_BACK, true, true);
    }

    // textures for materials which don't have them, and the sampler they all use

    if(ok) {
        uint8_t const white[4] = { 255, 255, 255, 255 };
        uint8_t const flat_normal[4] = { 128, 128, 255, 255 };
        white_texture = dev->create_texture(white, 1, 1, false, false, "white");
        flat_normal_texture = dev->create_texture(flat_normal, 1, 1, false, false, "flat normal");

        SDL_GPUSamplerCreateInfo sci{};
        sci.min_filter = SDL_GPU_FILTER_LINEAR;
        sci.mag_filter = SDL_GPU_FILTER_LINEAR;
        sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sci.enable_anisotropy = true;
        sci.max_anisotropy = 8;
        sci.min_lod = 0;
        sci.max_lod = 1000;
        sampler = SDL_CreateGPUSampler(gpu, &sci);

        if(white_texture == nullptr || flat_normal_texture == nullptr || sampler == nullptr) {
            LOG_ERROR("Can't create default textures/sampler: {}", SDL_GetError());
            ok = false;
        }
    }

    // edges

    if(ok) {
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(edge_vertex);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attribute{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(edge_vertex, position) };

        SDL_GPUGraphicsPipelineCreateInfo ci{};
        ci.vertex_shader = edge_vert;
        ci.fragment_shader = edge_frag;
        ci.vertex_input_state.vertex_buffer_descriptions = &buffer;
        ci.vertex_input_state.num_vertex_buffers = 1;
        ci.vertex_input_state.vertex_attributes = &attribute;
        ci.vertex_input_state.num_vertex_attributes = 1;
        ci.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
        ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        ci.depth_stencil_state.enable_depth_test = true;
        ci.depth_stencil_state.enable_depth_write = true;
        ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        ci.multisample_state = multisample;
        ci.target_info = target_info;

        edge_pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
        if(edge_pipeline == nullptr) {
            LOG_ERROR("Can't create edge pipeline: {}", SDL_GetError());
            ok = false;
        }
    }

    // grid - blended, depth tested but doesn't write depth

    if(ok) {
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(grid_vertex);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attribute{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(grid_vertex, corner) };

        SDL_GPUColorTargetDescription blended_target = color_target;
        SDL_GPUColorTargetBlendState &blend = blended_target.blend_state;
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo ci{};
        ci.vertex_shader = grid_vert;
        ci.fragment_shader = grid_frag;
        ci.vertex_input_state.vertex_buffer_descriptions = &buffer;
        ci.vertex_input_state.num_vertex_buffers = 1;
        ci.vertex_input_state.vertex_attributes = &attribute;
        ci.vertex_input_state.num_vertex_attributes = 1;
        ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        ci.depth_stencil_state.enable_depth_test = true;
        ci.depth_stencil_state.enable_depth_write = false;
        ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        ci.multisample_state = multisample;
        ci.target_info = target_info;
        ci.target_info.color_target_descriptions = &blended_target;

        grid_pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
        if(grid_pipeline == nullptr) {
            LOG_ERROR("Can't create grid pipeline: {}", SDL_GetError());
            ok = false;
        }

        grid_vertex const square[4] = { { { -1, -1 } }, { { 1, -1 } }, { { -1, 1 } }, { { 1, 1 } } };
        grid_buffer = dev->create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, square, sizeof(square), "grid");
    }

    // pipelines keep what they need from the shaders
    for(SDL_GPUShader *shader : { mesh_vert, mesh_frag, edge_vert, edge_frag, grid_vert, grid_frag, pbr_vert, pbr_frag }) {
        if(shader != nullptr) {
            SDL_ReleaseGPUShader(gpu, shader);
        }
    }

    return ok;
}

//////////////////////////////////////////////////////////////////////

void model_renderer::cleanup()
{
    if(dev == nullptr) {
        return;
    }
    clear_model();
    release_targets();
    if(grid_buffer != nullptr) {
        SDL_ReleaseGPUBuffer(dev->gpu, grid_buffer);
        grid_buffer = nullptr;
    }
    for(SDL_GPUGraphicsPipeline **pipeline : { &mesh_pipeline,
                                                &transparent_back_pipeline,
                                                &transparent_front_pipeline,
                                                &pbr_pipeline,
                                                &pbr_transparent_back_pipeline,
                                                &pbr_transparent_front_pipeline,
                                                &grid_pipeline }) {
        if(*pipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(dev->gpu, *pipeline);
            *pipeline = nullptr;
        }
    }
    if(edge_pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(dev->gpu, edge_pipeline);
        edge_pipeline = nullptr;
    }
    for(SDL_GPUTexture **texture : { &white_texture, &flat_normal_texture }) {
        if(*texture != nullptr) {
            SDL_ReleaseGPUTexture(dev->gpu, *texture);
            *texture = nullptr;
        }
    }
    if(sampler != nullptr) {
        SDL_ReleaseGPUSampler(dev->gpu, sampler);
        sampler = nullptr;
    }
    dev = nullptr;
}

//////////////////////////////////////////////////////////////////////

model_renderer::texture_set::texture_set(texture_set &&other) noexcept : gpu(other.gpu), textures(std::move(other.textures))
{
    other.textures.clear();
}

model_renderer::texture_set &model_renderer::texture_set::operator=(texture_set &&other) noexcept
{
    if(this != &other) {
        release();
        gpu = other.gpu;
        textures = std::move(other.textures);
        other.textures.clear();
    }
    return *this;
}

model_renderer::texture_set::~texture_set()
{
    release();
}

void model_renderer::texture_set::release()
{
    for(SDL_GPUTexture *texture : textures) {
        if(texture != nullptr) {
            SDL_ReleaseGPUTexture(gpu, texture);
        }
    }
    textures.clear();
}

//////////////////////////////////////////////////////////////////////

model_renderer::texture_set model_renderer::upload_textures(step_model &model, std::stop_token stop) const
{
    texture_set result;
    result.gpu = dev->gpu;
    for(size_t i = 0; i < model.textures.size() && !stop.stop_requested(); ++i) {
        step_texture &texture = model.textures[i];
        SDL_GPUTexture *gpu_texture = nullptr;
        if(!texture.pixels.empty()) {
            gpu_texture = dev->create_texture(texture.pixels.data(), texture.width, texture.height, texture.srgb, true, std::format("texture {}", i).c_str());
        }
        result.textures.push_back(gpu_texture);
        texture.pixels = {};
    }
    return result;
}

//////////////////////////////////////////////////////////////////////

void model_renderer::set_model(step_model const &model, texture_set &&model_textures)
{
    clear_model();

    if(!model.indices.empty()) {
        vertex_buffer = dev->create_buffer(
            SDL_GPU_BUFFERUSAGE_VERTEX, model.vertices.data(), (uint32_t)(model.vertices.size() * sizeof(mesh_vertex)), "model vertices");
        index_buffer =
            dev->create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, model.indices.data(), (uint32_t)(model.indices.size() * sizeof(uint32_t)), "model indices");
        if(vertex_buffer != nullptr && index_buffer != nullptr) {
            num_indices = (uint32_t)model.indices.size();
            num_opaque_indices = model.num_opaque_indices;
        }
    }

    // materials for realistic shading

    batches = model.batches;
    num_opaque_batches = model.num_opaque_batches;

    // already uploaded (or any which weren't, here)
    textures = std::move(model_textures.textures);
    model_textures.textures.clear();
    for(size_t i = textures.size(); i < model.textures.size(); ++i) {
        step_texture const &texture = model.textures[i];
        SDL_GPUTexture *gpu_texture = nullptr;
        if(!texture.pixels.empty()) {
            gpu_texture = dev->create_texture(texture.pixels.data(), texture.width, texture.height, texture.srgb, true, std::format("texture {}", i).c_str());
        }
        textures.push_back(gpu_texture);
    }

    auto texture_or = [&](int index, SDL_GPUTexture *fallback) {
        return (index >= 0 && textures[index] != nullptr) ? textures[index] : fallback;
    };

    for(step_material const &m : model.materials) {
        float alpha_mode = m.opaque ? -1.0f : m.alpha_cutoff;
        materials.push_back({ { m.emissive[0], m.emissive[1], m.emissive[2], alpha_mode },
                              { m.metallic, m.roughness, texture_or(m.normal_texture, nullptr) != nullptr ? 1.0f : 0.0f, 0 },
                              { texture_or(m.base_color_texture, white_texture),
                                texture_or(m.metallic_roughness_texture, white_texture),
                                texture_or(m.normal_texture, flat_normal_texture),
                                texture_or(m.occlusion_texture, white_texture),
                                texture_or(m.emissive_texture, white_texture) } });
    }

    // a long way, perspective has no far plane so they go off to the horizon
    {
        gpu::vec3 origin{ (float)-model.center[0], (float)-model.center[1], (float)-model.center[2] };
        float length = (float)model.radius * 1e4f;
        edge_vertex axes[6];
        for(int axis = 0; axis < 3; ++axis) {
            gpu::vec3 direction{ axis == 0 ? length : 0.0f, axis == 1 ? length : 0.0f, axis == 2 ? length : 0.0f };
            gpu::vec3 a = origin - direction;
            gpu::vec3 b = origin + direction;
            axes[axis * 2] = { { a.x, a.y, a.z } };
            axes[axis * 2 + 1] = { { b.x, b.y, b.z } };
        }
        axes_buffer = dev->create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, axes, sizeof(axes), "axes");
    }

    if(!model.edges.empty()) {
        edge_buffer =
            dev->create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, model.edges.data(), (uint32_t)(model.edges.size() * sizeof(edge_vertex)), "model edges");
        if(edge_buffer != nullptr) {
            num_edge_vertices = (uint32_t)model.edges.size();
        }
    }
}

//////////////////////////////////////////////////////////////////////

void model_renderer::clear_model()
{
    for(SDL_GPUBuffer **buffer : { &vertex_buffer, &index_buffer, &edge_buffer, &axes_buffer }) {
        if(*buffer != nullptr) {
            SDL_ReleaseGPUBuffer(dev->gpu, *buffer);
            *buffer = nullptr;
        }
    }
    num_indices = 0;
    num_opaque_indices = 0;
    num_edge_vertices = 0;

    for(SDL_GPUTexture *texture : textures) {
        if(texture != nullptr) {
            SDL_ReleaseGPUTexture(dev->gpu, texture);
        }
    }
    textures.clear();
    materials.clear();
    batches.clear();
    num_opaque_batches = 0;
}

//////////////////////////////////////////////////////////////////////

void model_renderer::create_targets(uint32_t width, uint32_t height)
{
    release_targets();

    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.width = width;
    ci.height = height;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = sample_count;

    if(sample_count != SDL_GPU_SAMPLECOUNT_1) {
        ci.format = color_format;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        msaa_texture = SDL_CreateGPUTexture(dev->gpu, &ci);
        if(msaa_texture == nullptr) {
            LOG_ERROR("Can't create MSAA target: {}", SDL_GetError());
        }
    }

    ci.format = depth_format;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_texture = SDL_CreateGPUTexture(dev->gpu, &ci);
    if(depth_texture == nullptr) {
        LOG_ERROR("Can't create depth target: {}", SDL_GetError());
    }

    target_width = width;
    target_height = height;
}

//////////////////////////////////////////////////////////////////////

void model_renderer::release_targets()
{
    for(SDL_GPUTexture **texture : { &msaa_texture, &depth_texture }) {
        if(*texture != nullptr) {
            SDL_ReleaseGPUTexture(dev->gpu, *texture);
            *texture = nullptr;
        }
    }
    target_width = 0;
    target_height = 0;
}

//////////////////////////////////////////////////////////////////////

void model_renderer::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swapchain_texture, uint32_t width, uint32_t height, draw_params const &params)
{
    viewport_t const &viewport = params.viewport;
    float const *background = params.background;

    if(width != target_width || height != target_height) {
        create_targets(width, height);
    }

    bool msaa = msaa_texture != nullptr;

    SDL_GPUColorTargetInfo color{};
    color.clear_color = { background[0], background[1], background[2], 1.0f };
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    if(msaa) {
        color.texture = msaa_texture;
        color.store_op = SDL_GPU_STOREOP_RESOLVE;
        color.resolve_texture = swapchain_texture;
        color.cycle = true;
    } else {
        color.texture = swapchain_texture;
        color.store_op = SDL_GPU_STOREOP_STORE;
    }

    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_texture;
    depth.clear_depth = 0.0f;    // reversed, 0 is far
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, depth_texture != nullptr ? &depth : nullptr);

    if(has_model() && depth_texture != nullptr && viewport.w >= 1 && viewport.h >= 1) {

        SDL_GPUViewport vp{ viewport.x, viewport.y, viewport.w, viewport.h, 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &vp);

        SDL_Rect scissor{ (int)viewport.x, (int)viewport.y, (int)viewport.w, (int)viewport.h };
        SDL_SetGPUScissor(pass, &scissor);

        vertex_uniforms uniforms{ params.view, params.projection };

        bool realistic = params.realistic && !materials.empty();

        auto is_visible = [&](int part_index) { return params.parts == nullptr || (*params.parts)[part_index].visible; };

        // realistic shading: a material's textures and settings, tinted if it's selected
        auto bind_material = [&](int material_index, float const *tint) {
            gpu_material const &material = materials[material_index];
            pbr_fragment_uniforms u{};
            std::copy(tint, tint + 4, u.tint);
            u.eye[0] = params.eye.x;
            u.eye[1] = params.eye.y;
            u.eye[2] = params.eye.z;
            u.eye[3] = params.exposure;
            std::copy(material.emissive, material.emissive + 4, u.emissive);
            std::copy(material.params, material.params + 4, u.params);
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            SDL_GPUTextureSamplerBinding bindings[num_material_textures];
            for(int i = 0; i < num_material_textures; ++i) {
                bindings[i] = { material.textures[i], sampler };
            }
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, num_material_textures);
        };

        float const no_tint[4]{};

        if(num_indices != 0 && realistic) {
            SDL_BindGPUGraphicsPipeline(pass, pbr_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
            SDL_GPUBufferBinding vertices{ vertex_buffer, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUBufferBinding indices{ index_buffer, 0 };
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);

            // consecutive batches with the same material are drawn together
            uint32_t run_first = 0;
            uint32_t run_count = 0;
            int run_material = 0;
            auto draw_run = [&]() {
                if(run_count != 0) {
                    bind_material(run_material, no_tint);
                    SDL_DrawGPUIndexedPrimitives(pass, run_count, 1, run_first, 0, 0);
                    run_count = 0;
                }
            };
            for(uint32_t i = 0; i < num_opaque_batches; ++i) {
                step_batch const &batch = batches[i];
                if(!is_visible(batch.part)) {
                    continue;
                }
                if(run_count != 0 && batch.material == run_material && batch.first_index == run_first + run_count) {
                    run_count += batch.num_indices;
                } else {
                    draw_run();
                    run_first = batch.first_index;
                    run_count = batch.num_indices;
                    run_material = batch.material;
                }
            }
            draw_run();

            // selected parts again on top, tinted
            if(params.parts != nullptr && params.selected_parts != nullptr) {
                for(int part_index : *params.selected_parts) {
                    step_part const &part = (*params.parts)[part_index];
                    if(part.visible) {
                        for(uint32_t i = part.first_batch; i < part.first_batch + part.num_batches; ++i) {
                            bind_material(batches[i].material, params.selection_tint);
                            SDL_DrawGPUIndexedPrimitives(pass, batches[i].num_indices, 1, batches[i].first_index, 0, 0);
                        }
                    }
                }
            }
        } else if(num_indices != 0) {
            SDL_BindGPUGraphicsPipeline(pass, mesh_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
            SDL_GPUBufferBinding vertices{ vertex_buffer, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUBufferBinding indices{ index_buffer, 0 };
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);

            color_uniforms untinted{};
            SDL_PushGPUFragmentUniformData(cmd, 0, &untinted, sizeof(untinted));
            draw_visible(
                num_opaque_indices,
                params.parts,
                [](step_part const &part) { return std::pair{ part.first_index, part.num_indices }; },
                [&](uint32_t first, uint32_t count) { SDL_DrawGPUIndexedPrimitives(pass, count, 1, first, 0, 0); });

            // selected parts again on top, tinted
            if(params.parts != nullptr && params.selected_parts != nullptr && !params.selected_parts->empty()) {
                color_uniforms tint;
                std::copy(params.selection_tint, params.selection_tint + 4, tint.color);
                SDL_PushGPUFragmentUniformData(cmd, 0, &tint, sizeof(tint));
                for(int part_index : *params.selected_parts) {
                    step_part const &part = (*params.parts)[part_index];
                    if(part.visible && part.num_indices != 0) {
                        SDL_DrawGPUIndexedPrimitives(pass, part.num_indices, 1, part.first_index, 0, 0);
                    }
                }
            }
        }

        // edges before the transparent parts so they show through them
        if(params.show_edges && num_edge_vertices != 0) {
            color_uniforms edge{ { edge_color[0], edge_color[1], edge_color[2], edge_color[3] } };
            SDL_BindGPUGraphicsPipeline(pass, edge_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
            SDL_PushGPUFragmentUniformData(cmd, 0, &edge, sizeof(edge));
            SDL_GPUBufferBinding vertices{ edge_buffer, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            draw_visible(
                num_edge_vertices,
                params.parts,
                [](step_part const &part) { return std::pair{ part.first_edge_vertex, part.num_edge_vertices }; },
                [&](uint32_t first, uint32_t count) { SDL_DrawGPUPrimitives(pass, count, 1, first, 0); });
        }

        // axes
        if(params.show_axes && axes_buffer != nullptr) {
            SDL_BindGPUGraphicsPipeline(pass, edge_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
            SDL_GPUBufferBinding vertices{ axes_buffer, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            for(int axis = 0; axis < 3; ++axis) {
                color_uniforms color;
                std::copy(axis_colors[axis], axis_colors[axis] + 4, color.color);
                SDL_PushGPUFragmentUniformData(cmd, 0, &color, sizeof(color));
                SDL_DrawGPUPrimitives(pass, 2, 1, axis * 2, 0);
            }
        }

        // grid, transparent parts blend over it
        if(params.show_grid && grid_buffer != nullptr) {
            grid_vertex_uniforms grid_vertex_data{ params.view, params.projection, {} };
            std::copy(params.grid_plane, params.grid_plane + 4, grid_vertex_data.plane);
            grid_fragment_uniforms grid_fragment_data{};
            std::copy(params.grid_color, params.grid_color + 4, grid_fragment_data.color);
            std::copy(params.grid_lines, params.grid_lines + 4, grid_fragment_data.lines);
            std::copy(params.grid_fade, params.grid_fade + 4, grid_fragment_data.fade);
            SDL_BindGPUGraphicsPipeline(pass, grid_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &grid_vertex_data, sizeof(grid_vertex_data));
            SDL_PushGPUFragmentUniformData(cmd, 0, &grid_fragment_data, sizeof(grid_fragment_data));
            SDL_GPUBufferBinding vertices{ grid_buffer, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        }

        // transparent parts, furthest first
        if(num_indices > num_opaque_indices && params.parts != nullptr) {
            std::vector<std::pair<float, int>> order;
            for(int i = 0; i < (int)params.parts->size(); ++i) {
                step_part const &part = (*params.parts)[i];
                if(part.visible && part.num_transparent_indices != 0) {
                    gpu::vec3 middle = (part.bounds_min + part.bounds_max) * 0.5f;
                    order.emplace_back((middle - params.eye).length(), i);
                }
            }
            std::sort(order.begin(), order.end(), [](auto const &a, auto const &b) { return a.first > b.first; });

            auto is_selected = [&](int part_index) {
                return params.selected_parts != nullptr &&
                       std::find(params.selected_parts->begin(), params.selected_parts->end(), part_index) != params.selected_parts->end();
            };

            SDL_GPUBufferBinding vertices{ vertex_buffer, 0 };
            SDL_GPUBufferBinding indices{ index_buffer, 0 };

            color_uniforms untinted{};
            color_uniforms tint;
            std::copy(params.selection_tint, params.selection_tint + 4, tint.color);

            SDL_GPUGraphicsPipeline *back_pipeline = realistic ? pbr_transparent_back_pipeline : transparent_back_pipeline;
            SDL_GPUGraphicsPipeline *front_pipeline = realistic ? pbr_transparent_front_pipeline : transparent_front_pipeline;

            for(auto const &[distance, part_index] : order) {
                step_part const &part = (*params.parts)[part_index];
                bool selected = is_selected(part_index);
                for(SDL_GPUGraphicsPipeline *pipeline : { back_pipeline, front_pipeline }) {
                    SDL_BindGPUGraphicsPipeline(pass, pipeline);
                    SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
                    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
                    SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    if(realistic) {
                        for(uint32_t i = part.first_transparent_batch; i < part.first_transparent_batch + part.num_transparent_batches; ++i) {
                            bind_material(batches[i].material, selected ? params.selection_tint : no_tint);
                            SDL_DrawGPUIndexedPrimitives(pass, batches[i].num_indices, 1, batches[i].first_index, 0, 0);
                        }
                    } else {
                        color_uniforms const &part_tint = selected ? tint : untinted;
                        SDL_PushGPUFragmentUniformData(cmd, 0, &part_tint, sizeof(part_tint));
                        SDL_DrawGPUIndexedPrimitives(pass, part.num_transparent_indices, 1, part.first_transparent_index, 0, 0);
                    }
                }
            }
        }
    }

    SDL_EndGPURenderPass(pass);
}
