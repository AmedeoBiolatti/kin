#include "gpu_device.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kin::gpu {

namespace {

// Force SPIR-V so SDL_GPU selects the Vulkan backend.
constexpr SDL_GPUShaderFormat SupportedShaderFormats = SDL_GPU_SHADERFORMAT_SPIRV;
constexpr u32 DefaultUploadRingSize = 1024u * 1024u;

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
}

GpuDevice::~GpuDevice() {
    if (!_device) {
        return;
    }
    SDL_WaitForGPUIdle(_device);
    release_upload_ring();
    if (_window) {
        SDL_ReleaseWindowFromGPUDevice(_device, _window);
    }
    SDL_DestroyGPUDevice(_device);
}

GpuDevice::GpuDevice(GpuDevice&& other) noexcept
    : _device(std::exchange(other._device, nullptr)),
      _window(std::exchange(other._window, nullptr)),
      _swapchain_format(std::exchange(other._swapchain_format, SDL_GPU_TEXTUREFORMAT_INVALID)),
      _present_mode(std::exchange(other._present_mode, SDL_GPU_PRESENTMODE_VSYNC)),
      _upload_ring(std::exchange(other._upload_ring, nullptr)),
      _upload_ring_size(std::exchange(other._upload_ring_size, 0)),
      _upload_ring_offset(std::exchange(other._upload_ring_offset, 0)) {}

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
    return GpuTexture{_device, texture, width, height, format};
}

GpuTexture GpuDevice::create_texture_from_rgba(const u8* pixels, u32 width, u32 height) {
    if (!pixels || width == 0 || height == 0) {
        throw std::runtime_error("create_texture_from_rgba failed: invalid arguments");
    }

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* raw_texture = SDL_CreateGPUTexture(_device, &texture_info);
    if (!raw_texture) {
        throw sdl_error("SDL_CreateGPUTexture failed");
    }

    const u32 byte_count = width * height * 4;
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(_device, &transfer_info);
    if (!transfer) {
        SDL_ReleaseGPUTexture(_device, raw_texture);
        throw sdl_error("SDL_CreateGPUTransferBuffer failed");
    }

    void* mapped = SDL_MapGPUTransferBuffer(_device, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(_device, transfer);
        SDL_ReleaseGPUTexture(_device, raw_texture);
        throw sdl_error("SDL_MapGPUTransferBuffer failed");
    }
    std::memcpy(mapped, pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(_device, transfer);

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(_device);
    if (!command_buffer) {
        SDL_ReleaseGPUTransferBuffer(_device, transfer);
        SDL_ReleaseGPUTexture(_device, raw_texture);
        throw sdl_error("SDL_AcquireGPUCommandBuffer failed");
    }

    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.pixels_per_row = width;
    source.rows_per_layer = height;

    SDL_GPUTextureRegion destination{};
    destination.texture = raw_texture;
    destination.w = width;
    destination.h = height;
    destination.d = 1;

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);

    if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
        SDL_ReleaseGPUTransferBuffer(_device, transfer);
        SDL_ReleaseGPUTexture(_device, raw_texture);
        throw sdl_error("SDL_SubmitGPUCommandBuffer failed");
    }

    SDL_ReleaseGPUTransferBuffer(_device, transfer);
    return GpuTexture{_device, raw_texture, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
}

void GpuDevice::update_texture(SDL_GPUTexture* texture, u32 x, u32 y, u32 w, u32 h, const u8* pixels) {
    if (!texture || !pixels || w == 0 || h == 0) {
        throw std::runtime_error("update_texture failed: invalid arguments");
    }
    const u32 byte_count = w * h * 4;
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = byte_count;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(_device, &transfer_info);
    if (!transfer) {
        throw sdl_error("SDL_CreateGPUTransferBuffer failed");
    }
    void* mapped = SDL_MapGPUTransferBuffer(_device, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(_device, transfer);
        throw sdl_error("SDL_MapGPUTransferBuffer failed");
    }
    std::memcpy(mapped, pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(_device, transfer);
    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(_device);
    if (!command_buffer) {
        SDL_ReleaseGPUTransferBuffer(_device, transfer);
        throw sdl_error("SDL_AcquireGPUCommandBuffer failed");
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.pixels_per_row = w;
    source.rows_per_layer = h;
    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.x = x;
    destination.y = y;
    destination.w = w;
    destination.h = h;
    destination.d = 1;
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    // Not cycled: a cycled texture gets fresh storage, and the texels outside the
    // region would be lost. Command buffers run in the order they are submitted, so
    // frames submitted before read the old texels and frames after the new.
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);
    if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
        SDL_ReleaseGPUTransferBuffer(_device, transfer);
        throw sdl_error("SDL_SubmitGPUCommandBuffer failed");
    }
    SDL_ReleaseGPUTransferBuffer(_device, transfer);
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
    if (!SDL_SubmitGPUCommandBuffer(_command_buffer)) {
        throw sdl_error("SDL_SubmitGPUCommandBuffer failed");
    }
    _submitted = true;
    _command_buffer = nullptr;
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
