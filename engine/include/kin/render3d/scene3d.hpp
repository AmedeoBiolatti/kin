#pragma once

// What a 3D view shows, filled each frame (docs/3d_plan.md): a camera,
// lights, and meshes to draw. Plain data, no GPU: Renderer3D::render() draws it.
//
//     kin::Scene3D scene;
//     scene.camera = kin::Camera3D::orbit(target, 30.0f, 40.0f, 8.0f);
//     scene.draw(cube, kin::Mat4f::rotation_y(angle), {.color = red});
//     scene.draw_instanced(rock, rock_instances);
//     r3d.render(view, scene);

#include <kin/core/math3d.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kin {

class Model;

// How a material's colour goes over what is behind it.
enum class Blend3D : u8 {
    Opaque,   // replaces it, hides what is further
    Alpha,    // over it by its alpha (drawn after the opaque, furthest first)
    Additive, // adds light to it: sparks, glows, flashes (in any order)
};

struct Material3D {
    Color color = colors::white; // times the mesh's vertex colours and each instance's tint
    // Light of its own, as a share of its colour: glows in the dark, and
    // past white feeds bloom in an HDR view.
    f32 emissive = 0.0f;
    bool unlit = false;        // its colour as it is, no light or shade
    bool double_sided = false; // its back faces drawn too (else culled)
    Blend3D blend = Blend3D::Opaque;
    // Opaque meshes cast the sun's shadow (Lights3D::shadow_area) unless this
    // is off, and take it unless `receives_shadow` is.
    bool casts_shadow = true;
    bool receives_shadow = true;
    // Only its triangles' edges, a pixel wide, in its colour (no light),
    // drawn just in front of the faces they edge: a model's wireframe drawn
    // over it, or lines (triangles a, b, b) on the ground. Needs line fill
    // (desktop GPUs have it): where its pipeline can't be made, not drawn.
    bool wireframe = false;
    // The base colour's texture (the mesh's UVs), times the colours above;
    // none: white.
    Texture texture{};

    friend bool operator==(const Material3D&, const Material3D&) = default;
};

// One copy of a mesh in a draw_instanced() call. A transform that mirrors
// (a negative scale) turns its faces inside out: draw it double-sided.
struct MeshInstance {
    Mat4f transform{};
    Color tint = colors::white;
    u32 id = 0; // what Renderer3D::pick says is under a pixel showing it
};

// Light from far away, in one direction: the sun.
struct DirectionalLight {
    Vec3f direction{-0.4f, 1.0f, 0.3f}; // toward the light (any length but zero)
    Vec3f color{1.0f, 1.0f, 1.0f};      // linear; above 1 is brighter
};

// Light from a point, fading to nothing at `radius`: a muzzle flash, a lamp.
struct PointLight {
    Vec3f position{};
    Vec3f color{1.0f, 1.0f, 1.0f}; // linear; above 1 is brighter
    f32 radius = 5.0f;
};

struct Lights3D {
    DirectionalLight sun{};
    // A second sun that casts no shadow, to lift the far side (black: none).
    DirectionalLight fill{.direction = {0.6f, 0.4f, -0.5f}, .color = {0.0f, 0.0f, 0.0f}};
    Vec3f ambient{0.25f, 0.25f, 0.28f}; // light from everywhere, linear
    // With it, ambient light comes from the sky above (`ambient`) to the
    // ground below (this), by which way a face looks; without, from all round.
    std::optional<Vec3f> ground_ambient{};
    // Where the sun casts shadows: a box round what casts and receives them
    // (the level, say). Empty: no shadows. Smaller is sharper.
    Bounds3 shadow_area{};
    u32 shadow_resolution = 2048; // the shadow map's side, in texels
    // At most 16 light a frame: those that reach the view, the strongest
    // and nearest first. Cleared with the draws.
    std::vector<PointLight> points;
};

class Scene3D {
public:
    Scene3D() = default;
    Scene3D(Scene3D&&) noexcept = default;
    Scene3D& operator=(Scene3D&&) noexcept = default;

    Camera3D camera{};
    Lights3D lights{};
    // Copies wholly outside the camera's view are skipped (each tested as the
    // sphere round its mesh's bounds, moved and scaled with it).
    bool cull = true;

    // What is drawn over the scene, in front of everything in it, seen by its
    // own camera and lit by this scene's lights: a first-person gun or hands,
    // which then never sink into a wall. Made on first use; its own overlay
    // is never drawn.
    Scene3D& overlay();
    const Scene3D* overlay_if_any() const { return _overlay.get(); }

    // `mesh` placed by `transform`. Draws of the same mesh and material one
    // after another are drawn as one (instanced).
    void draw(const Mesh& mesh, const Mat4f& transform, const Material3D& material = {},
              Color tint = colors::white, u32 id = 0);
    // Each part of `model` (model.hpp), placed by `transform`.
    void draw(const Model& model, const Mat4f& transform, Color tint = colors::white);
    // Many copies of `mesh`, in one draw.
    void draw_instanced(const Mesh& mesh, std::span<const MeshInstance> instances, const Material3D& material = {});
    void draw_instanced(const Mesh& mesh, std::span<const Mat4f> transforms, const Material3D& material = {});
    // Room for `count` copies of `mesh`, for the caller to fill (from worker
    // threads, say). Valid until the next draw is added.
    std::span<MeshInstance> add_instances(const Mesh& mesh, std::size_t count, const Material3D& material = {});
    void add_light(const PointLight& light) { lights.points.push_back(light); }
    // Takes the draws and point lights away, the overlay's too (cameras, the
    // sun and the ambient light stay), keeping their room.
    void clear();

    struct Draw {
        Mesh mesh;
        Material3D material;
        u32 first = 0; // into instances()
        u32 count = 0;
    };
    std::span<const Draw> draws() const { return _draws; }
    std::span<const MeshInstance> instances() const { return _instances; }

private:
    std::vector<Draw> _draws;
    std::vector<MeshInstance> _instances;
    std::unique_ptr<Scene3D> _overlay;
};

} // namespace kin
