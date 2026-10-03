#include "gpu_device.hpp"

#include <kin/core/jobs.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace kin::gpu {

namespace {

// Force SPIR-V so SDL_GPU selects the Vulkan backend.
constexpr SDL_GPUShaderFormat SupportedShaderFormats = SDL_GPU_SHADERFORMAT_SPIRV;
constexpr u32 DefaultUploadRingSize = 1024u * 1024u;
// Texture uploads share a staging buffer of this size. It grows to fit a bigger
// upload (fresh transfer memory each time writes several times slower, its
// pages faulting in), and shrinks back once none has come for a while.
constexpr u32 TextureStagingSize = 16u * 1024u * 1024u;
constexpr u32 StagingShrinkAfterBatches = 600;
// Where each staged upload starts in it (Vulkan wants a multiple of the texel
// size and of 4; this covers every format, and D3D12's 512 would be next).
constexpr u32 TextureStagingAlignment = 256u;

u32 align_up(u32 value, u32 alignment) {
    if (alignment == 0) {
        return value;
    }
    const u32 remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

u32 next_power_of_two(u32 value) {
    if (value <= 1) {
        return 1;
    }
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return value + 1;
}

// Copies `bytes` into mapped transfer memory, or zeros when `src` is null. The
// memory is usually write-combined (uncached, for the GPU to read): there,
// non-temporal stores that bypass the cache write about twice as fast as a
// plain memcpy's.
void copy_to_gpu_memory(u8* dst, const void* src, std::size_t bytes) {
#if defined(__SSE2__) || defined(_M_X64)
    const auto* from = static_cast<const u8*>(src);
    const std::size_t head = std::min(bytes, (16 - reinterpret_cast<std::uintptr_t>(dst) % 16) % 16);
    if (from) {
        std::memcpy(dst, from, head);
    } else {
        std::memset(dst, 0, head);
    }
    std::size_t i = head;
    const __m128i zero = _mm_setzero_si128();
    for (; i + 64 <= bytes; i += 64) {
        const auto load = [&](std::size_t at) {
            return from ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + at)) : zero;
        };
        const __m128i a = load(i), b = load(i + 16), c = load(i + 32), d = load(i + 48);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), a);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
    }
    if (from) {
        std::memcpy(dst + i, from + i, bytes - i);
    } else {
        std::memset(dst + i, 0, bytes - i);
    }
    _mm_sfence(); // the stores are visible before the GPU is told to read them
#else
    if (src) {
        std::memcpy(dst, src, bytes);
    } else {
        std::memset(dst, 0, bytes);
    }
#endif
}

std::runtime_error sdl_error(std::string_view prefix) {
    std::string message{prefix};
    message += ": ";
    message += SDL_GetError();
    return std::runtime_error(message);
}

} // namespace

std::string gpu_support_report() {
    std::string report = "SDL_GPU drivers: ";
    const int driver_count = SDL_GetNumGPUDrivers();
    report += std::to_string(driver_count);
    for (int i = 0; i < driver_count; ++i) {
        const char* driver = SDL_GetGPUDriver(i);
        report += "\n  ";
        report += driver ? driver : "<unknown>";
        report += SDL_GPUSupportsShaderFormats(SDL_GPU_SHADERFORMAT_SPIRV, driver) ? " spirv" : "";
        report += SDL_GPUSupportsShaderFormats(SDL_GPU_SHADERFORMAT_DXIL, driver) ? " dxil" : "";
    }
    return report;
}

GpuDevice::GpuDevice(Window& window, bool debug)
    : GpuDevice(window, GpuDeviceOptions{.debug = debug}) {}

GpuDevice::GpuDevice(Window& window, GpuDeviceOptions options)
    : _window(static_cast<SDL_Window*>(window.native_handle())),
      _present_mode(options.present_mode) {
    _device = SDL_CreateGPUDevice(SupportedShaderFormats, options.debug, nullptr);
    if (!_device) {
        std::string message = "SDL_CreateGPUDevice(SPIRV) failed: ";
        message += SDL_GetError();
        message += "\n";
        message += gpu_support_report();
        throw std::runtime_error(message);
    }

    if (!SDL_ClaimWindowForGPUDevice(_device, _window)) {
        SDL_DestroyGPUDevice(_device);
        _device = nullptr;
        throw sdl_error("SDL_ClaimWindowForGPUDevice failed");
    }

    // Non-fatal: some (notably visible/resizable) windows reject explicit swapchain
    // params; the default swapchain from ClaimWindow (SDR + VSYNC) works fine.
    if (!SDL_SetGPUSwapchainParameters(_device, _window,
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       _present_mode)) {
        SDL_ClearError();
        _present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    }

    _swapchain_format = SDL_GetGPUSwapchainTextureFormat(_device, _window);
    if (_swapchain_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_ReleaseWindowFromGPUDevice(_device, _window);
        SDL_DestroyGPUDevice(_device);
        _device = nullptr;
        _window = nullptr;
        throw sdl_error("SDL_GPU swapchain unavailable for window");
    }
    _shared = std::make_shared<SDL_GPUDevice*>(_device);
}

