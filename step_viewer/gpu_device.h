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
        SDL_GPUShader *load_shader(char const *shader_name, SDL_GPUShaderStage stage, int num_uniform_buffers);

        // Create a GPU buffer and upload data into it
        SDL_GPUBuffer *create_buffer(SDL_GPUBufferUsageFlags usage, void const *data, uint32_t size, char const *name);
    };

}    // namespace gpu
