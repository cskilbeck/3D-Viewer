//////////////////////////////////////////////////////////////////////
// SDL_GPU device wrapper

#pragma once

#include <cstdint>

#include <SDL3/SDL.h>

//////////////////////////////////////////////////////////////////////

namespace gpu
{
    //////////////////////////////////////////////////////////////////////
    // GPU device wrapper

    struct device
    {
        SDL_GPUDevice *gpu{};
        SDL_Window *window{};
        SDL_GPUShaderFormat shader_formats{};

        bool init(SDL_Window *win);
        void shutdown();

        // Load a shader (e.g. "mesh.vert") from the embedded resources in whatever format this platform uses
        SDL_GPUShader *load_shader(char const *shader_name, SDL_GPUShaderStage stage, int num_uniform_buffers, int num_samplers = 0);

        // Create a GPU buffer and upload data into it
        SDL_GPUBuffer *create_buffer(SDL_GPUBufferUsageFlags usage, void const *data, uint32_t size, char const *name);

        // Create a 2D RGBA8 texture from pixels (top row first) with a full set of mipmaps if mipmaps is set
        SDL_GPUTexture *create_texture(void const *pixels, uint32_t width, uint32_t height, bool srgb, bool mipmaps, char const *name);
    };

}    // namespace gpu
