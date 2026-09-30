#include "gpu_pipeline_cache.hpp"

#include <cstddef>

namespace kin::gpu {

namespace {

void set_blend(SDL_GPUColorTargetBlendState& blend, GpuBlendMode mode) {
    blend.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                             SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    switch (mode) {
    case GpuBlendMode::Replace:
        blend.enable_blend = false;
        break;
    case GpuBlendMode::Additive:
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    case GpuBlendMode::Multiply:
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    case GpuBlendMode::Premultiplied:
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    case GpuBlendMode::Alpha:
    default:
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        break;
    }
}

} // namespace

GpuPipelineCache::~GpuPipelineCache() {
    destroy();
}

void GpuPipelineCache::destroy() {
    if (_device) {
        for (Entry& e : _entries) {
            if (e.pipeline) {
                SDL_ReleaseGPUGraphicsPipeline(_device, e.pipeline);
            }
        }
    }
    _entries.clear();
}

SDL_GPUGraphicsPipeline* GpuPipelineCache::get(SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                               GpuBlendMode blend, SDL_GPUTextureFormat target_format,
                                               GpuVertexLayout layout) {
    for (const Entry& e : _entries) {
        if (e.vertex == vertex && e.fragment == fragment && e.blend == blend && e.format == target_format &&
            e.layout == layout) {
            return e.pipeline;
        }
    }
    if (!_device || !vertex || !fragment) {
        return nullptr;
    }

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    SDL_GPUVertexAttribute attrs[4]{};
    u32 attr_count = 0;
    const auto attribute = [&](SDL_GPUVertexElementFormat format, u32 offset) {
        attrs[attr_count].location = attr_count;
        attrs[attr_count].buffer_slot = 0;
        attrs[attr_count].format = format;
        attrs[attr_count].offset = offset;
        ++attr_count;
    };
    if (layout == GpuVertexLayout::SpriteInstances) {
        vb.pitch = sizeof(GpuSpriteInstance);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuSpriteInstance, x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuSpriteInstance, u0));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuSpriteInstance, pivot_x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuSpriteInstance, r));
    } else {
        vb.pitch = sizeof(GpuVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, u));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuVertex, r));
    }

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_buffer_descriptions = &vb;
    vertex_input.num_vertex_attributes = attr_count;
    vertex_input.vertex_attributes = attrs;

    SDL_GPUColorTargetBlendState blend_state{};
    set_blend(blend_state, blend);

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = target_format;
    color_target.blend_state = blend_state;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.num_color_targets = 1;
    target_info.color_target_descriptions = &color_target;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state = vertex_input;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info = target_info;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(_device, &info);
    if (!pipeline) {
        return nullptr;
    }
    _entries.push_back(Entry{vertex, fragment, blend, target_format, layout, pipeline});
    return pipeline;
}

} // namespace kin::gpu
