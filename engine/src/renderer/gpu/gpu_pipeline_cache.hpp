#pragma once
// Caches SDL_GPU graphics pipelines keyed by what they are made from
// (GpuPipelineKey): shaders, blend mode, colour-target format, vertex layout,
// and the depth, stencil, culling and sample count of the pass they draw in.
// Shared by the 2D path, custom material/effect shaders and 3D views.
#include "gpu_geometry_batch.hpp" // GpuBlendMode

#include <SDL3/SDL.h>

#include <vector>

namespace kin::gpu {

// What a draw does with the depth buffer (3D views). Depth is reverse Z
// (kin/core/math3d.hpp): nearer is greater, so a fragment passes where its
// depth is greater than or equal to what is there.
enum class GpuDepthTest : u8 { None, TestWrite, Test };
// Which triangles are not drawn; counter-clockwise ones (as glTF winds them)
// face the front.
enum class GpuCull : u8 { None, Back, Front };

struct GpuPipelineKey {
    SDL_GPUShader* vertex = nullptr;
    SDL_GPUShader* fragment = nullptr;
    GpuBlendMode blend = GpuBlendMode::Alpha;
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID; // the colour target's
    GpuVertexLayout layout = GpuVertexLayout::Triangles;
    // The depth-stencil target attached to the pass (INVALID: none), which
    // `stencil` and `depth` test or write.
    SDL_GPUTextureFormat depth_stencil = SDL_GPU_TEXTUREFORMAT_INVALID;
    GpuStencilOp stencil = GpuStencilOp::None;
    GpuDepthTest depth = GpuDepthTest::None;
    GpuCull cull = GpuCull::None;
    SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1; // the pass's targets'
    // Triangles' edges only; drawn filled where the device has no line fill mode.
    bool wireframe = false;

    friend bool operator==(const GpuPipelineKey&, const GpuPipelineKey&) = default;
};

class GpuPipelineCache {
public:
    GpuPipelineCache() = default;
    ~GpuPipelineCache();

    GpuPipelineCache(const GpuPipelineCache&) = delete;
    GpuPipelineCache& operator=(const GpuPipelineCache&) = delete;

    void init(SDL_GPUDevice* device) { _device = device; }

    // Returns a cached pipeline or creates one. Returns nullptr on failure.
    SDL_GPUGraphicsPipeline* get(const GpuPipelineKey& key);
    // The 2D path's: no depth test, no culling, one sample. `depth_stencil`:
    // the format of the stencil attached to the pass (INVALID: none), which
    // `stencil` tests or writes.
    SDL_GPUGraphicsPipeline* get(SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                 GpuBlendMode blend, SDL_GPUTextureFormat target_format,
                                 GpuVertexLayout layout = GpuVertexLayout::Triangles,
                                 SDL_GPUTextureFormat depth_stencil = SDL_GPU_TEXTUREFORMAT_INVALID,
                                 GpuStencilOp stencil = GpuStencilOp::None) {
        return get(GpuPipelineKey{.vertex = vertex, .fragment = fragment, .blend = blend, .format = target_format,
                                  .layout = layout, .depth_stencil = depth_stencil, .stencil = stencil});
    }

    void destroy();
    // Releases the pipelines made with `fragment` (it is being replaced). SDL
    // keeps them until the GPU is done with them.
    void forget(SDL_GPUShader* fragment);

    // Calls fn(vertex, fragment, blend, format, layout) for each pipeline made
    // without a depth-stencil target (those with one are made when a clip or
    // a 3D view first needs them).
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const Entry& e : _entries) {
            if (e.key.depth_stencil == SDL_GPU_TEXTUREFORMAT_INVALID) {
                fn(e.key.vertex, e.key.fragment, e.key.blend, e.key.format, e.key.layout);
            }
        }
    }

private:
    struct Entry {
        GpuPipelineKey key;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
    };

    SDL_GPUDevice* _device = nullptr;
    std::vector<Entry> _entries;
};

} // namespace kin::gpu
