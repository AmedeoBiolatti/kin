#include "gpu_geometry_batch.hpp"

#include "gpu_pipeline_cache.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>

namespace kin::gpu {

namespace {

bool same_scissor(const SDL_Rect& a, const SDL_Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool same_binding(const SDL_GPUTextureSamplerBinding& a, const SDL_GPUTextureSamplerBinding& b) {
    return a.texture == b.texture && a.sampler == b.sampler;
}

} // namespace

void GpuGeometryBatch::begin(const GpuTexture& target, SDL_FColor clear, bool do_clear) {
    _target = &target;
    _clear = clear;
    _do_clear = do_clear;
    _vertices.clear();
    _instances.clear();
    _shader_vertices.clear();
    _ranges.clear();
    _uniform_bytes.clear();
    _extra_bindings.clear();
}

void GpuGeometryBatch::push_instances(std::span<const GpuSpriteInstance> instances, SDL_GPUTexture* texture,
                                      SDL_Rect scissor, GpuBlendMode blend, SDL_GPUSampler* sampler) {
    if (instances.empty()) {
        return;
    }
    const u32 first = static_cast<u32>(_instances.size());
    _instances.insert(_instances.end(), instances.begin(), instances.end());
    if (!_ranges.empty()) {
        Range& last = _ranges.back();
        if (last.layout == GpuVertexLayout::SpriteInstances && last.texture == texture && last.sampler == sampler && last.blend == blend &&
            same_scissor(last.scissor, scissor)) {
            last.vertex_count += static_cast<u32>(instances.size());
            return;
        }
    }
    Range range;
    range.texture = texture;
    range.sampler = sampler;
    range.scissor = scissor;
    range.blend = blend;
    range.first_vertex = first;
    range.vertex_count = static_cast<u32>(instances.size());
    range.layout = GpuVertexLayout::SpriteInstances;
    _ranges.push_back(range);
}

void GpuGeometryBatch::push(std::span<const GpuVertex> tris, SDL_GPUShader* fragment,
                            SDL_GPUTexture* texture, SDL_Rect scissor, GpuBlendMode blend,
                            const void* uniform, u32 uniform_size, SDL_GPUSampler* sampler,
                            std::span<const SDL_GPUTextureSamplerBinding> extra) {
    if (tris.empty()) {
        return;
    }
    const u32 first = static_cast<u32>(_vertices.size());
    _vertices.insert(_vertices.end(), tris.begin(), tris.end());
    add_range(GpuVertexLayout::Triangles, first, static_cast<u32>(tris.size()), fragment, texture, scissor, blend,
              uniform, uniform_size, sampler, extra);
}

void GpuGeometryBatch::push_shader_vertices(std::span<const GpuShaderVertex> tris, SDL_GPUShader* fragment,
                                            SDL_GPUTexture* texture, SDL_Rect scissor, GpuBlendMode blend,
                                            const void* uniform, u32 uniform_size, SDL_GPUSampler* sampler,
                                            std::span<const SDL_GPUTextureSamplerBinding> extra) {
    if (tris.empty()) {
        return;
    }
    const u32 first = static_cast<u32>(_shader_vertices.size());
    _shader_vertices.insert(_shader_vertices.end(), tris.begin(), tris.end());
    add_range(GpuVertexLayout::ShaderVertices, first, static_cast<u32>(tris.size()), fragment, texture, scissor,
              blend, uniform, uniform_size, sampler, extra);
}

void GpuGeometryBatch::add_range(GpuVertexLayout layout, u32 first, u32 count, SDL_GPUShader* fragment,
                                 SDL_GPUTexture* texture, SDL_Rect scissor, GpuBlendMode blend, const void* uniform,
                                 u32 uniform_size, SDL_GPUSampler* sampler,
                                 std::span<const SDL_GPUTextureSamplerBinding> extra) {
    assert(extra.size() < MaxShaderSamplers);
    const bool can_coalesce = uniform_size == 0 && !_ranges.empty() && _ranges.back().layout == layout &&
                              _ranges.back().fragment == fragment &&
                              _ranges.back().texture == texture &&
                              _ranges.back().sampler == sampler &&
                              _ranges.back().extra_count == extra.size() &&
                              std::equal(extra.begin(), extra.end(),
                                         _extra_bindings.begin() + _ranges.back().extra_offset,
                                         same_binding) &&
                              _ranges.back().blend == blend &&
                              _ranges.back().uniform_size == 0 &&
                              same_scissor(_ranges.back().scissor, scissor);
    if (can_coalesce) {
        _ranges.back().vertex_count += count;
        return;
    }

    Range range{};
    range.fragment = fragment;
    range.texture = texture;
    range.sampler = sampler;
    range.extra_offset = static_cast<u32>(_extra_bindings.size());
    range.extra_count = static_cast<u32>(extra.size());
    _extra_bindings.insert(_extra_bindings.end(), extra.begin(), extra.end());
    range.scissor = scissor;
    range.blend = blend;
    range.first_vertex = first;
    range.vertex_count = count;
    range.layout = layout;
    if (uniform && uniform_size > 0) {
        range.uniform_offset = static_cast<u32>(_uniform_bytes.size());
        range.uniform_size = uniform_size;
        const auto* bytes = static_cast<const u8*>(uniform);
        _uniform_bytes.insert(_uniform_bytes.end(), bytes, bytes + uniform_size);
    }
    _ranges.push_back(range);
}

void GpuGeometryBatch::flush(GpuFrame& frame, GpuDevice& device, GpuPipelineCache& cache,
                             const FlushContext& ctx) {
    if (!_target) {
        return;
    }

    SDL_GPUColorTargetInfo color_target{};
    color_target.texture = _target->handle();
    color_target.load_op = _do_clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    color_target.clear_color = _clear;

    if (empty() || _ranges.empty()) {
        // Nothing drawn — still honor the clear so the target is initialized.
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.command_buffer(), &color_target, 1, nullptr);
        SDL_EndGPURenderPass(pass);
        reset();
        return;
    }

