#pragma once

// The seam between Renderer3D and a renderer backend that can draw 3D views
// (IRenderer2DBackend::renderer_3d(); SDL_GPU only). Experimental: see
// docs/3d_plan.md.

#include <kin/core/types.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/mesh_lod.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/shadow_cascades.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <array>
#include <memory>
#include <span>

namespace kin {

// What 3D views can have on this device.
struct Renderer3DCapabilities {
    // The depth buffer views draw with: 32-bit float where the device has it,
    // else 24 or 16-bit fixed point (less precise far away).
    u32 depth_bits = 0;
    bool depth_float = false;
    // The most samples a pixel of a view can take (MSAA; 1: none), with 8-bit
    // colour and with HDR (16-bit float) colour.
    u32 max_samples = 1;
    u32 max_samples_hdr = 1;
    // Whether Scene3D::ambient_occlusion, ::contact_shadows and ::taa can work
    // here (the formats and compute they need); where not, they are off and a
    // line in the log says so the first time one is asked for.
    bool ambient_occlusion = false;
    bool contact_shadows = false;
    bool temporal_aa = false;
};

struct View3DOptions {
    // Samples a pixel (MSAA: smooth edges), 1, 2, 4 or 8; fewer where the
    // device has fewer.
    u32 samples = 4;
    // What shows where nothing is drawn; transparent lets what is under the
    // view show through.
    Color background = colors::transparent;
    // Light kept past white (a float picture), exposed and tonemapped (ACES:
    // brights roll off, the darkest tones darken a little) on its way to the
    // 2D frame; left linear for an HDR 2D pipeline, which tonemaps itself.
    // On with bloom.
    bool hdr = false;
    f32 exposure = 1.0f;
    // A glow round what is brighter than `bloom_threshold` (linear), this
    // strong (0: none). Emissive materials past 1 glow.
    f32 bloom = 0.0f;
    f32 bloom_threshold = 1.0f;
    // What is under each pixel kept too (Renderer3D::pick): one more pass
    // over the scene's opaque meshes.
    bool pick = false;
};

// What a pixel of a view shows (Renderer3D::pick): the mesh copy nearest the
// camera there (its MeshInstance::id) and the point of it the pixel shows.
struct PickResult {
    bool hit = false; // false: nothing but background there
    u32 id = 0;
    Vec3f position{};
};

// What the last render() drew.
struct Render3DStats {
    u32 draws = 0;     // draw calls
    u32 instances = 0; // meshes drawn
    u32 culled = 0;    // meshes skipped, outside the view
    u32 shadow_draws = 0; // draw calls into the shadow map
    u32 shadow_casters = 0; // meshes drawn into it (those seen, and those casting into view; all cascades)
    u32 shadow_cascades = 0; // maps drawn (0: no shadows)
    std::array<u32, max_shadow_cascades> cascade_casters{}; // meshes drawn into each cascade
    u32 ao_draws = 0;      // draw calls into the depth and normal pre-pass (ambient occlusion or contact shadows; 0: neither this render)
    u32 ao_instances = 0;  // meshes drawn into it
    u32 ao_dispatches = 0; // compute dispatches (the occlusion and its blur)
    u32 contact_dispatches = 0; // compute dispatches of the contact shadows (the march and its blur): 0 when off
    u32 taa_dispatches = 0; // compute dispatches of the temporal anti-aliasing (the resolve, and the sharpen): 0 when off
    bool taa_reset = false; // its history was not used this render: the first, after a resize or reset_history(), or the camera changed kind
    Vec2f taa_jitter{};     // this render's offset in pixels (0, 0 when off)
    u32 samples = 0;        // samples a pixel of the scene took: the view's, or 1 with temporal anti-aliasing
    u32 lights = 0;    // point lights used
    u32 environment_prefilters = 0; // environments filtered by this render (once each)
    u64 triangles = 0; // drawn by the main pass: at the level each copy is drawn at (Scene3D::lod)
    // With Scene3D::lod: the copies the camera sees, by the level of detail they are drawn at (all in 0 when it is off).
    std::array<u32, max_mesh_levels> lod_copies_by_level{};
    f64 cpu_ms = 0.0;  // render()'s own time on the CPU
};

// A view's targets on the backend.
class IView3DBackend {
public:
    virtual ~IView3DBackend() = default;
    // The picture, premultiplied, to draw with Renderer2D::draw_texture.
    virtual Texture texture() const = 0;
    virtual Vec2i size() const = 0;
    virtual u32 samples() const = 0;
    // Forgets the temporal anti-aliasing's history, so the next frame starts
    // afresh (a camera cut, a scene change).
    virtual void reset_history() {}
};

class IRenderer3DBackend {
public:
    virtual ~IRenderer3DBackend() = default;

    virtual Renderer3DCapabilities capabilities() const = 0;
    // Null when the data can't be uploaded (validated already). `chain`, when
    // not null, holds the levels of detail to keep with it (level 0 of it is `data`).
    virtual std::shared_ptr<IMeshBackend> create_mesh(const MeshData& data, const MeshLodChain* chain) = 0;
    // An equirectangular picture of `size` pixels, three floats (linear RGB)
    // each, rows top to bottom; its filtered copies are made on the first
    // render that uses it. Null when it can't be made (no compute).
    virtual std::shared_ptr<IEnvironmentBackend> create_environment(Vec2i size, std::span<const f32> rgb) = 0;
    // Null when the SPIR-V can't be made into a fragment shader.
    virtual std::shared_ptr<IMaterialShaderBackend> create_material_shader(std::span<const u8> spirv) = 0;
    virtual std::shared_ptr<IView3DBackend> create_view(Vec2i size, const View3DOptions& options) = 0;
    virtual bool resize_view(IView3DBackend& view, Vec2i size) = 0;
    // Records `scene` into `view`, after the 2D draws so far and before those
    // to come.
    virtual bool render(IView3DBackend& view, const Scene3D& scene, Render3DStats& stats) = 0;
    // What `pixel` of the view's last render shows; waits for the GPU.
    virtual PickResult pick(IView3DBackend& view, Vec2i pixel) = 0;
};

} // namespace kin
