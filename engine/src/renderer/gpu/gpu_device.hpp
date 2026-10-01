#pragma once
// Ported from v0 engine/renderer_gpu. SDL_GPU device + swapchain + upload ring,
// and a per-frame command-buffer wrapper. kin forces the SPIR-V/Vulkan driver.
#include <kin/core/types.hpp>
#include <kin/platform/window.hpp>

#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <utility>
#include <vector>

namespace kin::gpu {

struct ClearColor {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;
};

struct GpuDeviceOptions {
    bool debug = false;
    SDL_GPUPresentMode present_mode = SDL_GPU_PRESENTMODE_VSYNC;
};

std::string gpu_support_report();

class GpuFrame;

class GpuDevice {
public:
    explicit GpuDevice(Window& window, bool debug = false);
    GpuDevice(Window& window, GpuDeviceOptions options);
    ~GpuDevice();

    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&& other) noexcept;
    GpuDevice& operator=(GpuDevice&& other) noexcept = delete;

    GpuTexture create_render_texture(u32 width, u32 height,
                                     SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID);
    GpuTexture create_texture_from_rgba(const u8* pixels, u32 width, u32 height);
    // A sampled texture of `format` (`texel_bytes` per texel) filled from `pixels`,
    // or with zeros when null. RGBA8 textures can also be render targets.
    GpuTexture create_texture(const void* pixels, u32 width, u32 height, SDL_GPUTextureFormat format,
                              u32 texel_bytes);
    // Uploads `pixels` (tightly packed, `texel_bytes` per texel) over the w x h
    // region at (x, y), ordered after the frames submitted before it.
    void update_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const u8* pixels,
                        u32 texel_bytes = 4);
    GpuBuffer create_buffer(SDL_GPUBufferUsageFlags usage, const void* data, u32 size);
    void upload_buffer(GpuBuffer& buffer, const void* data, u32 size);
    void upload_buffer(GpuFrame& frame, GpuBuffer& buffer, const void* data, u32 size);

    // Download `src` to tightly-packed RGBA8 (waits for the GPU). Returns false on
    // failure. Used by the backend's read_rgba.
    bool read_texture_rgba(const GpuTexture& src, std::vector<u8>& out_rgba);

    GpuFrame begin_frame(bool acquire_swapchain = false);
    void clear_render_target(GpuFrame& frame, const GpuTexture& target, ClearColor color);
    void present_texture(GpuFrame& frame, const GpuTexture& texture,
                         SDL_GPUFilter filter = SDL_GPU_FILTER_NEAREST);
    void wait_idle();
    // When the first command buffer since the last call was submitted
    // (SDL_GetTicksNS), or 0 if none was; GPU frame timing starts frames there.
    u64 take_first_submit_ns() { return std::exchange(_first_submit_ns, 0); }

    SDL_GPUDevice* handle() const { return _device; }
    SDL_GPUTextureFormat swapchain_format() const { return _swapchain_format; }
    SDL_GPUPresentMode present_mode() const { return _present_mode; }
    const char* driver_name() const;

private:
    friend class GpuFrame;

    struct UploadSlice {
        SDL_GPUTransferBuffer* transfer = nullptr;
        u32 offset = 0;
    };

    UploadSlice stage_upload_data(const void* data, u32 size, u32 alignment);
    void release_upload_ring();

    SDL_GPUDevice* _device = nullptr;
    SharedDevice _shared; // handed to textures; nulled on destruction
    SDL_Window* _window = nullptr;
    SDL_GPUTextureFormat _swapchain_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUPresentMode _present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    SDL_GPUTransferBuffer* _upload_ring = nullptr;
    u32 _upload_ring_size = 0;
    u32 _upload_ring_offset = 0;
    u64 _first_submit_ns = 0;
};

class GpuFrame {
public:
    GpuFrame() = default;
    explicit GpuFrame(GpuDevice& device, bool acquire_swapchain = false);
    ~GpuFrame();

    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;
    GpuFrame(GpuFrame&& other) noexcept;
    GpuFrame& operator=(GpuFrame&& other) noexcept = delete;

    bool acquire_swapchain();
    void submit();
    // Submits and returns a fence that signals when the GPU has finished this
    // frame; release it with SDL_ReleaseGPUFence.
    SDL_GPUFence* submit_with_fence();
    void upload_buffer(GpuBuffer& buffer, const void* data, u32 size);

    SDL_GPUCommandBuffer* command_buffer() const { return _command_buffer; }
    SDL_GPUTexture* swapchain_texture() const { return _swapchain_texture; }
    u32 swapchain_width() const { return _swapchain_width; }
    u32 swapchain_height() const { return _swapchain_height; }

private:
    GpuDevice* _device = nullptr;
    SDL_GPUCommandBuffer* _command_buffer = nullptr;
    SDL_GPUTexture* _swapchain_texture = nullptr;
    u32 _swapchain_width = 0;
    u32 _swapchain_height = 0;
    bool _submitted = false;
};

} // namespace kin::gpu
