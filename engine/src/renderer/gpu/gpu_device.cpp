#include "gpu_device.hpp"

#include "gpu_memory.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <kin/assets/texture_file.hpp>
#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
constexpr u32 InitialAdaptiveStagingSize = 4u * 1024u * 1024u;
constexpr u32 MinimumStagingSize = 1024u * 1024u;
constexpr u32 MaximumGrownStagingSize =
    64u * 1024u * 1024u; // what batches of small writes can grow it to (one bigger write still grows it)
constexpr u32 StagingWindowBatches = 120;
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

// Waits for a read-back's fence and gives it back. When the wait fails (a lost device) the fence is not released: it
// may not be signalled, and SDL would hand it out again for another submission while the GPU could still be using it.
// It is left, a leak on a device that is gone, and the read-back fails.
bool wait_for_readback(SDL_GPUDevice* device, SDL_GPUFence* fence) {
    if (SDL_WaitForGPUFences(device, true, &fence, 1)) {
        SDL_ReleaseGPUFence(device, fence);
        return true;
    }
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        KIN_LOG_WARN_F("render", "GPU read-back dropped: waiting for its fence failed",
                       (LogFields{{.name = "reason", .value = SDL_GetError()}}));
    }
    return false;
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

GpuDevice::GpuDevice(Window& window, bool debug) : GpuDevice(window, GpuDeviceOptions{.debug = debug}) {}

GpuDevice::GpuDevice(Window& window, GpuDeviceOptions options)
    : _window(static_cast<SDL_Window*>(window.native_handle())), _present_mode(options.present_mode) {
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

    // The present mode asked for if the window's swapchain takes it. Without vsync (IMMEDIATE) MAILBOX comes next: it
    // does not wait for the screen either (a finished frame replaces one still waiting to be shown) and does not tear;
    // NVIDIA's Vulkan on Wayland has no IMMEDIATE, and VSYNC there held frames to whole refreshes. Non-fatal: some
    // (notably visible/resizable) windows reject explicit swapchain params; the default swapchain from ClaimWindow
    // (SDR + VSYNC) works fine.
    const SDL_GPUPresentMode asked = _present_mode;
    _present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    for (const SDL_GPUPresentMode mode :
         {asked, asked == SDL_GPU_PRESENTMODE_IMMEDIATE ? SDL_GPU_PRESENTMODE_MAILBOX : asked}) {
        if (SDL_WindowSupportsGPUPresentMode(_device, _window, mode) &&
            SDL_SetGPUSwapchainParameters(_device, _window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
            _present_mode = mode;
            break;
        }
        SDL_ClearError();
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
    _texture_pool = std::make_shared<GpuTexturePool>(_shared);
    register_memory_device(_device);
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
        tracked_release_transfer(_device, _staging);
    }
    if (_texture_pool) {
        _texture_pool->close();
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
    unregister_memory_device(_device);
}

GpuDevice::GpuDevice(GpuDevice&& other) noexcept
    : _device(std::exchange(other._device, nullptr)), _shared(std::move(other._shared)),
      _window(std::exchange(other._window, nullptr)),
      _swapchain_format(std::exchange(other._swapchain_format, SDL_GPU_TEXTUREFORMAT_INVALID)),
      _present_mode(std::exchange(other._present_mode, SDL_GPU_PRESENTMODE_VSYNC)),
      _upload_ring(std::exchange(other._upload_ring, nullptr)),
      _upload_ring_size(std::exchange(other._upload_ring_size, 0)),
      _upload_ring_offset(std::exchange(other._upload_ring_offset, 0)),
      _first_submit_ns(std::exchange(other._first_submit_ns, 0)),
      _upload_commands(std::exchange(other._upload_commands, nullptr)),
      _upload_pass(std::exchange(other._upload_pass, nullptr)), _staging(std::exchange(other._staging, nullptr)),
      _staging_used(std::exchange(other._staging_used, 0)), _staging_size(std::exchange(other._staging_size, 0)),
      _staging_recent_peak(std::exchange(other._staging_recent_peak, 0)),
      _staging_recent_batches(std::exchange(other._staging_recent_batches, 0)),
      _staging_overflow(std::exchange(other._staging_overflow, false)),
      _staging_spilled(std::exchange(other._staging_spilled, 0)),
      _staging_allocations(std::exchange(other._staging_allocations, 0)),
      _uploads_staged(std::exchange(other._uploads_staged, 0)),
      _upload_batches(std::exchange(other._upload_batches, 0)), _idle_waits(std::exchange(other._idle_waits, 0)) {
    _texture_pool = std::move(other._texture_pool);
}

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

    SDL_GPUTexture* texture = tracked_create_texture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture failed");
    }
    return GpuTexture{_shared, texture, width, height, format};
}