    const auto upload = [&](GpuBuffer& buffer, const void* data, u32 bytes) {
        if (bytes == 0) {
            return;
        }
        if (!buffer || buffer.size() < bytes) {
            const u32 capacity = bytes < 4096u ? 4096u : bytes;
            buffer = device.create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, nullptr, capacity);
        }
        device.upload_buffer(frame, buffer, data, bytes);
    };
    upload(_vertex_buffer, _vertices.data(), static_cast<u32>(_vertices.size() * sizeof(GpuVertex)));
    upload(_instance_buffer, _instances.data(), static_cast<u32>(_instances.size() * sizeof(GpuSpriteInstance)));
    upload(_shader_vertex_buffer, _shader_vertices.data(),
           static_cast<u32>(_shader_vertices.size() * sizeof(GpuShaderVertex)));

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.command_buffer(), &color_target, 1, nullptr);

    SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(_target->width()),
                             static_cast<f32>(_target->height()), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);

    struct VertexUniforms {
        f32 scale[2];
        f32 translate[2];
    } uniforms{{ctx.view.scale[0], ctx.view.scale[1]}, {ctx.view.translate[0], ctx.view.translate[1]}};
    SDL_PushGPUVertexUniformData(frame.command_buffer(), 0, &uniforms, sizeof(uniforms));

    // Slot 0 holds the triangle vertices, the shader vertices or, for an
    // instanced range, its instances (bound at the range's offset, so every draw
    // starts at instance 0). Which array is bound now:
    std::optional<GpuVertexLayout> bound_vertices;

    const int target_w = static_cast<int>(_target->width());
    const int target_h = static_cast<int>(_target->height());

    // Cache the last-applied render-pass state so consecutive ranges that share it
    // (e.g. many sprites differing only by texture all use the same pipeline + scissor)
    // skip redundant binds. Render-pass state persists until rebound, so eliding an
    // identical re-bind is pixel-neutral.
    SDL_GPUGraphicsPipeline* bound_pipeline = nullptr;
    SDL_Rect bound_scissor{};
    bool have_scissor = false;
    SDL_GPUTextureSamplerBinding bound_bindings[MaxShaderSamplers]{};
    u32 bound_binding_count = 0;

    for (const Range& range : _ranges) {
        SDL_GPUShader* fragment = range.fragment ? range.fragment : ctx.default_fragment;
        SDL_GPUShader* vertex = range.layout == GpuVertexLayout::SpriteInstances ? ctx.instance_shader
                              : range.layout == GpuVertexLayout::ShaderVertices  ? ctx.shader_vertex_shader
                                                                                 : ctx.vertex_shader;
        SDL_GPUGraphicsPipeline* pipeline = cache.get(vertex, fragment, range.blend, ctx.target_format, range.layout);
        if (!pipeline) {
            continue;
        }
        bool pipeline_changed = false;
        if (pipeline != bound_pipeline) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound_pipeline = pipeline;
            pipeline_changed = true;
        }

        const int x0 = SDL_clamp(range.scissor.x, 0, target_w);
        const int y0 = SDL_clamp(range.scissor.y, 0, target_h);
        const int x1 = SDL_clamp(range.scissor.x + range.scissor.w, 0, target_w);
        const int y1 = SDL_clamp(range.scissor.y + range.scissor.h, 0, target_h);
        SDL_Rect scissor{x0, y0, x1 - x0 > 0 ? x1 - x0 : 0, y1 - y0 > 0 ? y1 - y0 : 0};
        if (!have_scissor || !same_scissor(scissor, bound_scissor)) {
            SDL_SetGPUScissor(pass, &scissor);
            bound_scissor = scissor;
            have_scissor = true;
        }

        if (range.uniform_size > 0) {
            SDL_PushGPUFragmentUniformData(frame.command_buffer(), 0,
                                           _uniform_bytes.data() + range.uniform_offset,
                                           range.uniform_size);
        }

        SDL_GPUTextureSamplerBinding tex_bindings[MaxShaderSamplers]{};
        tex_bindings[0].texture = range.texture ? range.texture : ctx.white_texture;
        tex_bindings[0].sampler = range.sampler ? range.sampler : ctx.sampler;
        const u32 binding_count = 1 + range.extra_count;
        for (u32 i = 0; i < range.extra_count; ++i) {
            const SDL_GPUTextureSamplerBinding& extra = _extra_bindings[range.extra_offset + i];
            tex_bindings[1 + i].texture = extra.texture ? extra.texture : ctx.white_texture;
            tex_bindings[1 + i].sampler = extra.sampler ? extra.sampler : ctx.sampler;
        }
        // Rebind samplers whenever they differ — or whenever the pipeline was just
        // rebound, so we never rely on descriptor bindings persisting across a pipeline
        // change (keeps the elision provably pixel-neutral).
        if (pipeline_changed || binding_count != bound_binding_count ||
            !std::equal(tex_bindings, tex_bindings + binding_count, bound_bindings, same_binding)) {
            SDL_BindGPUFragmentSamplers(pass, 0, tex_bindings, binding_count);
            std::copy(tex_bindings, tex_bindings + binding_count, bound_bindings);
            bound_binding_count = binding_count;
        }

        if (range.layout == GpuVertexLayout::SpriteInstances) {
            SDL_GPUBufferBinding binding{};
            binding.buffer = _instance_buffer.handle();
            binding.offset = range.first_vertex * static_cast<u32>(sizeof(GpuSpriteInstance));
            SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
            bound_vertices.reset();
            SDL_DrawGPUPrimitives(pass, 6, range.vertex_count, 0, 0);
        } else {
            if (bound_vertices != range.layout || pipeline_changed) {
                SDL_GPUBufferBinding binding{};
                binding.buffer = range.layout == GpuVertexLayout::ShaderVertices ? _shader_vertex_buffer.handle()
                                                                                 : _vertex_buffer.handle();
                SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
                bound_vertices = range.layout;
            }
            SDL_DrawGPUPrimitives(pass, range.vertex_count, 1, range.first_vertex, 0);
        }
    }

    SDL_EndGPURenderPass(pass);
    reset();
}

void GpuGeometryBatch::reset() {
    _target = nullptr;
    _vertices.clear();
    _instances.clear();
    _shader_vertices.clear();
    _ranges.clear();
    _uniform_bytes.clear();
    _extra_bindings.clear();
}

} // namespace kin::gpu
