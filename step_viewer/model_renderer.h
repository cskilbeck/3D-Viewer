//////////////////////////////////////////////////////////////////////
// Draws a step_model with SDL_GPU: shaded triangles + edges

#pragma once

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "gpu_device.h"
#include "gpu_math.h"

struct step_model;
struct step_part;

struct model_renderer
{
    struct viewport_t
    {
        float x, y, w, h;    // in pixels
    };

    bool init(gpu::device &dev, SDL_GPUTextureFormat swapchain_format);
    void cleanup();

    // upload the model's geometry (call on the main thread)
    void set_model(step_model const &model);
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
        gpu::vec3 eye;    // for sorting transparent parts
        bool show_edges;
        float selection_tint[4];                // rgb + strength
        std::vector<step_part> const *parts;    // so selected parts can be found
        std::vector<int> const *selected_parts;
    };

    // clear the whole target to background and draw the model into the viewport
    void render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swapchain_texture, uint32_t width, uint32_t height, draw_params const &params);

    gpu::device *dev{};

    SDL_GPUSampleCount sample_count{ SDL_GPU_SAMPLECOUNT_1 };
    SDL_GPUTextureFormat color_format{};
    SDL_GPUTextureFormat depth_format{};

    SDL_GPUGraphicsPipeline *mesh_pipeline{};
    SDL_GPUGraphicsPipeline *transparent_back_pipeline{};     // back faces of transparent parts
    SDL_GPUGraphicsPipeline *transparent_front_pipeline{};    // then their front faces
    SDL_GPUGraphicsPipeline *edge_pipeline{};

    // render targets, recreated when the size changes
    SDL_GPUTexture *msaa_texture{};
    SDL_GPUTexture *depth_texture{};
    uint32_t target_width{};
    uint32_t target_height{};

    // the model
    SDL_GPUBuffer *vertex_buffer{};
    SDL_GPUBuffer *index_buffer{};
    SDL_GPUBuffer *edge_buffer{};
    uint32_t num_indices{};
    uint32_t num_opaque_indices{};
    uint32_t num_edge_vertices{};

private:
    void create_targets(uint32_t width, uint32_t height);
    void release_targets();
};