GpuTexture GpuDevice::create_target(u32 width, u32 height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                    SDL_GPUSampleCount samples, u32 levels) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    info.sample_count = samples;
    SDL_GPUTexture* texture = tracked_create_texture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture (target) failed");
    }
    poison_texture(texture, format, width, height, 1, levels, usage, samples);
    return GpuTexture{_shared, texture, width, height, format};
}

GpuTexture GpuDevice::create_target_array(u32 width, u32 height, u32 layers, SDL_GPUTextureFormat format,
                                          SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = layers;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = tracked_create_texture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture (target array) failed");
    }
    poison_texture(texture, format, width, height, layers, 1, usage);
    return GpuTexture{_shared, texture, width, height, format};
}

GpuTexture GpuDevice::create_volume(u32 width, u32 height, u32 depth, SDL_GPUTextureFormat format,
                                    SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_3D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = depth;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = tracked_create_texture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture (volume) failed");
    }
    return GpuTexture{_shared, texture, width, height, format};
}

SDL_GPUTextureFormat GpuDevice::depth_stencil_format() const {
    for (const SDL_GPUTextureFormat format :
         {SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT}) {
        if (SDL_GPUTextureSupportsFormat(_device, format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            return format;
        }
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

GpuTexture GpuDevice::create_depth_stencil(u32 width, u32 height) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = depth_stencil_format();
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = tracked_create_texture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture (depth-stencil) failed");
    }
    poison_texture(texture, info.format, width, height, 1, 1, info.usage);
    return GpuTexture{_shared, texture, width, height, info.format};
}

GpuTexture GpuDevice::create_texture_from_rgba(const u8* pixels, u32 width, u32 height) {
    if (!pixels) {
        throw std::runtime_error("create_texture_from_rgba failed: invalid arguments");
    }
    return create_texture(pixels, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 4);
}

GpuTexture GpuDevice::create_texture(const void* pixels, u32 width, u32 height, SDL_GPUTextureFormat format,
                                     u32 texel_bytes, SDL_GPUTextureUsageFlags extra_usage) {
    if (width == 0 || height == 0 || texel_bytes == 0) {
        throw std::runtime_error("create_texture failed: invalid arguments");
    }

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    // An empty texture is cleared on the GPU rather than sent zeros, which takes
    // being a render target: RGBA8 textures are anyway, data formats where the
    // device allows (it is one more kind, so pooled apart).
    const bool clear_on_gpu =
        format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
        (!pixels && SDL_GPUTextureSupportsFormat(_device, format, SDL_GPU_TEXTURETYPE_2D,
                                                 SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET));
    if (clear_on_gpu) {
        texture_info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    texture_info.usage |= extra_usage;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    // A released texture of the same kind, if the pool has one: its first upload
    // replaces it whole, cycled in case a frame on the GPU still reads it.
    SDL_GPUTexture* raw_texture = _texture_pool->take(width, height, format, texture_info.usage);
    const bool reused = raw_texture != nullptr;
    if (!reused) {
        raw_texture = tracked_create_texture(_device, &texture_info);
        if (!raw_texture) {
            throw sdl_error("SDL_CreateGPUTexture failed");
        }
    }

    try {
        if (!pixels && clear_on_gpu) {
            clear_texture(raw_texture, /*cycle=*/reused);
        } else {
            stage_texture_upload(raw_texture, 0, 0, width, height, pixels, texel_bytes, /*cycle=*/reused);
        }
    } catch (...) {
        tracked_release_texture(_device, raw_texture);
        throw;
    }
    const u64 bytes = static_cast<u64>(width) * height * texel_bytes;
    return GpuTexture{_shared, raw_texture, width, height, format, _texture_pool, bytes, texture_info.usage};
}

SDL_GPUTextureFormat GpuDevice::compressed_format(TextureFormat format, bool decode_srgb) {
    switch (format) {
    case TextureFormat::Bc1RgbaUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case TextureFormat::Bc1RgbaSrgb:
        return decode_srgb ? SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case TextureFormat::Bc3RgbaUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case TextureFormat::Bc3RgbaSrgb:
        return decode_srgb ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case TextureFormat::Bc4RUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
    case TextureFormat::Bc5RgUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    case TextureFormat::Bc7RgbaUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case TextureFormat::Bc7RgbaSrgb:
        return decode_srgb ? SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case TextureFormat::Astc4x4RgbaUnorm:
        return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
    case TextureFormat::Astc4x4RgbaSrgb:
        return decode_srgb ? SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
    default:
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

GpuTexture GpuDevice::create_texture_compressed(const CompressedImage& image, bool decode_srgb) {
    const SDL_GPUTextureFormat format = compressed_format(image.format, decode_srgb);
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID || !image.valid()) {
        throw std::runtime_error("create_texture_compressed failed: invalid image");
    }
    if (!SDL_GPUTextureSupportsFormat(_device, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        return {};
    }
    const auto width = static_cast<u32>(image.size.x);
    const auto height = static_cast<u32>(image.size.y);
    const auto levels = static_cast<u32>(image.levels.size());
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    // As in create_texture, a released one of the same kind (its level count too)
    // if the pool has one. Every level is replaced; the first upload cycles the
    // whole texture, in case a frame on the GPU still reads it.
    SDL_GPUTexture* texture = _texture_pool->take(width, height, format, info.usage, levels);
    const bool reused = texture != nullptr;
    if (!reused) {
        texture = tracked_create_texture(_device, &info);
        if (!texture) {
            throw sdl_error("SDL_CreateGPUTexture (compressed) failed");
        }
    }
    u64 bytes = 0;
    try {
        for (u32 level = 0; level < levels; ++level) {
            const auto& from = image.levels[level];
            const UploadDestination to{.texture = texture,
                                       .w = static_cast<u32>(from.size.x),
                                       .h = static_cast<u32>(from.size.y),
                                       .level = level,
                                       .blocks = true};
            stage_upload(to, static_cast<u32>(from.bytes.size()), from.bytes.data(), reused && level == 0, nullptr);
            bytes += from.bytes.size();
        }
    } catch (...) {
        tracked_release_texture(_device, texture);
        throw;
    }
    return GpuTexture{_shared, texture, width, height, format, _texture_pool, bytes, info.usage, levels};
}

void GpuDevice::update_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const u8* pixels, u32 texel_bytes,
                               bool whole) {
    if (!texture || !pixels || w == 0 || h == 0 || texel_bytes == 0) {
        throw std::runtime_error("update_texture failed: invalid arguments");
    }
    // Not cycled unless the whole texture is replaced: a cycled texture gets fresh
    // storage, and the texels outside the region would be lost. Uploads run in
    // order with the frames submitted around them, so frames submitted before read
    // the old texels and frames after the new.
    stage_texture_upload(texture, x, y, w, h, pixels, texel_bytes, whole);
}

void GpuDevice::write_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, u32 texel_bytes, bool whole,
                              const std::function<void(std::span<u8>)>& fill) {
    if (!texture || w == 0 || h == 0 || texel_bytes == 0) {
        throw std::runtime_error("write_texture failed: invalid arguments");
    }
    stage_texture_upload(texture, x, y, w, h, nullptr, texel_bytes, whole, &fill);
}

void GpuDevice::stage_texture_upload(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const void* pixels,
                                     u32 texel_bytes, bool cycle, const std::function<void(std::span<u8>)>* write) {
    const u64 bytes64 = static_cast<u64>(w) * h * texel_bytes;
    if (bytes64 > 0xFFFFFFFFull) {
        throw std::runtime_error("texture upload too large");
    }
    stage_upload(UploadDestination{.texture = texture, .x = x, .y = y, .w = w, .h = h}, static_cast<u32>(bytes64),
                 pixels, cycle, write);
}

void GpuDevice::upload_storage_buffer(SDL_GPUBuffer* buffer, u32 offset, u32 bytes, const void* data, bool cycle,
                                      const std::function<void(std::span<u8>)>* write) {
    if (!buffer || bytes == 0) {
        throw std::runtime_error("upload_storage_buffer failed: invalid arguments");
    }
    stage_upload(UploadDestination{.buffer = buffer, .buffer_offset = offset}, bytes, data, cycle, write);
}

void GpuDevice::upload_storage_ranges(SDL_GPUBuffer* buffer, std::span<const std::array<u32, 2>> ranges,
                                      const std::function<void(std::span<u8>)>& fill) {
    u64 total = 0;
    for (const auto& range : ranges)
        total += range[1];
    if (!buffer || total > 0xFFFFFFFFull) {
        throw std::runtime_error("upload_storage_ranges failed: invalid arguments");
    }
    if (total == 0)
        return;
    stage_upload(UploadDestination{.buffer = buffer, .ranges = ranges}, static_cast<u32>(total), nullptr, false, &fill);
}

void GpuDevice::stage_upload(const UploadDestination& to, u32 bytes, const void* pixels, bool cycle,
                             const std::function<void(std::span<u8>)>* write) {
    refuse_upload_inside_fill();
    std::unique_lock lock{_uploads_mutex};
    const auto copy_in = [&](SDL_GPUTransferBuffer* transfer, u32 offset, bool cycle_transfer) {
        auto* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(_device, transfer, cycle_transfer));
        if (!mapped) {
            throw sdl_error("SDL_MapGPUTransferBuffer failed");
        }
        // Big ones in slices on the workers, when there are some: writes to this
        // memory scale with cores. (Below about 1 MB handing out costs more.)
        constexpr std::size_t Slice = 256u * 1024u;
        if (write) {
            _filling = std::this_thread::get_id();
            try {
                (*write)(std::span<u8>{mapped + offset, bytes});
            } catch (...) {
                _filling = std::thread::id{};
                SDL_UnmapGPUTransferBuffer(_device, transfer);
                throw;
            }
            _filling = std::thread::id{};
        } else if (_jobs && bytes >= 4 * Slice) {
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
    // A small scene should not cycle 16 MiB backings throughout startup,
    // before the first observation window can shrink them. Large writes grow
    // immediately; after a window retain its observed working-set floor.
    const u32 floor = _staging_size ? _staging_size : InitialAdaptiveStagingSize;
    const u32 needed = std::max(floor, next_power_of_two(bytes));
    if (_upload_commands && (align_up(_staging_used, TextureStagingAlignment) + bytes > _staging_size)) {
        _staging_overflow = true; // a part of the batch: its flush is not the batch's end
        lock.unlock();
        flush_uploads(); // full: send what is there
        lock.lock();
    }
    if (_staging && needed > _staging_size) {
        tracked_release_transfer(_device, _staging); // freed once its copies are done
        _staging = nullptr;
    }
    if (!_staging) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = needed;
        _staging = tracked_create_transfer(_device, &info);
        if (!_staging) {
            throw sdl_error("SDL_CreateGPUTransferBuffer failed");
        }
        _staging_size = info.size;
        ++_staging_allocations;
    }

    // The first upload of a batch cycles the staging buffer: if the GPU still
    // copies from the last batch, SDL hands out other memory instead of waiting.
    const u32 offset = align_up(_staging_used, TextureStagingAlignment);
    copy_in(_staging, offset, _staging_used == 0);
    _staging_used = offset + bytes;
    SDL_GPUTransferBuffer* transfer = _staging;
    begin_upload_commands();
    if (!_upload_pass) {
        _upload_pass = SDL_BeginGPUCopyPass(_upload_commands);
    }
    if (to.buffer && !to.ranges.empty()) {
        u32 at = offset;
        for (const auto& [where, size] : to.ranges) {
            if (size == 0)
                continue;
            const SDL_GPUTransferBufferLocation source{.transfer_buffer = transfer, .offset = at};
            const SDL_GPUBufferRegion destination{.buffer = to.buffer, .offset = where, .size = size};
            SDL_UploadToGPUBuffer(_upload_pass, &source, &destination, false);
            at += size;
        }
    } else if (to.buffer) {
        const SDL_GPUTransferBufferLocation source{.transfer_buffer = transfer, .offset = offset};
        const SDL_GPUBufferRegion destination{.buffer = to.buffer, .offset = to.buffer_offset, .size = bytes};
        SDL_UploadToGPUBuffer(_upload_pass, &source, &destination, cycle);
    } else {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer;
        source.offset = offset;
        source.pixels_per_row = to.blocks ? 0 : to.w; // 0: as packed as the region (a 2 x 2 level is one 4 x 4 block)
        source.rows_per_layer = to.blocks ? 0 : to.h;
        SDL_GPUTextureRegion destination{};
        destination.texture = to.texture;
        destination.mip_level = to.level;
        destination.x = to.x;
        destination.y = to.y;
        destination.w = to.w;
        destination.h = to.h;
        destination.d = 1;
        SDL_UploadToGPUTexture(_upload_pass, &source, &destination, cycle);
    }
    ++_uploads_staged;
}

void GpuDevice::begin_upload_commands() {
    if (!_upload_commands) {
        _upload_commands = SDL_AcquireGPUCommandBuffer(_device);
        if (!_upload_commands) {
            throw sdl_error("SDL_AcquireGPUCommandBuffer failed");
        }
    }
}

void GpuDevice::end_upload_pass() {
    if (_upload_pass) {
        SDL_EndGPUCopyPass(_upload_pass); // reopened by the next upload, in order
        _upload_pass = nullptr;
    }
}

GpuTexture GpuDevice::make_mipmapped(const GpuTexture& source) {
    const u32 w = source.width(), h = source.height();
    const u32 levels = complete_mip_levels(w, h); // not by the longer side: see the header
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = source.format();
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; // generating needs both
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = tracked_create_texture(_device, &info);
    if (!texture) {
        throw sdl_error("SDL_CreateGPUTexture failed");
    }
    poison_texture(texture, info.format, w, h, 1, levels, info.usage, SDL_GPU_SAMPLECOUNT_1,
                   /*first_level=*/1); // KIN_POISON_TEXTURES: before level 0 is copied
    refuse_upload_inside_fill();
    const std::lock_guard lock{_uploads_mutex};
    begin_upload_commands();
    end_upload_pass();
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(_upload_commands);
    const SDL_GPUTextureLocation from{.texture = source.handle()};
    const SDL_GPUTextureLocation to{.texture = texture};
    SDL_CopyGPUTextureToTexture(pass, &from, &to, w, h, 1, false);
    SDL_EndGPUCopyPass(pass);
    if (levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(_upload_commands, texture);
    }
    return GpuTexture{_shared, texture, w, h, source.format()}; // not pooled: a kind of its own
}

void GpuDevice::generate_mipmaps(SDL_GPUTexture* texture) {
    refuse_upload_inside_fill();
    const std::lock_guard lock{_uploads_mutex};
    begin_upload_commands();
    end_upload_pass();
    SDL_GenerateMipmapsForGPUTexture(_upload_commands, texture);
}

u32 GpuDevice::complete_mip_levels(u32 width, u32 height) {
    u32 levels = 1;
    while ((std::min(width, height) >> levels) > 0) {
        ++levels;
    }
    return levels;
}

void GpuDevice::poison_texture(SDL_GPUTexture* texture, SDL_GPUTextureFormat format, u32 width, u32 height, u32 layers,
                               u32 levels, SDL_GPUTextureUsageFlags usage, SDL_GPUSampleCount samples,
                               u32 first_level) {
    const char* env = std::getenv("KIN_POISON_TEXTURES");
    const int mode = env ? std::atoi(env) : 0;
    if (mode <= 0 || !texture) {
        return;
    }
    const bool large = mode >= 2;
    refuse_upload_inside_fill();
    const std::lock_guard lock{_uploads_mutex};
    begin_upload_commands();
    end_upload_pass();
    if (usage & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) {
        for (u32 layer = 0; layer < layers; ++layer) {
            SDL_GPUDepthStencilTargetInfo target{};
            target.texture = texture;
            target.clear_depth = 0.37f;
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            target.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
            target.stencil_store_op = SDL_GPU_STOREOP_STORE;
            target.clear_stencil = 0x5A;
            target.layer = static_cast<Uint8>(layer);
            if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(_upload_commands, nullptr, 0, &target)) {
                SDL_EndGPURenderPass(pass);
            }
        }
        return;
    }
    // Bytes of one texel and the pattern that fills it.
    u32 texel = 0;
    u16 half = large ? 0x7BFF : 0x7E00;             // 65504 or NaN
    u32 single = large ? 0x46EA6000u : 0x7FC00000u; // 3e4 or NaN
    bool is_half = false, is_single = false;
    switch (format) {
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
        texel = 8;
        is_half = true;
        break;
    case SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT:
        texel = 4;
        is_half = true;
        break;
    case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
        texel = 2;
        is_half = true;
        break;
    case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
        texel = 4;
        is_single = true;
        break;
    case SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT:
        texel = 8;
        is_single = true;
        break;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT:
        texel = 16;
        is_single = true;
        break;
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
        texel = 1;
        break;
    case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
        texel = 2;
        break;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
        texel = 4;
        break;
    default:
        return; // not a format the renderer makes
    }
    if (samples != SDL_GPU_SAMPLECOUNT_1) {
        // A multisampled target cannot be uploaded to: cleared (a render pass), to NaN or white/large.
        if (usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) {
            const float v = is_half || is_single ? (large ? 3.0e4f : std::numeric_limits<float>::quiet_NaN()) : 1.0f;
            SDL_GPUColorTargetInfo target{};
            target.texture = texture;
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            target.clear_color = {v, v, v, v};
            if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(_upload_commands, &target, 1, nullptr)) {
                SDL_EndGPURenderPass(pass);
            }
        }
        return;
    }
    std::vector<u8> bytes;
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 level = first_level; level < levels; ++level) {
            const u32 w = std::max(1u, width >> level), h = std::max(1u, height >> level);
            const u64 total = static_cast<u64>(w) * h * texel;
            bytes.resize(static_cast<std::size_t>(total));
            if (is_half) {
                for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
                    std::memcpy(&bytes[i], &half, 2);
                }
            } else if (is_single) {
                for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
                    std::memcpy(&bytes[i], &single, 4);
                }
            } else {
                std::fill(bytes.begin(), bytes.end(), u8{0xFF});
            }
            // Staged like any upload, but to one mip level and layer of the texture.
            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = static_cast<u32>(total);
            SDL_GPUTransferBuffer* transfer = tracked_create_transfer(_device, &info);
            if (!transfer) {
                return;
            }
            if (auto* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(_device, transfer, false))) {
                std::memcpy(mapped, bytes.data(), bytes.size());
                SDL_UnmapGPUTransferBuffer(_device, transfer);
                if (!_upload_pass) {
                    _upload_pass = SDL_BeginGPUCopyPass(_upload_commands);
                }
                SDL_GPUTextureTransferInfo source{};
                source.transfer_buffer = transfer;
                source.pixels_per_row = w;
                source.rows_per_layer = h;
                SDL_GPUTextureRegion destination{};
                destination.texture = texture;
                destination.mip_level = level;
                destination.layer = layer;
                destination.w = w;
                destination.h = h;
                destination.d = 1;
                SDL_UploadToGPUTexture(_upload_pass, &source, &destination, false);
            }
            tracked_release_transfer(_device, transfer); // freed once the copy is done
        }
    }
}

