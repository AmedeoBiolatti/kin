#pragma once

// 3D entities (experimental, docs/3d_plan.md): a place in the world, composed
// through the flecs hierarchy as Transform2D is, and a mesh to draw there.
//
//     world.entity().set(kin::Transform3D{.position = {2, 0, 0}})
//                   .set(kin::MeshRenderer{.mesh = crate, .material = {.color = brown}});
//     kin::WorldRenderState3D state{world};
//     ...each frame:
//     state.propagate_transforms();
//     state.collect(scene);   // one instanced draw a mesh and material
//     r3d.render(view, scene);

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D rendering is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <kin/core/math3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/scene3d.hpp>

#include <flecs.h>

#include <vector>

namespace kin {

// Where an entity sits relative to its parent (flecs ChildOf), or to the
// world without one: scaled, then turned, then moved to `position`.
struct Transform3D {
    Vec3f position{};
    Quatf rotation{};
    Vec3f scale{1.0f, 1.0f, 1.0f};

    Mat4f matrix() const { return Mat4f::trs(position, rotation, scale); }
};

// Transform3D composed through the hierarchy (WorldRenderState3D::propagate_transforms):
// the entity's own space to the world's. Added with Transform3D.
struct WorldTransform3D {
    Mat4f matrix{};

    Vec3f position() const { return {matrix.m[12], matrix.m[13], matrix.m[14]}; }
};

// A mesh drawn at the entity's WorldTransform3D.
struct MeshRenderer {
    Mesh mesh;
    Material3D material{};
    Color tint = colors::white;
    bool visible = true;
};

class WorldRenderState3D {
public:
    explicit WorldRenderState3D(flecs::world& world);
    explicit WorldRenderState3D(EcsWorld& world) : WorldRenderState3D(world.raw()) {}

    // Every WorldTransform3D from its Transform3D and its parent's, parents first.
    void propagate_transforms();
    // A draw into `scene` for each mesh and material the MeshRenderers use, all
    // their entities instances of it (in the order the world keeps them).
    void collect(Scene3D& scene);

private:
    flecs::observer _transform_observer;
    flecs::query<WorldTransform3D, const Transform3D, const WorldTransform3D> _transforms;
    flecs::query<const WorldTransform3D, const MeshRenderer> _meshes;
    struct Bucket {
        Mesh mesh;
        Material3D material;
        std::vector<MeshInstance> instances;
    };
    std::vector<Bucket> _buckets; // kept between frames, emptied each collect
};

} // namespace kin
