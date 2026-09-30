#pragma once
// Reusable triangle-stream batcher for the SDL_GPU 2D path. Generalizes v0's
// quad-only sprite batch: accepts arbitrary triangle lists, each tagged with a
// fragment shader (for material pipelines), texture, scissor, and blend mode, and
// emits them in one render pass. Coordinates are in target pixels; the caller
// supplies the pixels->NDC view (scale/translate) at flush time. Kept independent
// of the IRenderer2DBackend adapter so it can drive non-UI rendering too.
#include <kin/core/types.hpp>
#include <kin/renderer/shader.hpp>

#include "gpu_device.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <span>
#include <vector>

namespace kin::gpu {

class GpuPipelineCache;

enum class GpuBlendMode {
    Alpha,         // straight alpha over
    Premultiplied, // premultiplied alpha over (render-target / blur path)
    Additive,
    Replace,
    Multiply,      // dst * src; the destination's alpha is kept
};

struct GpuVertex {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;
};

// One draw_sprites() quad for sprite_instanced.vert, in target-pixel space.
struct GpuSpriteInstance {
    f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    f32 pivot_x = 0.0f, pivot_y = 0.0f, cos = 1.0f, sin = 0.0f; // rotation about the pivot
    u8 r = 255, g = 255, b = 255, a = 255;
};

// Which vertex input a pipeline reads: GpuVertex triangles, or GpuSpriteInstance
// quads (one per instance).
enum class GpuVertexLayout : u8 { Triangles, SpriteInstances };

// Maps target-pixel coords -> NDC (top-left origin); fed to the vertex shader UBO.
struct GpuView {
    f32 scale[2]{1.0f, 1.0f};
    f32 translate[2]{-1.0f, -1.0f};
};

class GpuGeometryBatch {
public:
    GpuGeometryBatch() = default;

    // Start accumulating for `target`. `clear` (with `do_clear`) becomes the pass
    // load op when flushed.
    void begin(const GpuTexture& target, SDL_FColor clear, bool do_clear);

    // Append triangles (vertex count multiple of 3) in target-pixel space.
    // `fragment` selects the pipeline's fragment shader (nullptr = default 2D);
    // `texture` binds at fragment sampler 0 (nullptr = the default white texture,
    // supplied at flush). `uniform`/`uniform_size` push fragment uniform slot 0
    // (materials); pass {nullptr,0} for none. `extra` binds fragment sampler slots
    // 1, 2, ... for multi-input material shaders (cross-dissolve, bloom combine, data
    // textures); within it a null texture means the white texture and a null sampler
    // the FlushContext default. At most MaxShaderSamplers - 1 entries. Consecutive
    // pushes with identical state coalesce into one draw range.
    void push(std::span<const GpuVertex> tris,
              SDL_GPUShader* fragment,
              SDL_GPUTexture* texture,
              SDL_Rect scissor,
              GpuBlendMode blend,
              const void* uniform = nullptr,
              u32 uniform_size = 0,
              SDL_GPUSampler* sampler = nullptr, // nullptr -> FlushContext default sampler
              std::span<const SDL_GPUTextureSamplerBinding> extra = {});

    // Append quads drawn by instancing (see GpuSpriteInstance) with the default
    // fragment shader. Consecutive pushes with identical state coalesce.
    void push_instances(std::span<const GpuSpriteInstance> instances,
                        SDL_GPUTexture* texture,
                        SDL_Rect scissor,
                        GpuBlendMode blend,
                        SDL_GPUSampler* sampler = nullptr);

    bool empty() const { return _vertices.empty() && _instances.empty(); }

    // Context shared by every range in a flush.
    struct FlushContext {
        SDL_GPUShader* vertex_shader = nullptr;   // shared 2D vertex shader
        SDL_GPUShader* instance_shader = nullptr; // sprite_instanced.vert, for instance ranges
        SDL_GPUShader* default_fragment = nullptr; // used when a range's fragment is null
        SDL_GPUTexture* white_texture = nullptr;   // used when a range's texture is null
        SDL_GPUSampler* sampler = nullptr;
        SDL_GPUTextureFormat target_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        GpuView view{};
    };

    // Emit one render pass into the active target and reset the accumulator.
    void flush(GpuFrame& frame, GpuDevice& device, GpuPipelineCache& cache, const FlushContext& ctx);

    void reset();

private:
    struct Range {
        SDL_GPUShader* fragment = nullptr;
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUSampler* sampler = nullptr; // nullptr -> FlushContext default
        u32 extra_offset = 0; // into _extra_bindings: sampler slots 1..extra_count
        u32 extra_count = 0;
        SDL_Rect scissor{};
        GpuBlendMode blend = GpuBlendMode::Alpha;
        u32 uniform_offset = 0; // into _uniform_bytes; size 0 == none
        u32 uniform_size = 0;
        u32 first_vertex = 0;   // or first instance, for an instanced range
        u32 vertex_count = 0;   // or instance count
        bool instanced = false;
    };

    const GpuTexture* _target = nullptr;
    SDL_FColor _clear{0.0f, 0.0f, 0.0f, 1.0f};
    bool _do_clear = true;
    std::vector<GpuVertex> _vertices;
    std::vector<GpuSpriteInstance> _instances;
    std::vector<Range> _ranges;
    std::vector<u8> _uniform_bytes;
    std::vector<SDL_GPUTextureSamplerBinding> _extra_bindings;
    GpuBuffer _vertex_buffer;
    GpuBuffer _instance_buffer;
};

} // namespace kin::gpu