GpuDevice::~GpuDevice() {
    if (!_device) {
        return;
    }
    try {
        flush_uploads();
    } catch (const std::exception&) {
        // The device is going away: its uploads with it.
    }
    if (_staging) {
        SDL_ReleaseGPUTransferBuffer(_device, _staging);
    }
    // Textures still alive (held past the renderer) must not release into the
    // destroyed device; they see a null handle from here on.
    *_shared = nullptr;
    SDL_WaitForGPUIdle(_device);
    release_upload_ring();
    if (_window) {
        SDL_ReleaseWindowFromGPUDevice(_device, _window);
    }
    SDL_DestroyGPUDevice(_device);
}

GpuDevice::GpuDevice(GpuDevice&& other) noexcept
    : _device(std::exchange(other._device, nullptr)),
      _shared(std::move(other._shared)),
      _window(std::exchange(other._window, nullptr)),
      _swapchain_format(std::exchange(other._swapchain_format, SDL_GPU_TEXTUREFORMAT_INVALID)),
      _present_mode(std::exchange(other._present_mode, SDL_GPU_PRESENTMODE_VSYNC)),
      _upload_ring(std::exchange(other._upload_ring, nullptr)),
      _upload_ring_size(std::exchange(other._upload_ring_size, 0)),
      _upload_ring_offset(std::exchange(other._upload_ring_offset, 0)),
      _first_submit_ns(std::exchange(other._first_submit_ns, 0)),
      _upload_commands(std::exchange(other._upload_commands, nullptr)),
      _upload_pass(std::exchange(other._upload_pass, nullptr)),
      _staging(std::exchange(other._staging, nullptr)),
      _staging_used(std::exchange(other._staging_used, 0)),
      _staging_size(std::exchange(other._staging_size, 0)),
      _big_upload_batch(std::exchange(other._big_upload_batch, 0)),
      _uploads_staged(std::exchange(other._uploads_staged, 0)),
      _upload_batches(std::exchange(other._upload_batches, 0)) {}

GpuFrame GpuDevice::begin_frame(bool acquire_swapchain) {
    return GpuFrame{*this, acquire_swapchain};
}

GpuTexture GpuDevice::create_render_texture(u32 width, u32 height, SDL_GPUTextureFormat format) {
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        format = _swapchain_format;
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture failed");
    }
    return GpuTexture{_shared, texture, width, height, format};
}

GpuTexture GpuDevice::create_texture_from_rgba(const u8* pixels, u32 width, u32 height) {
    if (!pixels) {
        throw std::runtime_error("create_texture_from_rgba failed: invalid arguments");
    }
    return create_texture(pixels, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 4);
}

GpuTexture GpuDevice::create_texture(const void* pixels, u32 width, u32 height, SDL_GPUTextureFormat format,
                                     u32 texel_bytes) {
    if (width == 0 || height == 0 || texel_bytes == 0) {
        throw std::runtime_error("create_texture failed: invalid arguments");
    }

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
        texture_info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* raw_texture = SDL_CreateGPUTexture(_device, &texture_info);
    if (!raw_texture) {
        throw sdl_error("SDL_CreateGPUTexture failed");
    }

    try {
        stage_texture_upload(raw_texture, 0, 0, width, height, pixels, texel_bytes, /*cycle=*/false);
    } catch (...) {
        SDL_ReleaseGPUTexture(_device, raw_texture);
        throw;
    }
    return GpuTexture{_shared, raw_texture, width, height, format};
}

void GpuDevice::update_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const u8* pixels,
                               u32 texel_bytes, bool whole) {
    if (!texture || !pixels || w == 0 || h == 0 || texel_bytes == 0) {
        throw std::runtime_error("update_texture failed: invalid arguments");
    }
    // Not cycled unless the whole texture is replaced: a cycled texture gets fresh
    // storage, and the texels outside the region would be lost. Uploads run in
    // order with the frames submitted around them, so frames submitted before read
    // the old texels and frames after the new.
    stage_texture_upload(texture, x, y, w, h, pixels, texel_bytes, whole);
}

