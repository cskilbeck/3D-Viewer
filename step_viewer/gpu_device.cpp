//////////////////////////////////////////////////////////////////////
// SDL_GPU device wrapper

#include <cstring>
#include <string>

#include <cmrc/cmrc.hpp>

#include "log.h"
#include "gpu_device.h"

LOG_CONTEXT("gpu_device", info);

CMRC_DECLARE(my_shaders);

namespace
{
#if defined(_WIN32)
    SDL_GPUShaderFormat constexpr shader_format = SDL_GPU_SHADERFORMAT_DXIL;
    char const *const shader_extension = ".dxil";
    char const *const shader_entry_point = "main";
#elif defined(__APPLE__)
    SDL_GPUShaderFormat constexpr shader_format = SDL_GPU_SHADERFORMAT_MSL;
    char const *const shader_extension = ".metal";
    char const *const shader_entry_point = "main0";    // Metal doesn't allow "main"
#else
    SDL_GPUShaderFormat constexpr shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
    char const *const shader_extension = ".spv";
    char const *const shader_entry_point = "main";
#endif
}    // namespace

namespace gpu
{
    //////////////////////////////////////////////////////////////////////

    bool device::init(SDL_Window *win)
    {
        window = win;

        // Request the native shader format for this platform (ImGui's SDL_GPU
        // backend ships its own shaders in each of these formats)
        SDL_GPUShaderFormat formats = shader_format;

        LOG_INFO("Creating SDL_GPU device");
        // Prefer D3D12 on Windows, Metal on macOS, falls back to Vulkan elsewhere
        char const *preferred_driver = nullptr;
#if defined(_WIN32)
        preferred_driver = "direct3d12";
#elif defined(__APPLE__)
        preferred_driver = "metal";
#endif
        gpu = SDL_CreateGPUDevice(formats, true, preferred_driver);
        if(!gpu) {
            LOG_ERROR("SDL_CreateGPUDevice failed: {}", SDL_GetError());
            return false;
        }

        LOG_INFO("GPU driver: {}", SDL_GetGPUDeviceDriver(gpu));
        shader_formats = SDL_GetGPUShaderFormats(gpu);
        LOG_INFO("Shader formats: SPIRV={} DXIL={} DXBC={} MSL={}",
                 (shader_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0,
                 (shader_formats & SDL_GPU_SHADERFORMAT_DXIL) != 0,
                 (shader_formats & SDL_GPU_SHADERFORMAT_DXBC) != 0,
                 (shader_formats & SDL_GPU_SHADERFORMAT_MSL) != 0);

        if(!SDL_ClaimWindowForGPUDevice(gpu, window)) {
            LOG_ERROR("SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
            return false;
        }

        return true;
    }

    //////////////////////////////////////////////////////////////////////

    SDL_GPUShader *device::load_shader(char const *shader_name, SDL_GPUShaderStage stage, int num_uniform_buffers)
    {
        std::string resource_name = std::string(shader_name) + shader_extension;
        auto fs = cmrc::my_shaders::get_filesystem();
        if(!fs.is_file(resource_name)) {
            LOG_ERROR("Shader not found: {}", resource_name);
            return nullptr;
        }
        auto file = fs.open(resource_name);

        SDL_GPUShaderCreateInfo ci{};
        ci.code = reinterpret_cast<Uint8 const *>(file.begin());
        ci.code_size = file.size();
        ci.entrypoint = shader_entry_point;
        ci.format = shader_format;
        ci.stage = stage;
        ci.num_uniform_buffers = num_uniform_buffers;

        SDL_GPUShader *shader = SDL_CreateGPUShader(gpu, &ci);
        if(!shader) {
            LOG_ERROR("SDL_CreateGPUShader failed for '{}': {}", resource_name, SDL_GetError());
        }
        return shader;
    }

    //////////////////////////////////////////////////////////////////////

    SDL_GPUBuffer *device::create_buffer(SDL_GPUBufferUsageFlags usage, void const *data, uint32_t size, char const *name)
    {
        SDL_GPUBufferCreateInfo ci{};
        ci.usage = usage;
        ci.size = size;
        ci.props = SDL_CreateProperties();
        SDL_SetStringProperty(ci.props, SDL_PROP_GPU_BUFFER_CREATE_NAME_STRING, name);
        SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(gpu, &ci);
        SDL_DestroyProperties(ci.props);
        if(buffer == nullptr) {
            LOG_ERROR("SDL_CreateGPUBuffer failed for '{}' ({} bytes): {}", name, size, SDL_GetError());
            return nullptr;
        }

        SDL_GPUTransferBufferCreateInfo tci{};
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tci.size = size;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(gpu, &tci);
        if(transfer == nullptr) {
            LOG_ERROR("SDL_CreateGPUTransferBuffer failed ({} bytes): {}", size, SDL_GetError());
            SDL_ReleaseGPUBuffer(gpu, buffer);
            return nullptr;
        }

        void *mapped = SDL_MapGPUTransferBuffer(gpu, transfer, false);
        memcpy(mapped, data, size);
        SDL_UnmapGPUTransferBuffer(gpu, transfer);

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{ transfer, 0 };
        SDL_GPUBufferRegion dst{ buffer, 0, size };
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);

        // safe to release now, SDL keeps it alive until the copy is done
        SDL_ReleaseGPUTransferBuffer(gpu, transfer);
        return buffer;
    }

    //////////////////////////////////////////////////////////////////////

    void device::shutdown()
    {
        if(gpu && window) {
            SDL_ReleaseWindowFromGPUDevice(gpu, window);
        }
        if(gpu) {
            SDL_DestroyGPUDevice(gpu);
            gpu = nullptr;
        }
    }

}    // namespace gpu
