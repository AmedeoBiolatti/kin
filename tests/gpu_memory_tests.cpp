#include "../engine/src/renderer/gpu/gpu_memory.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
using namespace kin;
int main() {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = 7;
    info.height = 5;
    info.layer_count_or_depth = 3;
    info.num_levels = 3;
    info.format = SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    assert(gpu::texture_payload_bytes(info) == (32 + 8 + 8) * 3);
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    info.width = 64;
    info.height = 32;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_4;
    assert(gpu::texture_payload_bytes(info) == 64 * 32 * 3 * 8 * 4);
    info.type = SDL_GPU_TEXTURETYPE_3D;
    info.width = 8;
    info.height = 4;
    info.layer_count_or_depth = 4;
    info.num_levels = 3;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    assert(gpu::texture_payload_bytes(info) == (8 * 4 * 4 + 4 * 2 * 2 + 2 * 1 * 1) * 8);
    App app;
    auto& window = app.create_window({.title = "GPU memory ledger", .width = 64, .height = 64, .hidden = true});
    DataBuffer held_buffer;
    Texture held_texture;
    {
        Renderer2D renderer{make_render_backend(window, false, true)};
        if (!renderer.backend_stats().gpu_memory.available)
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        auto warm = renderer.create_data_buffer(16);
        renderer.clear(colors::black);
        renderer.present();
        auto base = renderer.backend_stats().gpu_memory;
        {
            auto buffer = renderer.create_data_buffer(4096);
            auto target = renderer.create_render_target({128, 64});
            auto current = renderer.backend_stats().gpu_memory;
            assert(current.buffer_bytes == base.buffer_bytes + 4096);
            assert(current.texture_bytes == base.texture_bytes + 128 * 64 * 4);
            assert(current.live_handles == base.live_handles + 2);
        }
        auto released = renderer.backend_stats().gpu_memory;
        assert(released.buffer_bytes == base.buffer_bytes && released.texture_bytes == base.texture_bytes);
        assert(released.live_handles == base.live_handles);
        // Same sampled image descriptor reuses the idle texture handle.
        {
            auto sampled = renderer.create_texture({64, 64}, TextureFormat::Rgba8);
            assert(sampled);
        }
        auto pooled = renderer.backend_stats().gpu_memory;
        assert(pooled.pooled_texture_bytes >= 64 * 64 * 4);
        for (int i = 0; i < 100; ++i) {
            auto sampled = renderer.create_texture({64, 64}, TextureFormat::Rgba8);
            assert(sampled);
        }
        auto reused = renderer.backend_stats().gpu_memory;
        assert(reused.allocations == pooled.allocations);
        assert(reused.texture_bytes == pooled.texture_bytes && reused.live_handles == pooled.live_handles);
        // Repeated target resizing must return to the same descriptor plateau.
        for (int i = 0; i < 100; ++i) {
            auto target = renderer.create_render_target({i % 2 ? 128 : 64, 64});
            assert(target);
        }
        auto plateau = renderer.backend_stats().gpu_memory;
        assert(plateau.texture_bytes == pooled.texture_bytes && plateau.buffer_bytes == pooled.buffer_bytes);
        assert(plateau.peak_payload_bytes >= plateau.payload_bytes());
        held_buffer = renderer.create_data_buffer(32);
        held_texture = renderer.create_texture({16, 16}, TextureFormat::Rgba8);
    }
    // Buffers and textures can both outlive the renderer without releasing into a dead device.
    held_buffer = {};
    held_texture = {};
    std::cout << "GPU memory: descriptor sizes, pool reuse, 100 resize cycles, plateau and late handle destruction\n";
}
