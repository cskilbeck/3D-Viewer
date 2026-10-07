//////////////////////////////////////////////////////////////////////
// Draws a step_model with SDL_GPU: shaded triangles + edges, plus a grid and axes
// Depth is reversed (near = 1, far = 0) for precision
// Shading is either CAD (vertex colors with a headlight) or realistic (PBR materials
// and textures lit by a built in studio environment)
//
// Transparency is one of
//   none:     transparent parts sorted by distance, each drawn back faces then front faces
//   sorted:   every transparent triangle sorted by distance (on the CPU)
//   peeled:   depth peeling, exact up to a number of layers. The layers are drawn without
//             MSAA into a float target, then blended over the (MSAA) opaque image

#pragma once

#include <cstdint>
#include <stop_token>
#include <vector>

#include <SDL3/SDL.h>

#include "gpu_device.h"
#include "gpu_math.h"

struct step_model;
struct step_part;
struct step_batch;

struct model_renderer
{
    struct viewport_t
    {
        float x, y, w, h;    // in pixels
    };

    enum transparency_mode
    {
        transparency_none = 0,
        transparency_sorted = 1,
        transparency_peeled = 2,
    };

    bool init(gpu::device &dev, SDL_GPUTextureFormat swapchain_format);
    void cleanup();

    // A model's textures on the GPU. Big textures take a while to upload, so the loader
    // thread does it (SDL_GPU allows that) and set_model() takes them over. They're
    // released if that doesn't happen
    struct texture_set
    {
        SDL_GPUDevice *gpu{};
        std::vector<SDL_GPUTexture *> textures;    // by step_model::textures index, null if it failed

        texture_set() = default;
        texture_set(texture_set const &) = delete;
        texture_set &operator=(texture_set const &) = delete;
        texture_set(texture_set &&other) noexcept;
        texture_set &operator=(texture_set &&other) noexcept;
        ~texture_set();
        void release();
    };

    // upload the model's textures (any thread) and free their pixels
    texture_set upload_textures(step_model &model, std::stop_token stop) const;

    // upload the model's geometry and take over its textures (call on the main thread)
    void set_model(step_model const &model, texture_set &&model_textures);
    void clear_model();

    bool has_model() const
    {
        return num_indices != 0 || num_edge_vertices != 0;
    }

    struct draw_params
    {
        viewport_t viewport;
        float background[3];
        gpu::mat4 view;
        gpu::mat4 projection;
        gpu::vec3 eye;    // for sorting transparent things
        bool show_edges;
        float edge_width;    // pixels
        float axis_width;
        bool realistic;    // PBR rather than CAD shading
        float exposure;    // realistic shading brightness
        float selection_tint[4];                // rgb + strength
        std::vector<step_part> const *parts;    // so selected parts can be found
        std::vector<int> const *selected_parts;

        int transparency;    // transparency_mode
        int peel_layers;     // for transparency_peeled

        // grid on the XY plane
        bool show_grid;
        float grid_plane[4];    // xy = middle of the square it's drawn on, z = height of the plane, w = half size of the square
        float grid_color[4];
        float grid_lines[4];    // x = spacing, yz = offset so lines land on multiples of spacing in file coordinates
        float grid_fade[4];     // xy = middle, z = radius where it's faded out

        bool show_axes;
    };

