#include <kin/ecs/render3d.hpp>

namespace kin {

WorldRenderState3D::WorldRenderState3D(flecs::world& world)
    : _transform_observer(world.observer<Transform3D>("add_world_transform_3d")
                              .event(flecs::OnAdd)
                              .yield_existing()
                              .each([](flecs::entity entity, Transform3D&) { entity.ensure<WorldTransform3D>(); })),
      _transforms(world.query_builder<WorldTransform3D, const Transform3D, const WorldTransform3D>()
                      .term_at(2).parent().cascade().optional()
                      .cached()
                      .build()),
      _meshes(world.query_builder<const WorldTransform3D, const MeshRenderer>().cached().build()) {}

void WorldRenderState3D::propagate_transforms() {
    // Tables come parents first (cascade), and every entity in a table shares
    // its parent: one tight loop a table.
    _transforms.run([](flecs::iter& it) {
        while (it.next()) {
            const auto self = it.field<WorldTransform3D>(0);
            const auto local = it.field<const Transform3D>(1);
            if (!it.is_set(2)) {
                for (const auto i : it) {
                    self[i].matrix = local[i].matrix();
                }
                continue;
            }
            const Mat4f parent = it.field<const WorldTransform3D>(2)[0].matrix;
            for (const auto i : it) {
                self[i].matrix = parent * local[i].matrix();
            }
        }
    });
}

void WorldRenderState3D::collect(Scene3D& scene) {
    for (Bucket& bucket : _buckets) {
        bucket.instances.clear();
    }
    _meshes.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto world = it.field<const WorldTransform3D>(0);
            const auto renderers = it.field<const MeshRenderer>(1);
            Bucket* last = nullptr; // entities of a table mostly share a mesh
            for (const auto i : it) {
                const MeshRenderer& r = renderers[i];
                if (!r.visible || !r.mesh.valid()) {
                    continue;
                }
                if (!last || !(last->mesh == r.mesh) || !(last->material == r.material)) {
                    last = nullptr;
                    for (Bucket& bucket : _buckets) {
                        if (bucket.mesh == r.mesh && bucket.material == r.material) {
                            last = &bucket;
                            break;
                        }
                    }
                    if (!last) {
                        last = &_buckets.emplace_back(Bucket{r.mesh, r.material, {}});
                    }
                }
                last->instances.push_back({world[i].matrix, r.tint});
            }
        }
    });
    for (const Bucket& bucket : _buckets) {
        scene.draw_instanced(bucket.mesh, bucket.instances, bucket.material);
    }
    // Meshes no longer drawn are let go.
    std::erase_if(_buckets, [](const Bucket& bucket) { return bucket.instances.empty(); });
}

} // namespace kin
