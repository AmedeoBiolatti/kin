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

#include <span>
#include <vector>

namespace kin {

struct Material3D {
    Color color = colors::white; // times the mesh's vertex colours and each instance's tint
    // Light of its own, as a share of its colour: glows in the dark, and
    // past white feeds bloom in an HDR scene.
    f32 emissive = 0.0f;
    bool unlit = false;        // its colour as it is, no light or shade
    bool double_sided = false; // its back faces drawn too (else culled)

    friend bool operator==(const Material3D&, const Material3D&) = default;
};

// One copy of a mesh in a draw_instanced() call. A transform that mirrors
// (a negative scale) turns its faces inside out: draw it double-sided.
struct MeshInstance {
    Mat4f transform{};
    Color tint = colors::white;
};

// Light from far away, in one direction: the sun.
struct DirectionalLight {
    Vec3f direction{-0.4f, 1.0f, 0.3f}; // toward the light (any length but zero)
    Vec3f color{1.0f, 1.0f, 1.0f};      // linear; above 1 is brighter
};

struct Lights3D {
    DirectionalLight sun{};
    Vec3f ambient{0.25f, 0.25f, 0.28f}; // light from everywhere, linear
};

class Scene3D {
public:
    Camera3D camera{};
    Lights3D lights{};

    // `mesh` placed by `transform`. Draws of the same mesh and material one
    // after another are drawn as one (instanced).
    void draw(const Mesh& mesh, const Mat4f& transform, const Material3D& material = {},
              Color tint = colors::white);
    // Many copies of `mesh`, in one draw.
    void draw_instanced(const Mesh& mesh, std::span<const MeshInstance> instances, const Material3D& material = {});
    void draw_instanced(const Mesh& mesh, std::span<const Mat4f> transforms, const Material3D& material = {});
    // Room for `count` copies of `mesh`, for the caller to fill (from worker
    // threads, say). Valid until the next draw is added.
    std::span<MeshInstance> add_instances(const Mesh& mesh, std::size_t count, const Material3D& material = {});
    // Takes the draws away (camera and lights stay), keeping their room.
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
};

} // namespace kin
