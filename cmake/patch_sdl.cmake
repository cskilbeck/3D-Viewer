######################################################################
# Patch SDL's D3D12 GPU backend (run as a FetchContent PATCH_COMMAND in the SDL source dir)
#
# The GPU descriptor heaps (2048 samplers, the D3D12 maximum) are only checked
# for overflow before each write of a whole table, so a draw which binds several
# samplers can write past the end of the heap. Switching heaps half way through
# binding a draw's resources would leave its other tables pointing at the old
# heap anyway, so instead: switch before binding anything if either heap is
# nearly full, and rebind everything into the new ones.

set(_file "src/gpu/d3d12/SDL_gpu_d3d12.c")
file(READ "${_file}" _source)

if(_source MATCHES "patched by 3D-Viewer")
    return()
endif()

set(_from [=[    /* Acquire GPU descriptor heaps if we haven't yet */
    if (commandBuffer->gpuDescriptorHeaps[0] == NULL) {
        D3D12_INTERNAL_SetGPUDescriptorHeaps(commandBuffer);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandles[MAX_TEXTURE_SAMPLERS_PER_STAGE];
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[MAX_VERTEX_BUFFERS];]=])

set(_to [=[    /* Acquire GPU descriptor heaps if we haven't yet */
    if (commandBuffer->gpuDescriptorHeaps[0] == NULL) {
        D3D12_INTERNAL_SetGPUDescriptorHeaps(commandBuffer);
    } else if (commandBuffer->gpuDescriptorHeaps[0]->currentDescriptorIndex + 256 > commandBuffer->gpuDescriptorHeaps[0]->maxDescriptors ||
               commandBuffer->gpuDescriptorHeaps[1]->currentDescriptorIndex + 256 > commandBuffer->gpuDescriptorHeaps[1]->maxDescriptors) {
        /* patched by 3D-Viewer: nearly full, start new heaps and rebind everything into them */
        D3D12_INTERNAL_SetGPUDescriptorHeaps(commandBuffer);
        commandBuffer->needVertexSamplerBind = true;
        commandBuffer->needVertexStorageTextureBind = true;
        commandBuffer->needVertexStorageBufferBind = true;
        commandBuffer->needFragmentSamplerBind = true;
        commandBuffer->needFragmentStorageTextureBind = true;
        commandBuffer->needFragmentStorageBufferBind = true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandles[MAX_TEXTURE_SAMPLERS_PER_STAGE];
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[MAX_VERTEX_BUFFERS];]=])

string(FIND "${_source}" "${_from}" _position)
if(_position EQUAL -1)
    message(FATAL_ERROR "patch_sdl.cmake: can't find the code to patch in ${_file}")
endif()

string(REPLACE "${_from}" "${_to}" _source "${_source}")
file(WRITE "${_file}" "${_source}")
message(STATUS "Patched SDL D3D12 descriptor heap overflow")