void GpuDevice::stage_texture_upload(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const void* pixels,
                                     u32 texel_bytes, bool cycle) {
    const u64 bytes64 = static_cast<u64>(w) * h * texel_bytes;
    if (bytes64 > 0xFFFFFFFFull) {
        throw std::runtime_error("texture upload too large");
    }
    const u32 bytes = static_cast<u32>(bytes64);
    std::unique_lock lock{_uploads_mutex};
    const auto fill = [&](SDL_GPUTransferBuffer* transfer, u32 offset, bool cycle_transfer) {
        auto* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(_device, transfer, cycle_transfer));
        if (!mapped) {
            throw sdl_error("SDL_MapGPUTransferBuffer failed");
        }
        // Big ones in slices on the workers, when there are some: writes to this
        // memory scale with cores. (Below about 1 MB handing out costs more.)
        constexpr std::size_t Slice = 256u * 1024u;
        if (_jobs && bytes >= 4 * Slice) {
            const auto* from = static_cast<const u8*>(pixels);
            const auto slices = static_cast<i32>((static_cast<std::size_t>(bytes) + Slice - 1) / Slice);
            _jobs->parallel_for(slices, [&](i32 i) {
                const std::size_t at = static_cast<std::size_t>(i) * Slice;
                copy_to_gpu_memory(mapped + offset + at, from ? from + at : nullptr,
                                   std::min<std::size_t>(Slice, bytes - at));
            });
        } else {
            copy_to_gpu_memory(mapped + offset, pixels, bytes);
        }
        SDL_UnmapGPUTransferBuffer(_device, transfer);
    };
    const u32 needed = std::max(TextureStagingSize, next_power_of_two(bytes));
    if (_upload_commands && (align_up(_staging_used, TextureStagingAlignment) + bytes > _staging_size)) {
        lock.unlock();
        flush_uploads(); // full: send what is there
        lock.lock();
    }
    if (_staging && needed > _staging_size) {
        SDL_ReleaseGPUTransferBuffer(_device, _staging); // freed once its copies are done
        _staging = nullptr;
    }
    if (!_staging) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = needed;
        _staging = SDL_CreateGPUTransferBuffer(_device, &info);
        if (!_staging) {
            throw sdl_error("SDL_CreateGPUTransferBuffer failed");
        }
        _staging_size = info.size;
    }
    if (bytes > TextureStagingSize / 2) {
        _big_upload_batch = _upload_batches;
    }
    // The first upload of a batch cycles the staging buffer: if the GPU still
    // copies from the last batch, SDL hands out other memory instead of waiting.
    const u32 offset = align_up(_staging_used, TextureStagingAlignment);
    fill(_staging, offset, _staging_used == 0);
    _staging_used = offset + bytes;
    SDL_GPUTransferBuffer* transfer = _staging;
    if (!_upload_commands) {
        _upload_commands = SDL_AcquireGPUCommandBuffer(_device);
        if (!_upload_commands) {
            throw sdl_error("SDL_AcquireGPUCommandBuffer failed");
        }
        _upload_pass = SDL_BeginGPUCopyPass(_upload_commands);
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.offset = offset;
    source.pixels_per_row = w;
    source.rows_per_layer = h;
    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.x = x;
    destination.y = y;
    destination.w = w;
    destination.h = h;
    destination.d = 1;
    SDL_UploadToGPUTexture(_upload_pass, &source, &destination, cycle);
    ++_uploads_staged;
}

void GpuDevice::flush_uploads() {
    const std::lock_guard lock{_uploads_mutex};
    if (!_upload_commands) {
        return;
    }
    SDL_EndGPUCopyPass(_upload_pass);
    _upload_pass = nullptr;
    SDL_GPUCommandBuffer* commands = std::exchange(_upload_commands, nullptr);
    _staging_used = 0;
    if (_first_submit_ns == 0) {
        _first_submit_ns = SDL_GetTicksNS();
    }
    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    ++_upload_batches;
    if (_staging_size > TextureStagingSize && _upload_batches - _big_upload_batch > StagingShrinkAfterBatches) {
        // No big upload for a while: back to the usual size at the next upload.
        SDL_ReleaseGPUTransferBuffer(_device, _staging);
        _staging = nullptr;
        _staging_size = TextureStagingSize;
    }
    if (!submitted) {
        throw sdl_error("SDL_SubmitGPUCommandBuffer failed");
    }
}

