#pragma once

// Experimental 3D rendering (docs/3d_plan.md): 3D views drawn inside a 2D
// frame, on the SDL_GPU backend. The API may change in any release. Built
// with KIN_EXPERIMENTAL_3D (a CMake option).
//
//     kin::Renderer3D r3d{renderer};
//     kin::Mesh cube = r3d.create_mesh(kin::meshes::box({1, 1, 1}));
//     kin::View3D view = r3d.create_view({640, 360});
//     ...each frame:
//     kin::Scene3D scene;
//     scene.camera = kin::Camera3D::orbit({}, angle, 30.0f, 4.0f);
//     scene.draw(cube, kin::Mat4f::identity(), {.color = kin::Color::rgb(200, 80, 60)});
//     r3d.render(view, scene);                       // after the 2D drawn so far
//     renderer.draw_texture(view.texture(), {0, 0, 640, 360});
//
// Without SDL_GPU (SDL_Renderer, headless runs), meshes are empty and a view
// shows only its background.

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D rendering is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <kin/render3d/backend3d.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/model.hpp>
#include <kin/render3d/scene3d.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace kin {

class Renderer2D;

// A picture a 3D scene is drawn into (colour and depth), laid into the frame
// with Renderer2D::draw_texture(view.texture(), rect). Cheap to copy (copies
// share it). It keeps its picture until rendered again.
class View3D {
public:
    View3D() = default;
    explicit View3D(std::shared_ptr<IView3DBackend> backend) : _backend(std::move(backend)) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    // Premultiplied; a new texture after resize().
    Texture texture() const { return _backend ? _backend->texture() : Texture{}; }
    Vec2i size() const { return _backend ? _backend->size() : Vec2i{}; }
    u32 samples() const { return _backend ? _backend->samples() : 1u; }
    f32 aspect() const {
        const Vec2i s = size();
        return s.y > 0 ? static_cast<f32>(s.x) / static_cast<f32>(s.y) : 1.0f;
    }
    IView3DBackend* backend() const { return _backend.get(); }
    // Drops the history Scene3D::taa blends in, so that the next frame does
    // not show the one before: call it when the camera cuts to somewhere
    // else, or the scene is replaced. (A resize does it by itself.)
    void reset_history() {
        if (_backend) {
            _backend->reset_history();
        }
    }

private:
    std::shared_ptr<IView3DBackend> _backend;
};

class Renderer3D {
public:
    // Draws through `renderer`, which must outlive it.
    explicit Renderer3D(Renderer2D& renderer);

    // Whether the renderer's backend draws 3D (SDL_GPU does, SDL_Renderer
    // doesn't).
    bool available() const { return _backend != nullptr; }
    // What views can have; all defaults where 3D isn't available.
    Renderer3DCapabilities capabilities() const;
    Renderer2D& renderer() const { return *_renderer; }

    // `data` kept on the GPU. Empty when 3D isn't available or the data is
    // not valid (MeshData::validate, logged).
    Mesh create_mesh(const MeshData& data);
    // The same with levels of detail (MeshOptions::levels): simpler versions of
    // the mesh that Scene3D::lod draws copies far from the camera with. Level 0
    // is `data` itself, so the picture without Scene3D::lod is the same. Takes
    // time to build (about a second for a million triangles).
    Mesh create_mesh(const MeshData& data, const MeshOptions& options);
    // A view `size` pixels (at least 1 x 1). Where 3D isn't available, a
    // render target that render() fills with the background.
    View3D create_view(Vec2i size, const View3DOptions& options = {});
    // A glTF 2.0 model (.glb or .gltf, read through the content folder or
    // pack): its meshes, materials and textures (mipmapped). Empty, with why
    // in `error` (and logged), when it can't be read or 3D isn't available.
    Model load_model(const std::filesystem::path& path, std::string* error = nullptr);
    // The same, each of its meshes made with `options` (levels of detail).
    Model load_model(const std::filesystem::path& path, const MeshOptions& options, std::string* error = nullptr);
    // An environment (render3d/environment.hpp) from a picture: `size` pixels
    // of linear RGB floats, rows top to bottom, 2:1 equirectangular. Filtered
    // on the GPU (with compute) the first time a render uses it; empty where
    // 3D or compute isn't available.
    Environment3D create_environment(Vec2i size, std::span<const f32> rgb);
    // One from a Radiance .hdr file, read through the content folder or pack.
    // Empty, with why in `error` (and logged), when it can't be read.
    Environment3D load_environment(const std::filesystem::path& path, std::string* error = nullptr);
    // A fragment shader for materials (Material3D::shader), from SPIR-V
    // compiled against kin_material.inc.glsl (docs/rendering_3d.md, Custom
    // shaders). Empty where 3D isn't available or the shader is refused.
    MaterialShader3D create_material_shader(std::span<const u8> spirv);
    // The same from GLSL source, compiled now by glslc (development: see
    // kin/renderer/shader_compiler.hpp; shipped games load SPIR-V). Empty,
    // with glslc's messages in `error` (and logged), when it doesn't compile.
    MaterialShader3D compile_material_shader(const std::filesystem::path& source, std::string* error = nullptr);
    // Gives `view` a new size (its texture changes); false when it can't.
    bool resize(View3D& view, Vec2i size);
    // Draws `scene` into `view`: after the 2D drawn so far in this frame, and
    // before the 2D drawn after (which may draw view.texture()).
    bool render(View3D& view, const Scene3D& scene);
    // What `pixel` (the view's own pixels, from its top left) showed when
    // `view` was last rendered: the copy there and the point of it. Needs a
    // view made with View3DOptions::pick. It waits for the GPU to finish
    // what is recorded so far (submitting it): ask when something calls for
    // it (a click, the pointer moving), not every frame.
    PickResult pick(View3D& view, Vec2i pixel);
    // What the last render() drew.
    const Render3DStats& stats() const { return _stats; }

private:
    Renderer2D* _renderer = nullptr;
    IRenderer3DBackend* _backend = nullptr;
    Render3DStats _stats{};
    bool _warned = false;
};

} // namespace kin
