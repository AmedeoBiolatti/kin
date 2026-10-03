#pragma once
// Caches SDL_GPU graphics pipelines keyed by (vertex shader, fragment shader,
// blend mode, color-target format, vertex layout): GpuVertex triangles or
// GpuSpriteInstance quads. Reusable by
// the default 2D path and by custom material/effect shaders (Phase G3+).
#include "gpu_geometry_batch.hpp" // GpuBlendMode

#include <SDL3/SDL.h>

#include <vector>

namespace kin::gpu {

class GpuPipelineCache {
public:
    GpuPipelineCache() = default;
    ~GpuPipelineCache();

    GpuPipelineCache(const GpuPipelineCache&) = delete;
    GpuPipelineCache& operator=(const GpuPipelineCache&) = delete;

    void init(SDL_GPUDevice* device) { _device = device; }

    // Returns a cached pipeline or creates one. Returns nullptr on failure.
    SDL_GPUGraphicsPipeline* get(SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                 GpuBlendMode blend, SDL_GPUTextureFormat target_format,
                                 GpuVertexLayout layout = GpuVertexLayout::Triangles);

    void destroy();
    // Releases the pipelines made with `fragment` (it is being replaced). SDL
    // keeps them until the GPU is done with them.
    void forget(SDL_GPUShader* fragment);

    // Calls fn(vertex, fragment, blend, format, layout) for each pipeline made.
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const Entry& e : _entries) {
            fn(e.vertex, e.fragment, e.blend, e.format, e.layout);
        }
    }

private:
    struct Entry {
        SDL_GPUShader* vertex = nullptr;
        SDL_GPUShader* fragment = nullptr;
        GpuBlendMode blend = GpuBlendMode::Alpha;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        GpuVertexLayout layout = GpuVertexLayout::Triangles;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
    };

    SDL_GPUDevice* _device = nullptr;
    std::vector<Entry> _entries;
};

} // namespace kin::gpu
