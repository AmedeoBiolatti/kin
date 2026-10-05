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

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <cmath>
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
    Max,           // max(dst, src) per channel
    Min,           // min(dst, src) per channel
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

// One draw_shader_geometry() vertex for shader_geometry.vert: a GpuVertex plus
// four free floats for the material fragment shader.
struct GpuShaderVertex {
    f32 x = 0.0f, y = 0.0f, u = 0.0f, v = 0.0f;
    u8 r = 255, g = 255, b = 255, a = 255;
    f32 custom[4]{};
};

// Which vertex input a pipeline reads: GpuVertex triangles, GpuSpriteInstance
// quads (one per instance), or GpuShaderVertex triangles.
// Shapes' vertices (Renderer2D::draw_shape), drawn indexed: 16 bytes.
struct GpuShapeVertex {
    f32 x = 0.0f, y = 0.0f;
    u8 r = 255, g = 255, b = 255, a = 255;
    f32 edge = 0.0f; // how far past the outline, negated (shape.frag)
};

// A shape primitive (circle, ellipse, rounded rectangle) drawn whole: one
// instance, a quad round it (sdf_shape.vert / .frag).
struct GpuSdfInstance {
    f32 a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;  // its own units to draw coordinates
    f32 tx = 0.0f, ty = 0.0f, half_w = 0.0f, half_h = 0.0f;
    f32 radius = 0.0f, half_stroke = 0.0f, margin = 0.0f, kind = 0.0f; // kind: 0 rounded, 1 sharp, 2 ellipse
    u8 fill_r = 0, fill_g = 0, fill_b = 0, fill_a = 0;
    u8 stroke_r = 0, stroke_g = 0, stroke_b = 0, stroke_a = 0;
};

