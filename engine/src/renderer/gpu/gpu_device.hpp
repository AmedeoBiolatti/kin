#pragma once
// Ported from v0 engine/renderer_gpu. SDL_GPU device + swapchain + upload ring,
// and a per-frame command-buffer wrapper. kin forces the SPIR-V/Vulkan driver.
#include <kin/core/types.hpp>
#include <kin/platform/window.hpp>

#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kin {
class JobSystem;
}

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
    GpuDevice(GpuDevice&& other) noexcept; // the upload state is moved, not the mutex
    GpuDevice& operator=(GpuDevice&& other) noexcept = delete;

    GpuTexture create_render_texture(u32 width, u32 height,
                                     SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID);
    GpuTexture create_texture_from_rgba(const u8* pixels, u32 width, u32 height);
    // A depth-stencil target of depth_stencil_format() (stencil clips).
    GpuTexture create_depth_stencil(u32 width, u32 height);
    // The depth-stencil format this device draws into, INVALID if none.
    SDL_GPUTextureFormat depth_stencil_format() const;
    // A sampled texture of `format` (`texel_bytes` per texel) filled from `pixels`,
    // or with zeros when null. RGBA8 textures can also be render targets.
    // `mipmapped`: with a full chain of smaller levels, made from the pixels (a
    // kind of its own, never pooled; RGBA8 only).
    GpuTexture create_texture(const void* pixels, u32 width, u32 height, SDL_GPUTextureFormat format,
                              u32 texel_bytes, SDL_GPUTextureUsageFlags extra_usage = 0, bool mipmapped = false);
    // Uploads `pixels` (tightly packed, `texel_bytes` per texel) over the w x h
    // region at (x, y), ordered after the frames submitted before it. `whole`:
    // the region is the whole texture, so its storage may be cycled (a frame
    // still reading the old texels doesn't hold the upload up).
    void update_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const u8* pixels,
                        u32 texel_bytes = 4, bool whole = false);
    // Uploads `bytes` (from `data`, zeros when null, or written by `fill`) to a
    // buffer at `offset`, with the texture uploads. `cycle`: the whole buffer is
    // replaced and may get fresh storage.
    void upload_storage_buffer(SDL_GPUBuffer* buffer, u32 offset, u32 bytes, const void* data, bool cycle,
                               const std::function<void(std::span<u8>)>* fill = nullptr);
    // As update_texture, but `fill` writes the texels straight into the upload
    // memory (no copy). It must not call back into the device.
    void write_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, u32 texel_bytes, bool whole,
                       const std::function<void(std::span<u8>)>& fill);
    // Texture uploads (create_texture, update_texture) are not submitted one by
    // one: each is recorded at once into a shared upload command buffer, which
    // goes to the GPU before the next submission (a frame, a read-back), so the
    // frame sees them. Recording at once keeps the order of uploads and draws:
    // a draw recorded before a cycled upload reads the old texels.
    void flush_uploads();
    // A copy of `source` (RGBA8) with a full mip chain, made on the GPU with the
    // uploads (the copies before the frame that draws it).
    GpuTexture make_mipmapped(const GpuTexture& source);
    // Remakes a mipmapped texture's smaller levels from level 0, with the uploads.
    void generate_mipmaps(SDL_GPUTexture* texture);
    // Big uploads copy their texels on these workers too (null: none).
    void set_job_system(JobSystem* jobs) { _jobs = jobs; }
    // Texture uploads recorded and command buffers submitted for them so far.
    std::pair<u64, u64> upload_counts() const { return {_uploads_staged, _upload_batches}; }

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

    // Copies `pixels` (or zeros, when null) to a transfer buffer and records the
    // upload to `texture`; or, with `fill`, lets it write the texels there.
    void stage_texture_upload(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const void* pixels,
                              u32 texel_bytes, bool cycle,
                              const std::function<void(std::span<u8>)>* fill = nullptr);
    // Where an upload goes: a texture's region, or a range of a buffer.
    struct UploadDestination {
        SDL_GPUTexture* texture = nullptr;
        u32 x = 0, y = 0, w = 0, h = 0;
        SDL_GPUBuffer* buffer = nullptr;
        u32 buffer_offset = 0;
    };
    void stage_upload(const UploadDestination& to, u32 bytes, const void* pixels, bool cycle,
                      const std::function<void(std::span<u8>)>* fill);

    SDL_GPUDevice* _device = nullptr;
    SharedDevice _shared; // handed to textures; nulled on destruction
    SDL_Window* _window = nullptr;
    SDL_GPUTextureFormat _swapchain_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUPresentMode _present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    SDL_GPUTransferBuffer* _upload_ring = nullptr;
    u32 _upload_ring_size = 0;
    u32 _upload_ring_offset = 0;
    u64 _first_submit_ns = 0;
    // Texture uploads: the command buffer and copy pass they are recorded into
    // (open until flush_uploads), and the staging buffer they share.
    std::mutex _uploads_mutex;
    // The thread inside a write_texture fill, which holds the mutex: an upload
    // from it would deadlock, so it is refused instead.
    std::atomic<std::thread::id> _filling{};
    void refuse_upload_inside_fill() const;
    void begin_upload_commands(); // the upload command buffer, acquired if needed
    void end_upload_pass();       // ends its copy pass (reopened by the next upload)
    // Clears `texture` (a render target) to zeros in the upload command buffer.
    void clear_texture(SDL_GPUTexture* texture, bool cycle);
    SDL_GPUCommandBuffer* _upload_commands = nullptr;
    SDL_GPUCopyPass* _upload_pass = nullptr;
    SDL_GPUTransferBuffer* _staging = nullptr;
    u32 _staging_used = 0;
    u32 _staging_size = 0;     // grows to fit the biggest upload, shrinks after
    u64 _big_upload_batch = 0; // the batch of the last upload over half the usual size
    JobSystem* _jobs = nullptr;
    std::shared_ptr<GpuTexturePool> _texture_pool; // released sampled textures, for create_texture
    u64 _uploads_staged = 0;
    u64 _upload_batches = 0;
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