GpuBuffer GpuDevice::create_buffer(SDL_GPUBufferUsageFlags usage, const void* data, u32 size) {
    if (size == 0) {
        throw std::runtime_error("create_buffer failed: buffer size must be non-zero");
    }

    SDL_GPUBufferCreateInfo buffer_info{};
    buffer_info.usage = usage;
    buffer_info.size = size;

    SDL_GPUBuffer* raw_buffer = SDL_CreateGPUBuffer(_device, &buffer_info);
    if (!raw_buffer) {
        throw sdl_error("SDL_CreateGPUBuffer failed");
    }

    GpuBuffer buffer{_device, raw_buffer, size};
    if (data) {
        upload_buffer(buffer, data, size);
    }
    return buffer;
}

void GpuDevice::upload_buffer(GpuBuffer& buffer, const void* data, u32 size) {
    GpuFrame frame = begin_frame();
    upload_buffer(frame, buffer, data, size);
    frame.submit();
}

void GpuDevice::upload_buffer(GpuFrame& frame, GpuBuffer& buffer, const void* data, u32 size) {
    frame.upload_buffer(buffer, data, size);
}

bool GpuDevice::read_texture_rgba(const GpuTexture& src, std::vector<u8>& out_rgba) {
    if (!_device || !src.handle() || src.width() == 0 || src.height() == 0) {
        return false;
    }
    flush_uploads();
    const u32 w = src.width();
    const u32 h = src.height();
    const u32 bytes = w * h * 4;

    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(_device, &tb_info);
    if (!tb) {
        return false;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(_device);
    if (!cmd) {
        SDL_ReleaseGPUTransferBuffer(_device, tb);
        return false;
    }

    SDL_GPUTextureRegion region{};
    region.texture = src.handle();
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    dst.pixels_per_row = w;
    dst.rows_per_layer = h;

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(_device, true, &fence, 1);
        SDL_ReleaseGPUFence(_device, fence);
    }

    bool ok = false;
    if (void* mapped = SDL_MapGPUTransferBuffer(_device, tb, false)) {
        out_rgba.resize(bytes);
        std::memcpy(out_rgba.data(), mapped, bytes);
        SDL_UnmapGPUTransferBuffer(_device, tb);
        ok = true;
    }
    SDL_ReleaseGPUTransferBuffer(_device, tb);
    return ok;
}

GpuDevice::UploadSlice GpuDevice::stage_upload_data(const void* data, u32 size, u32 alignment) {
    if (!data || size == 0) {
        throw std::runtime_error("stage_upload_data failed: invalid arguments");
    }

    const u32 required_size = next_power_of_two(std::max(DefaultUploadRingSize, align_up(size, alignment)));
    if (!_upload_ring || _upload_ring_size < required_size) {
        release_upload_ring();

        SDL_GPUTransferBufferCreateInfo transfer_info{};
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = required_size;
        _upload_ring = SDL_CreateGPUTransferBuffer(_device, &transfer_info);
        if (!_upload_ring) {
            throw sdl_error("SDL_CreateGPUTransferBuffer failed");
        }
        _upload_ring_size = required_size;
        _upload_ring_offset = 0;
    }

    u32 offset = align_up(_upload_ring_offset, alignment);
    bool cycle = false;
    if (offset + size > _upload_ring_size) {
        offset = 0;
        cycle = true;
    }

    void* mapped = SDL_MapGPUTransferBuffer(_device, _upload_ring, cycle);
    if (!mapped) {
        throw sdl_error("SDL_MapGPUTransferBuffer failed");
    }
    std::memcpy(static_cast<u8*>(mapped) + offset, data, size);
    SDL_UnmapGPUTransferBuffer(_device, _upload_ring);

    _upload_ring_offset = offset + size;
    return UploadSlice{_upload_ring, offset};
}

void GpuDevice::release_upload_ring() {
    if (_device && _upload_ring) {
        SDL_ReleaseGPUTransferBuffer(_device, _upload_ring);
    }
    _upload_ring = nullptr;
    _upload_ring_size = 0;
    _upload_ring_offset = 0;
}