    // clear the whole target to background and draw the model into the viewport
    void render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swapchain_texture, uint32_t width, uint32_t height, draw_params const &params);

    // MSAA: samples is 1 (off), 2, 4 or 8, it uses the most the GPU can do up to that
    void set_samples(int samples);
    bool sample_count_supported(int samples) const;

    // depth peeling needs depth textures which can be sampled and float render targets
    bool peel_supported{ false };
    bool peel_formats_supported{ false };

    gpu::device *dev{};

    SDL_GPUSampleCount sample_count{ SDL_GPU_SAMPLECOUNT_1 };
    SDL_GPUTextureFormat color_format{};
    SDL_GPUTextureFormat depth_format{};

    SDL_GPUGraphicsPipeline *mesh_pipeline{};
    SDL_GPUGraphicsPipeline *transparent_back_pipeline{};     // back faces of transparent parts
    SDL_GPUGraphicsPipeline *transparent_front_pipeline{};    // then their front faces
    SDL_GPUGraphicsPipeline *transparent_both_pipeline{};     // or both at once (sorted triangles)
    SDL_GPUGraphicsPipeline *pbr_pipeline{};                  // realistic versions of the same 4
    SDL_GPUGraphicsPipeline *pbr_transparent_back_pipeline{};
    SDL_GPUGraphicsPipeline *pbr_transparent_front_pipeline{};
    SDL_GPUGraphicsPipeline *pbr_transparent_both_pipeline{};
    SDL_GPUGraphicsPipeline *edge_pipeline{};
    SDL_GPUGraphicsPipeline *edge_overlay_pipeline{};    // over the depth peeled image (no MSAA)
    SDL_GPUGraphicsPipeline *grid_pipeline{};

    // depth peeling
    SDL_GPUGraphicsPipeline *opaque_depth_pipeline{};    // opaque depth without MSAA
    SDL_GPUGraphicsPipeline *peel_depth_pipeline{};      // the next layer's depth
    SDL_GPUGraphicsPipeline *pbr_peel_depth_pipeline{};
    SDL_GPUGraphicsPipeline *peel_color_pipeline{};    // its color, blended under the layers so far
    SDL_GPUGraphicsPipeline *pbr_peel_color_pipeline{};
    SDL_GPUGraphicsPipeline *composite_pipeline{};    // the layers over the opaque image

    SDL_GPUBuffer *grid_buffer{};    // a -1..1 square

    // for materials without some textures
    SDL_GPUTexture *white_texture{};
    SDL_GPUTexture *flat_normal_texture{};
    SDL_GPUSampler *sampler{};
    SDL_GPUSampler *point_sampler{};    // for reading render targets back

    // render targets, recreated when the size changes
    SDL_GPUTexture *msaa_texture{};
    SDL_GPUTexture *depth_texture{};
    uint32_t target_width{};
    uint32_t target_height{};

    // depth peeling targets (no MSAA), made when they're first needed
    SDL_GPUTextureFormat peel_depth_format{ SDL_GPU_TEXTUREFORMAT_D32_FLOAT };
    SDL_GPUTextureFormat layers_format{ SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT };
    SDL_GPUTexture *opaque_depth_texture{};
    SDL_GPUTexture *peel_depth_textures[2]{};
    SDL_GPUTexture *layers_texture{};
    uint32_t peel_width{};
    uint32_t peel_height{};

    // the model
    SDL_GPUBuffer *vertex_buffer{};
    SDL_GPUBuffer *index_buffer{};
    SDL_GPUBuffer *edge_buffer{};
    SDL_GPUBuffer *axes_buffer{};    // X, Y, Z lines through the origin of the file
    uint32_t num_indices{};
    uint32_t num_opaque_indices{};
    uint32_t num_edge_vertices{};

    // realistic shading
    static constexpr int num_material_textures = 5;    // base color, metallic/roughness, normal, occlusion, emissive

    struct gpu_material
    {
        float emissive[4];    // w = alpha mode, see pbr.frag
        float params[4];      // metallic, roughness, has normal map
        SDL_GPUTexture *textures[num_material_textures];
    };

    std::vector<SDL_GPUTexture *> textures;
    std::vector<gpu_material> materials;
    std::vector<step_batch> batches;
    uint32_t num_opaque_batches{};

    // sorted transparency: the transparent triangles, sorted into an index buffer when the view changes
    struct transparent_triangle
    {
        gpu::vec3 middle;
        int part;
        int material;
    };

    struct sorted_run
    {
        uint32_t first_index;
        uint32_t num_indices;
        int material;
        bool selected;
    };

    std::vector<transparent_triangle> transparent_triangles;
    std::vector<uint32_t> transparent_indices;    // 3 per transparent triangle
    std::vector<uint32_t> sorted_indices;
    std::vector<sorted_run> sorted_runs;
    SDL_GPUBuffer *sorted_index_buffer{};
    SDL_GPUTransferBuffer *sorted_transfer_buffer{};
    gpu::vec3 sorted_eye{};
    uint64_t sorted_signature{};
    bool sorted_valid{ false };

private:
    bool create_pipelines();
    void release_pipelines();
    SDL_GPUSampleCount supported_sample_count(int samples) const;

    void create_targets(uint32_t width, uint32_t height);
    void release_targets();
    void create_peel_targets(uint32_t width, uint32_t height);
    void release_peel_targets();

    void bind_material(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, draw_params const &params, int material_index, float const *tint) const;
    void update_sorted(SDL_GPUCommandBuffer *cmd, draw_params const &params, bool realistic);
    void draw_sorted(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, draw_params const &params, bool realistic) const;
    void draw_lines(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, draw_params const &params, SDL_GPUGraphicsPipeline *pipeline) const;
    void draw_transparent_parts(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, draw_params const &params, bool realistic) const;
    void render_peeled(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swapchain_texture, draw_params const &params, bool realistic);
};
