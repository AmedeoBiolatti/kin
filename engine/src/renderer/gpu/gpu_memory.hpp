#pragma once
#include <SDL3/SDL.h>
#include <kin/renderer/gpu_memory_stats.hpp>

namespace kin::gpu {
void register_memory_device(SDL_GPUDevice* device);
void unregister_memory_device(SDL_GPUDevice* device);
GpuMemoryStats gpu_memory_stats(SDL_GPUDevice* device);
u64 texture_payload_bytes(const SDL_GPUTextureCreateInfo& info);
void pooled_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture, bool pooled);
SDL_GPUTexture* tracked_create_texture(SDL_GPUDevice*, const SDL_GPUTextureCreateInfo*);
SDL_GPUBuffer* tracked_create_buffer(SDL_GPUDevice*, const SDL_GPUBufferCreateInfo*);
SDL_GPUTransferBuffer* tracked_create_transfer(SDL_GPUDevice*, const SDL_GPUTransferBufferCreateInfo*);
void tracked_release_texture(SDL_GPUDevice*, SDL_GPUTexture*);
void tracked_release_buffer(SDL_GPUDevice*, SDL_GPUBuffer*);
void tracked_release_transfer(SDL_GPUDevice*, SDL_GPUTransferBuffer*);
} // namespace kin::gpu