void GpuDevice::clear_render_target(GpuFrame& frame, const GpuTexture& target, ClearColor color) {
    SDL_GPUColorTargetInfo color_target{};
    color_target.texture = target.handle();
    color_target.clear_color = SDL_FColor{color.r, color.g, color.b, color.a};
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.command_buffer(), &color_target, 1, nullptr);
    SDL_EndGPURenderPass(pass);
}

void GpuDevice::present_texture(GpuFrame& frame, const GpuTexture& texture, SDL_GPUFilter filter) {
    frame.acquire_swapchain();
    if (!frame.swapchain_texture()) {
        return;
    }
    SDL_GPUBlitInfo blit{};
    blit.source.texture = texture.handle();
    blit.source.w = texture.width();
    blit.source.h = texture.height();
    blit.destination.texture = frame.swapchain_texture();
    blit.destination.w = frame.swapchain_width();
    blit.destination.h = frame.swapchain_height();
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    blit.filter = filter;
    SDL_BlitGPUTexture(frame.command_buffer(), &blit);
}

void GpuDevice::wait_idle() {
    if (_device) {
        flush_uploads();
    }
    if (_device && !SDL_WaitForGPUIdle(_device)) {
        throw sdl_error("SDL_WaitForGPUIdle failed");
    }
}

const char* GpuDevice::driver_name() const {
    return _device ? SDL_GetGPUDeviceDriver(_device) : "";
}

GpuFrame::GpuFrame(GpuDevice& device, bool should_acquire_swapchain) : _device(&device) {
    _command_buffer = SDL_AcquireGPUCommandBuffer(device._device);
    if (!_command_buffer) {
        throw sdl_error("SDL_AcquireGPUCommandBuffer failed");
    }
    if (should_acquire_swapchain) {
        acquire_swapchain();
    }
}

GpuFrame::~GpuFrame() {
    if (_command_buffer && !_submitted) {
        SDL_CancelGPUCommandBuffer(_command_buffer);
    }
}

GpuFrame::GpuFrame(GpuFrame&& other) noexcept
    : _device(std::exchange(other._device, nullptr)),
      _command_buffer(std::exchange(other._command_buffer, nullptr)),
      _swapchain_texture(std::exchange(other._swapchain_texture, nullptr)),
      _swapchain_width(std::exchange(other._swapchain_width, 0)),
      _swapchain_height(std::exchange(other._swapchain_height, 0)),
      _submitted(std::exchange(other._submitted, true)) {}

bool GpuFrame::acquire_swapchain() {
    if (_swapchain_texture) {
        return true;
    }
    if (!_device || !_command_buffer) {
        return false;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(_command_buffer, _device->_window,
                                               &_swapchain_texture,
                                               &_swapchain_width,
                                               &_swapchain_height)) {
        throw sdl_error("SDL_WaitAndAcquireGPUSwapchainTexture failed");
    }
    return _swapchain_texture != nullptr;
}

void GpuFrame::submit() {
    if (!_command_buffer || _submitted) {
        return;
    }
    _device->flush_uploads(); // ahead of this frame, which may draw what they fill
    if (_device->_first_submit_ns == 0) {
        _device->_first_submit_ns = SDL_GetTicksNS();
    }
    if (!SDL_SubmitGPUCommandBuffer(_command_buffer)) {
        throw sdl_error("SDL_SubmitGPUCommandBuffer failed");
    }
    _submitted = true;
    _command_buffer = nullptr;
}

SDL_GPUFence* GpuFrame::submit_with_fence() {
    if (!_command_buffer || _submitted) {
        return nullptr;
    }
    _device->flush_uploads();
    if (_device->_first_submit_ns == 0) {
        _device->_first_submit_ns = SDL_GetTicksNS();
    }
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(_command_buffer);
    if (!fence) {
        throw sdl_error("SDL_SubmitGPUCommandBufferAndAcquireFence failed");
    }
    _submitted = true;
    _command_buffer = nullptr;
    return fence;
}

void GpuFrame::upload_buffer(GpuBuffer& buffer, const void* data, u32 size) {
    if (!buffer || !data || size == 0 || size > buffer.size() || !_device || !_command_buffer) {
        throw std::runtime_error("upload_buffer failed: invalid state");
    }
    const GpuDevice::UploadSlice upload = _device->stage_upload_data(data, size, 16);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = upload.transfer;
    source.offset = upload.offset;
    SDL_GPUBufferRegion destination{};
    destination.buffer = buffer.handle();
    destination.size = size;

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(_command_buffer);
    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);
}

} // namespace kin::gpu
