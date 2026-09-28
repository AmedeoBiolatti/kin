#include "gpu_geometry_batch.hpp"

#include "gpu_pipeline_cache.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

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
    _ranges.clear();
    _uniform_bytes.clear();
    _extra_bindings.clear();
}

void GpuGeometryBatch::push(std::span<const GpuVertex> tris, SDL_GPUShader* fragment,
                            SDL_GPUTexture* texture, SDL_Rect scissor, GpuBlendMode blend,
                            const void* uniform, u32 uniform_size, SDL_GPUSampler* sampler,
                            std::span<const SDL_GPUTextureSamplerBinding> extra) {
    if (tris.empty()) {
        return;
    }
    assert(extra.size() < MaxShaderSamplers);
    const u32 first = static_cast<u32>(_vertices.size());
    _vertices.insert(_vertices.end(), tris.begin(), tris.end());

    const bool can_coalesce = uniform_size == 0 && !_ranges.empty() &&
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
        _ranges.back().vertex_count += static_cast<u32>(tris.size());
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
    range.vertex_count = static_cast<u32>(tris.size());
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

    if (_vertices.empty() || _ranges.empty()) {
        // Nothing drawn — still honor the clear so the target is initialized.
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.command_buffer(), &color_target, 1, nullptr);
        SDL_EndGPURenderPass(pass);
        reset();
        return;
    }

    const u32 vertex_bytes = static_cast<u32>(_vertices.size() * sizeof(GpuVertex));
    if (!_vertex_buffer || _vertex_buffer.size() < vertex_bytes) {
        const u32 capacity = vertex_bytes < 4096u ? 4096u : vertex_bytes;
        _vertex_buffer = device.create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, nullptr, capacity);
    }
    device.upload_buffer(frame, _vertex_buffer, _vertices.data(), vertex_bytes);

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.command_buffer(), &color_target, 1, nullptr);

    SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(_target->width()),
                             static_cast<f32>(_target->height()), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);

    struct VertexUniforms {
        f32 scale[2];
        f32 translate[2];
    } uniforms{{ctx.view.scale[0], ctx.view.scale[1]}, {ctx.view.translate[0], ctx.view.translate[1]}};
    SDL_PushGPUVertexUniformData(frame.command_buffer(), 0, &uniforms, sizeof(uniforms));

    SDL_GPUBufferBinding vertex_binding{};
    vertex_binding.buffer = _vertex_buffer.handle();
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);

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
        SDL_GPUGraphicsPipeline* pipeline = cache.get(ctx.vertex_shader, fragment, range.blend, ctx.target_format);
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

        SDL_DrawGPUPrimitives(pass, range.vertex_count, 1, range.first_vertex, 0);
    }

    SDL_EndGPURenderPass(pass);
    reset();
}

void GpuGeometryBatch::reset() {
    _target = nullptr;
    _vertices.clear();
    _ranges.clear();
    _uniform_bytes.clear();
    _extra_bindings.clear();
}

} // namespace kin::gpu
