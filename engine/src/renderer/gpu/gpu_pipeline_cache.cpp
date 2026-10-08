#include "gpu_pipeline_cache.hpp"

#include <kin/platform/log.hpp>

#include <cstddef>
#include <string>

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
    case GpuBlendMode::Max:
    case GpuBlendMode::Min: {
        // The factors are ignored by MIN and MAX (Vulkan, D3D12, Metal alike).
        const SDL_GPUBlendOp op = mode == GpuBlendMode::Max ? SDL_GPU_BLENDOP_MAX : SDL_GPU_BLENDOP_MIN;
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.color_blend_op = op;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.alpha_blend_op = op;
        break;
    }
    case GpuBlendMode::PremultipliedAdditive:
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
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

void GpuPipelineCache::forget(SDL_GPUShader* fragment) {
    std::erase_if(_entries, [&](const Entry& e) {
        if (e.key.fragment != fragment) {
            return false;
        }
        if (_device && e.pipeline) {
            SDL_ReleaseGPUGraphicsPipeline(_device, e.pipeline);
        }
        return true;
    });
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

SDL_GPUGraphicsPipeline* GpuPipelineCache::get(const GpuPipelineKey& key) {
    for (const Entry& e : _entries) {
        if (e.key == key) {
            return e.pipeline;
        }
    }
    if (!_device || !key.vertex || !key.fragment) {
        return nullptr;
    }
    const GpuVertexLayout layout = key.layout;
    const GpuStencilOp stencil = key.stencil;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    SDL_GPUVertexAttribute attrs[5]{}; // at most five, whichever layout
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
    } else if (layout == GpuVertexLayout::SdfInstances) {
        vb.pitch = sizeof(GpuSdfInstance);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuSdfInstance, a));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuSdfInstance, tx));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuSdfInstance, radius));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuSdfInstance, fill_r));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuSdfInstance, stroke_r));
    } else if (layout == GpuVertexLayout::MeshVertices) {
        vb.pitch = sizeof(GpuMeshVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuMeshVertex, x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuMeshVertex, nx));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuMeshVertex, u));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuMeshVertex, r));
    } else if (layout == GpuVertexLayout::ShapeVertices) {
        vb.pitch = sizeof(GpuShapeVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuShapeVertex, x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuShapeVertex, r));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(GpuShapeVertex, edge));
    } else if (layout == GpuVertexLayout::ShaderVertices) {
        vb.pitch = sizeof(GpuShaderVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuShaderVertex, x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuShaderVertex, u));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuShaderVertex, r));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuShaderVertex, custom));
    } else if (layout != GpuVertexLayout::None) {
        vb.pitch = sizeof(GpuVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, x));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, u));
        attribute(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuVertex, r));
    }

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.num_vertex_buffers = layout == GpuVertexLayout::None ? 0 : 1;
    vertex_input.vertex_buffer_descriptions = &vb;
    vertex_input.num_vertex_attributes = attr_count;
    vertex_input.vertex_attributes = attrs;

    SDL_GPUColorTargetBlendState blend_state{};
    set_blend(blend_state, key.blend);
    const bool stencil_write = stencil == GpuStencilOp::Increment || stencil == GpuStencilOp::Decrement;
    if (stencil_write) {
        blend_state.color_write_mask = 0; // only the stencil changes
        blend_state.enable_color_write_mask = true;
    }

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = key.format;
    color_target.blend_state = blend_state;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.num_color_targets = key.depth_only ? 0 : 1;
    target_info.color_target_descriptions = &color_target;
    target_info.has_depth_stencil_target = key.depth_stencil != SDL_GPU_TEXTUREFORMAT_INVALID;
    target_info.depth_stencil_format = key.depth_stencil;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = key.vertex;
    info.fragment_shader = key.fragment;
    info.vertex_input_state = vertex_input;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = key.wireframe ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = key.cull == GpuCull::Back    ? SDL_GPU_CULLMODE_BACK
                                      : key.cull == GpuCull::Front ? SDL_GPU_CULLMODE_FRONT
                                                                   : SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    if (key.depth_bias) {
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = 2.0f;
        info.rasterizer_state.depth_bias_slope_factor = 1.0f;
    }
    info.multisample_state.sample_count = key.samples;
    info.target_info = target_info;
    if (key.depth != GpuDepthTest::None && key.depth_stencil != SDL_GPU_TEXTUREFORMAT_INVALID) {
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = key.depth == GpuDepthTest::TestWrite;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL; // reverse Z
    }
    if (stencil != GpuStencilOp::None) {
        // Where the stencil holds the reference: draw, or count one clip in or out there.
        SDL_GPUStencilOpState face{};
        face.fail_op = SDL_GPU_STENCILOP_KEEP;
        face.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
        face.pass_op = stencil == GpuStencilOp::Increment   ? SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP
                       : stencil == GpuStencilOp::Decrement ? SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP
                                                            : SDL_GPU_STENCILOP_KEEP;
        face.compare_op = SDL_GPU_COMPAREOP_EQUAL;
        info.depth_stencil_state.enable_stencil_test = true;
        info.depth_stencil_state.front_stencil_state = face;
        info.depth_stencil_state.back_stencil_state = face;
        info.depth_stencil_state.compare_mask = 0xFF;
        info.depth_stencil_state.write_mask = stencil_write ? 0xFF : 0x00;
    }

    const u64 start_ns = SDL_GetTicksNS();
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(_device, &info);
    if (!pipeline) {
        return nullptr;
    }
    // Made on first use: worth seeing when it lands mid-game (tens of ms on a
    // cold driver cache).
    KIN_LOG_DEBUG_F("render", "pipeline created",
                    (LogFields{{.name = "ms", .value = std::to_string(static_cast<f64>(SDL_GetTicksNS() - start_ns) / 1e6)},
                               {.name = "pipelines", .value = std::to_string(_entries.size() + 1)}}));
    _entries.push_back(Entry{key, pipeline});
    return pipeline;
}

} // namespace kin::gpu