void GpuDevice::poison_buffer(SDL_GPUBuffer* buffer, u32 bytes) {
    const char* env = std::getenv("KIN_POISON_BUFFERS");
    if (!env || std::atoi(env) <= 0 || !buffer || bytes == 0) {
        return;
    }
    std::vector<u8> fill(bytes, u8{0xFF});
    upload_storage_buffer(buffer, 0, bytes, fill.data(), false);
}

void GpuDevice::clear_texture(SDL_GPUTexture* texture, bool cycle) {
    refuse_upload_inside_fill();
    const std::lock_guard lock{_uploads_mutex};
    begin_upload_commands();
    end_upload_pass();
    SDL_GPUColorTargetInfo target{};
    target.texture = texture;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    target.cycle = cycle;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(_upload_commands, &target, 1, nullptr));
}

void GpuDevice::refuse_upload_inside_fill() const {
    if (_filling.load() == std::this_thread::get_id()) {
        throw std::logic_error("write_texture: the fill used the renderer (upload or submit)");
    }
}

void GpuDevice::flush_uploads() {
    refuse_upload_inside_fill();
    const std::lock_guard lock{_uploads_mutex};
    if (_texture_pool) {
        _texture_pool->tick(); // before each submission: about once a frame
    }
    if (!_upload_commands) {
        return;
    }
    if (_upload_pass) {
        SDL_EndGPUCopyPass(_upload_pass);
        _upload_pass = nullptr;
    }
    SDL_GPUCommandBuffer* commands = std::exchange(_upload_commands, nullptr);
    const u32 batch_bytes = std::exchange(_staging_used, 0);
    if (_first_submit_ns == 0) {
        _first_submit_ns = SDL_GetTicksNS();
    }
    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    ++_upload_batches;
    const bool part = std::exchange(_staging_overflow, false);
    if (part) {
        _staging_spilled += batch_bytes; // the batch goes on in the next part
    } else {
        // Use aggregate batch occupancy, not the largest individual upload:
        // many small writes can together need a large buffer. Delayed shrinking
        // and a 2x size threshold avoid resizing after isolated quiet frames.
        // A batch that had to be sent in parts grows the buffer to what it needed
        // (up to MaximumGrownStagingSize), so that the next one like it is one
        // submission: the parts cycled as many backings as one buffer that size.
        const u64 needed = _staging_spilled + batch_bytes;
        _staging_spilled = 0;
        const u32 peak = static_cast<u32>(std::min<u64>(needed, MaximumGrownStagingSize));
        _staging_recent_peak = std::max(_staging_recent_peak, peak);
        if (needed > _staging_size && _staging_size < MaximumGrownStagingSize) {
            // As when shrinking: SDL frees the old buffer once its copies are done, and the next upload makes the new
            // one.
            if (_staging)
                tracked_release_transfer(_device, _staging);
            _staging = nullptr;
            _staging_size = next_power_of_two(peak);
        }
        if (++_staging_recent_batches == StagingWindowBatches) {
            const u32 target = std::max(MinimumStagingSize, next_power_of_two(_staging_recent_peak));
            if (_staging && target <= _staging_size / 2) {
                // SDL defers disposal until submitted copies finish. The next
                // upload allocates the smaller buffer; no new GPU wait is needed.
                tracked_release_transfer(_device, _staging);
                _staging = nullptr;
                _staging_size = target;
            }
            _staging_recent_peak = _staging_recent_batches = 0;
        }
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

    SDL_GPUBuffer* raw_buffer = tracked_create_buffer(_device, &buffer_info);
    if (!raw_buffer) {
        throw sdl_error("SDL_CreateGPUBuffer failed");
    }

    GpuBuffer buffer{_shared, raw_buffer, size};
    if (data) {
        upload_buffer(buffer, data, size);
    } else {
        poison_buffer(raw_buffer, size);
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
    return read_texture_region(src, 0, 0, src.width(), src.height(), 4, out_rgba);
}

bool GpuDevice::read_texture_region(const GpuTexture& src, u32 x, u32 y, u32 w, u32 h, u32 texel_bytes,
                                    std::vector<u8>& out) {
    PendingRead read;
    if (!begin_read_texture_region(src, x, y, w, h, texel_bytes, read)) {
        return false;
    }
    std::vector<u8> pixels;
    (void)finish_read(read, pixels, true);
    if (pixels.empty()) {
        return false;
    }
    out = std::move(pixels);
    return true;
}

bool GpuDevice::begin_read_texture_region(const GpuTexture& src, u32 x, u32 y, u32 w, u32 h, u32 texel_bytes,
                                          PendingRead& read) {
    read = {};
    if (!_device || !src.handle() || w == 0 || h == 0 || x + w > src.width() || y + h > src.height()) {
        return false;
    }
    flush_uploads();
    const u32 bytes = w * h * texel_bytes;

    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = bytes;
    SDL_GPUTransferBuffer* tb = tracked_create_transfer(_device, &tb_info);
    if (!tb) {
        return false;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(_device);
    if (!cmd) {
        tracked_release_transfer(_device, tb);
        return false;
    }

    SDL_GPUTextureRegion region{};
    region.texture = src.handle();
    region.x = x;
    region.y = y;
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

    // No fence (the submit failed): the copy may not have run, and finish_read gives nothing.
    read = {.transfer = tb, .fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd), .bytes = bytes};
    return true;
}

bool GpuDevice::finish_read(PendingRead& read, std::vector<u8>& out, bool wait) {
    out.clear();
    if (!read.transfer) {
        return true;
    }
    bool arrived = false;
    if (read.fence) {
        if (wait) {
            // (A failed wait keeps its fence: SDL must not reuse one the GPU may still signal.)
            arrived = wait_for_readback(_device, read.fence);
        } else if (SDL_QueryGPUFence(_device, read.fence)) {
            SDL_ReleaseGPUFence(_device, read.fence);
            arrived = true;
        } else {
            return false;
        }
    }
    if (void* mapped = arrived ? SDL_MapGPUTransferBuffer(_device, read.transfer, false) : nullptr) {
        out.resize(read.bytes);
        std::memcpy(out.data(), mapped, read.bytes);
        SDL_UnmapGPUTransferBuffer(_device, read.transfer);
    }
    tracked_release_transfer(_device, read.transfer);
    read = {};
    return true;
}

bool GpuDevice::read_volume(const GpuTexture& src, u32 depth, u32 texel_bytes, std::vector<u8>& out) {
    if (!_device || !src.handle() || depth == 0) {
        return false;
    }
    flush_uploads();
    const u64 total = static_cast<u64>(src.width()) * src.height() * depth * texel_bytes;
    if (total > std::numeric_limits<u32>::max()) {
        return false;
    }
    const u32 bytes = static_cast<u32>(total);
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = bytes;
    SDL_GPUTransferBuffer* tb = tracked_create_transfer(_device, &tb_info);
    if (!tb) {
        return false;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(_device);
    if (!cmd) {
        tracked_release_transfer(_device, tb);
        return false;
    }
    SDL_GPUTextureRegion region{};
    region.texture = src.handle();
    region.w = src.width();
    region.h = src.height();
    region.d = depth;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    dst.pixels_per_row = src.width();
    dst.rows_per_layer = src.height();
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    const bool arrived = fence && wait_for_readback(_device, fence); // as read_texture_region
    bool ok = false;
    if (void* mapped = arrived ? SDL_MapGPUTransferBuffer(_device, tb, false) : nullptr) {
        out.resize(bytes);
        std::memcpy(out.data(), mapped, bytes);
        SDL_UnmapGPUTransferBuffer(_device, tb);
        ok = true;
    }
    tracked_release_transfer(_device, tb);
    return ok;
}

bool GpuDevice::read_buffer(SDL_GPUBuffer* src, u32 offset, u32 bytes, std::vector<u8>& out) {
    if (!_device || !src || bytes == 0) {
        return false;
    }
    flush_uploads();
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = bytes;
    SDL_GPUTransferBuffer* tb = tracked_create_transfer(_device, &tb_info);
    if (!tb) {
        return false;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(_device);
    if (!cmd) {
        tracked_release_transfer(_device, tb);
        return false;
    }
    const SDL_GPUBufferRegion region{src, offset, bytes};
    const SDL_GPUTransferBufferLocation location{tb, 0};
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUBuffer(copy, &region, &location);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    const bool arrived = fence && wait_for_readback(_device, fence); // as above
    bool ok = false;
    if (void* mapped = arrived ? SDL_MapGPUTransferBuffer(_device, tb, false) : nullptr) {
        out.resize(bytes);
        std::memcpy(out.data(), mapped, bytes);
        SDL_UnmapGPUTransferBuffer(_device, tb);
        ok = true;
    }
    tracked_release_transfer(_device, tb);
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
        _upload_ring = tracked_create_transfer(_device, &transfer_info);
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
        tracked_release_transfer(_device, _upload_ring);
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
    ++_idle_waits;
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
    : _device(std::exchange(other._device, nullptr)), _command_buffer(std::exchange(other._command_buffer, nullptr)),
      _swapchain_texture(std::exchange(other._swapchain_texture, nullptr)),
      _swapchain_width(std::exchange(other._swapchain_width, 0)),
      _swapchain_height(std::exchange(other._swapchain_height, 0)), _submitted(std::exchange(other._submitted, true)) {}

bool GpuFrame::acquire_swapchain() {
    if (_swapchain_texture) {
        return true;
    }
    if (!_device || !_command_buffer) {
        return false;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(_command_buffer, _device->_window, &_swapchain_texture,
                                               &_swapchain_width, &_swapchain_height)) {
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