enum class GpuVertexLayout : u8 { Triangles, SpriteInstances, ShaderVertices, ShapeVertices, SdfInstances };

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
              std::span<const SDL_GPUTextureSamplerBinding> extra = {},
              std::span<SDL_GPUBuffer* const> storage = {});

    // As push(), for quads: four corners each (top-left, top-right,
    // bottom-right, bottom-left, as the quad is meant to be split), drawn
    // through a static index buffer instead of as six vertices.
    void push_quads(std::span<const GpuVertex> corners,
                    SDL_GPUShader* fragment,
                    SDL_GPUTexture* texture,
                    SDL_Rect scissor,
                    GpuBlendMode blend,
                    const void* uniform = nullptr,
                    u32 uniform_size = 0,
                    SDL_GPUSampler* sampler = nullptr,
                    std::span<const SDL_GPUTextureSamplerBinding> extra = {},
                    std::span<SDL_GPUBuffer* const> storage = {});

    // One quad with the default fragment shader and no uniforms: the common
    // case, appended straight to the last range when its state matches.
    void push_quad(const std::array<GpuVertex, 4>& corners, SDL_GPUTexture* texture, SDL_Rect scissor,
                   GpuBlendMode blend, SDL_GPUSampler* sampler) {
        if (!_ranges.empty()) {
            const Range& last = _ranges.back();
            if (last.quads && last.texture == texture && last.sampler == sampler && last.blend == blend &&
                last.fragment == nullptr && last.uniform_size == 0 && last.extra_count == 0 && last.storage_count == 0 &&
                last.layout == GpuVertexLayout::Triangles && last.scissor.x == scissor.x &&
                last.scissor.y == scissor.y && last.scissor.w == scissor.w && last.scissor.h == scissor.h) {
                if (_vertices.capacity() - _vertices.size() < 4) {
                    _vertices.reserve(std::max<std::size_t>(_vertices.capacity() * 2, 4096));
                }
                for (const GpuVertex& v : corners) {
                    _vertices.push_back(v); // in reserved room: plain stores
                }
                _ranges.back().vertex_count += 4;
                _area += quad_area(corners.data());
                bound(std::span<const GpuVertex>{corners});
                return;
            }
        }
        push_quads(corners, nullptr, texture, scissor, blend, nullptr, 0, sampler);
    }

    // As push(), with GpuShaderVertex triangles (draw_shader_geometry), drawn by
    // shader_geometry.vert.
    void push_shader_vertices(std::span<const GpuShaderVertex> tris,
                              SDL_GPUShader* fragment,
                              SDL_GPUTexture* texture,
                              SDL_Rect scissor,
                              GpuBlendMode blend,
                              const void* uniform,
                              u32 uniform_size,
                              SDL_GPUSampler* sampler,
                              std::span<const SDL_GPUTextureSamplerBinding> extra,
                              std::span<SDL_GPUBuffer* const> storage = {});

    // Append quads drawn by instancing (see GpuSpriteInstance) with the default
    // fragment shader. Consecutive pushes with identical state coalesce.
    void push_instances(std::span<const GpuSpriteInstance> instances,
                        SDL_GPUTexture* texture,
                        SDL_Rect scissor,
                        GpuBlendMode blend,
                        SDL_GPUSampler* sampler = nullptr);

    // Indexed shape triangles (shape.vert): `indices` count from `index_base`
    // (vertices[i - index_base]). Consecutive pushes with identical state coalesce.
    void push_shapes(std::span<const GpuShapeVertex> vertices, std::span<const u32> indices, u32 index_base,
                     SDL_GPUShader* fragment, SDL_Rect scissor, GpuBlendMode blend);
    // Shape primitives, one instance each (sdf_shape.vert). Coalesce likewise.
    void push_sdf(std::span<const GpuSdfInstance> instances, SDL_GPUShader* fragment, SDL_Rect scissor,
                  GpuBlendMode blend);

    bool empty() const {
        return _vertices.empty() && _instances.empty() && _shader_vertices.empty() && _shape_indices.empty() &&
               _sdf_instances.empty();
    }

    // The pixels everything flushed since the last call covered, in the
    // targets' own pixels (overlaps count each time, clipping is not taken
    // off): what the GPU shaded, for an overdraw figure.
    f64 take_pixels() { return std::exchange(_pixels, 0.0); }
    f64 pixels() const { return _pixels; } // so far, without taking them

    // The box (in draw coordinates) of what is pushed between these, nested.
    void begin_bounds() { _bounds.push_back(Box{}); }
    std::optional<Rectf> end_bounds() {
        const Box box = _bounds.back();
        _bounds.pop_back();
        if (box.x0 > box.x1) {
            return std::nullopt;
        }
        return Rectf{box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0};
    }

    // Context shared by every range in a flush.
    struct FlushContext {
        SDL_GPUShader* vertex_shader = nullptr;   // shared 2D vertex shader
        SDL_GPUShader* instance_shader = nullptr; // sprite_instanced.vert, for instance ranges
        SDL_GPUShader* shader_vertex_shader = nullptr; // shader_geometry.vert, for GpuShaderVertex ranges
        SDL_GPUShader* shape_vertex_shader = nullptr;  // shape.vert, for GpuShapeVertex ranges
        SDL_GPUShader* sdf_vertex_shader = nullptr;    // sdf_shape.vert, for GpuSdfInstance ranges
        SDL_GPUShader* default_fragment = nullptr; // used when a range's fragment is null
        SDL_GPUTexture* white_texture = nullptr;   // used when a range's texture is null
        SDL_GPUSampler* sampler = nullptr;
        SDL_GPUTextureFormat target_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        GpuView view{};
        // Set: every range draws with this shader, additively, its own inputs
        // left unbound (the overdraw view).
        SDL_GPUShader* override_fragment = nullptr;
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
        u32 storage_offset = 0; // into _storage_buffers: fragment storage buffer slots 0..
        u32 storage_count = 0;
        SDL_Rect scissor{};
        GpuBlendMode blend = GpuBlendMode::Alpha;
        u32 uniform_offset = 0; // into _uniform_bytes; size 0 == none
        u32 uniform_size = 0;
        u32 first_vertex = 0;   // or first instance, for an instanced range; first index, for shapes
        u32 vertex_count = 0;   // or instance count; index count, for shapes
        GpuVertexLayout layout = GpuVertexLayout::Triangles; // which array it indexes
        bool quads = false; // its vertices are quads' corners, drawn indexed
    };

    // Adds `count` vertices of `layout` at `first` to the last range when its
    // state matches, else as a new range.
    void add_range(GpuVertexLayout layout, bool quads, u32 first, u32 count, SDL_GPUShader* fragment,
                   SDL_GPUTexture* texture,
                   SDL_Rect scissor, GpuBlendMode blend, const void* uniform, u32 uniform_size,
                   SDL_GPUSampler* sampler, std::span<const SDL_GPUTextureSamplerBinding> extra,
                   std::span<SDL_GPUBuffer* const> storage);

    const GpuTexture* _target = nullptr;
    SDL_FColor _clear{0.0f, 0.0f, 0.0f, 1.0f};
    bool _do_clear = true;
    std::vector<GpuVertex> _vertices;
    std::vector<GpuSpriteInstance> _instances;
    std::vector<GpuShaderVertex> _shader_vertices;
    std::vector<GpuShapeVertex> _shape_vertices;
    std::vector<u32> _shape_indices; // into _shape_vertices
    std::vector<GpuSdfInstance> _sdf_instances;
    std::vector<Range> _ranges;
    std::vector<u8> _uniform_bytes;
    std::vector<SDL_GPUTextureSamplerBinding> _extra_bindings;
    std::vector<SDL_GPUBuffer*> _storage_buffers;
    GpuBuffer _vertex_buffer;
    GpuBuffer _instance_buffer;
    GpuBuffer _shader_vertex_buffer;
    GpuBuffer _shape_vertex_buffer;
    GpuBuffer _shape_index_buffer;
    GpuBuffer _sdf_buffer;
    GpuBuffer _quad_indices; // 0 1 2 0 2 3, 4 5 6 4 6 7, ...: made once
    f64 _area = 0.0;         // covered by what is pushed, in draw coordinates
    struct Box {
        f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        void add(f32 x, f32 y) {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    };
    std::vector<Box> _bounds; // open begin_bounds(), innermost last
    template<typename V>
    void bound(std::span<const V> vertices) {
        if (!_bounds.empty()) {
            for (const V& v : vertices) {
                _bounds.back().add(v.x, v.y);
            }
        }
    }
    f64 _pixels = 0.0;       // covered by what was flushed, in target pixels

    template<typename V>
    static f64 triangle_area(const V& a, const V& b, const V& c) {
        return 0.5 * std::abs(static_cast<f64>(b.x - a.x) * (c.y - a.y) - static_cast<f64>(c.x - a.x) * (b.y - a.y));
    }
    static f64 quad_area(const GpuVertex* q) { return triangle_area(q[0], q[1], q[2]) + triangle_area(q[0], q[2], q[3]); }
};

} // namespace kin::gpu
