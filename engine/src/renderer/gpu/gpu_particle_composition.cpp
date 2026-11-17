#include "gpu_device.hpp"
#include "gpu_renderer2d_backend.hpp"
#include "gpu_renderer3d.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace kin::gpu {
void GpuRenderer3D::composition_buffer(std::shared_ptr<GpuDataBuffer>& buffer, u32 bytes,
                                       SDL_GPUBufferUsageFlags usage) {
    bytes = std::max(bytes, 16u);
    if (!buffer || buffer->size() < bytes)
        buffer = std::make_shared<GpuDataBuffer>(_device->create_buffer(usage, nullptr, std::bit_ceil(bytes)));
    _owner->_retained_buffers.push_back(buffer);
}

void GpuRenderer3D::prepare_particle_atlas(std::span<const Texture> textures, bool decode) {
    std::vector<CompositionImage> images;
    bool changed = decode != _composition_decode || textures.size() != _composition_images.size();
    u32 levels = 0, max_width = 1, max_height = 1;
    for (u32 i = 0; i < textures.size(); ++i) {
        const auto* t = dynamic_cast<const GpuTextureBackend*>(textures[i].backend().get());
        if (!t || !t->texture().belongs_to(_device->handle()) || t->array_layers() ||
            !(t->texture().usage() & SDL_GPU_TEXTUREUSAGE_SAMPLER))
            throw std::runtime_error("foreign particle composition texture");
        CompositionImage image;
        image.texture = textures[i];
        image.handle = t->texture().handle();
        image.first_mip = levels;
        image.levels = t->texture().levels();
        image.key = t->version();
        image.unstable = t->premultiplied();
        if (i >= _composition_images.size() || image.unstable || image.texture != _composition_images[i].texture ||
            image.handle != _composition_images[i].handle || image.key != _composition_images[i].key ||
            image.levels != _composition_images[i].levels)
            changed = true;
        levels += image.levels;
        max_width = std::max(max_width, u32(t->size().x));
        max_height = std::max(max_height, u32(t->size().y));
        images.push_back(std::move(image));
    }
    if (!changed && _composition_atlas)
        return;
    std::vector<CompositionRect> input;
    for (const auto& image : images) {
        u32 w = u32(image.texture.size().x), h = u32(image.texture.size().y);
        for (u32 mip = 0; mip < image.levels; ++mip) {
            input.push_back({0, 0, w, h, 0});
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
    }
    if (input.empty())
        input.push_back({0, 0, 1, 1, 0});
    std::vector<u32> sorted(input.size());
    std::iota(sorted.begin(), sorted.end(), 0u);
    std::stable_sort(sorted.begin(), sorted.end(), [&](u32 a, u32 b) {
        return input[a].height != input[b].height ? input[a].height > input[b].height : input[a].width > input[b].width;
    });
    std::vector<CompositionRect> best;
    u32 best_width = 1, best_height = 1, best_layers = 1;
    u64 best_cost = ~u64{0};
    const u32 height_limit = std::max(max_height, 2048u);
    for (u32 width = std::bit_ceil(max_width); width <= std::max(std::bit_ceil(max_width), 2048u); width *= 2) {
        auto layout = input;
        u32 x = 0, y = 0, row = 0, layer = 0, used_height = 0;
        for (u32 i : sorted) {
            auto& r = layout[i];
            if (x + r.width > width) {
                y += row;
                row = 0;
                x = 0;
            }
            if (y + r.height > height_limit) {
                ++layer;
                y = 0;
                row = 0;
                x = 0;
            }
            r.x = x;
            r.y = y;
            r.layer = layer;
            x += r.width;
            row = std::max(row, r.height);
            used_height = std::max(used_height, y + r.height);
        }
        const u32 height = std::bit_ceil(used_height);
        const u64 cost = u64(width) * height * (layer + 1);
        if (cost < best_cost || (cost == best_cost && std::max(width, height) < std::max(best_width, best_height))) {
            best_cost = cost;
            best_width = width;
            best_height = height;
            best_layers = layer + 1;
            best = std::move(layout);
        }
    }
    if (best_layers > 2048)
        throw std::runtime_error("particle texture atlas exceeds array layer capacity");
    if (!_composition_atlas || _composition_atlas->size() != Vec2i{int(best_width), int(best_height)} ||
        _composition_atlas->array_layers() != best_layers) {
        _composition_atlas = std::make_shared<GpuTextureBackend>(
            _device->create_target_array(best_width, best_height, best_layers, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET),
            false, ScaleMode::Linear, TextureFormat::Rgba16Float, false, best_layers);
    }
    auto* pipeline = fullscreen(_composition_atlas_shader.handle(), SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    _owner->ensure_frame();
    _owner->flush_to_frame();
    auto* commands = _owner->_frame->command_buffer();
    _owner->retain(Texture{_composition_atlas});
    std::vector<bool> cleared(best_layers, false);
    u32 rect = 0;
    for (const auto& image : images) {
        const auto* t = static_cast<const GpuTextureBackend*>(image.texture.backend().get());
        _owner->retain(image.texture);
        t->mark_material_read(_owner->_frame_serial);
        const SDL_GPUTextureSamplerBinding sample{image.handle, _owner->_sampler_nearest};
        for (u32 mip = 0; mip < image.levels; ++mip, ++rect) {
            const auto& r = best[rect];
            SDL_GPUColorTargetInfo target{};
            target.texture = _composition_atlas->texture().handle();
            target.layer_or_depth_plane = r.layer;
            target.load_op = cleared[r.layer] ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            auto* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
            if (!pass)
                throw std::runtime_error("particle texture atlas pass could not begin");
            cleared[r.layer] = true;
            SDL_GPUViewport viewport{float(r.x), float(r.y), float(r.width), float(r.height), 0, 1};
            SDL_SetGPUViewport(pass, &viewport);
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_BindGPUFragmentSamplers(pass, 0, &sample, 1);
            const f32 flags[]{f32(mip), 0, 0, 0};
            SDL_PushGPUFragmentUniformData(commands, 0, flags, sizeof(flags));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
    }
    if (images.empty()) {
        SDL_GPUColorTargetInfo target{};
        target.texture = _composition_atlas->texture().handle();
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        auto* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
        if (!pass)
            throw std::runtime_error("particle atlas initialization failed");
        SDL_EndGPURenderPass(pass);
    }
    std::vector<CompositionMip> metadata;
    for (const auto& r : best)
        metadata.push_back({{r.x, r.y, r.width, r.height}, {r.layer, 0, 0, 0}});
    composition_buffer(_composition_mips, u32(metadata.size() * sizeof(CompositionMip)),
                       SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    // Uploads precede the frame's commands; these rectangles describe the atlas
    // producer just recorded and are consumed only by subsequent sprite draws.
    _device->upload_storage_buffer(_composition_mips->buffer().handle(), 0,
                                   u32(metadata.size() * sizeof(CompositionMip)), metadata.data(), true);
    _composition_images = std::move(images);
    _composition_rects = std::move(best);
    _composition_decode = decode;
}
} // namespace kin::gpu
