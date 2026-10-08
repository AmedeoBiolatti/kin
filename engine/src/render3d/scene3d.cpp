#include <kin/render3d/scene3d.hpp>

#include <algorithm>

namespace kin {

std::span<MeshInstance> Scene3D::add_instances(const Mesh& mesh, std::size_t count, const Material3D& material) {
    if (count == 0) {
        return {};
    }
    const u32 first = static_cast<u32>(_instances.size());
    _instances.resize(_instances.size() + count);
    // Straight after a draw of the same mesh and material: one draw.
    if (!_draws.empty() && _draws.back().mesh == mesh && _draws.back().material == material &&
        _draws.back().first + _draws.back().count == first) {
        _draws.back().count += static_cast<u32>(count);
    } else {
        _draws.push_back({mesh, material, first, static_cast<u32>(count)});
    }
    return {_instances.data() + first, count};
}

void Scene3D::draw(const Mesh& mesh, const Mat4f& transform, const Material3D& material, Color tint) {
    add_instances(mesh, 1, material)[0] = {transform, tint};
}

void Scene3D::draw_instanced(const Mesh& mesh, std::span<const MeshInstance> instances, const Material3D& material) {
    if (instances.empty()) {
        return;
    }
    const std::span<MeshInstance> room = add_instances(mesh, instances.size(), material);
    std::copy(instances.begin(), instances.end(), room.begin());
}

void Scene3D::draw_instanced(const Mesh& mesh, std::span<const Mat4f> transforms, const Material3D& material) {
    if (transforms.empty()) {
        return;
    }
    const std::span<MeshInstance> room = add_instances(mesh, transforms.size(), material);
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        room[i] = {transforms[i], colors::white};
    }
}

Scene3D& Scene3D::overlay() {
    if (!_overlay) {
        _overlay = std::make_unique<Scene3D>();
    }
    return *_overlay;
}

void Scene3D::clear() {
    _draws.clear();
    _instances.clear();
    if (_overlay) {
        _overlay->clear();
    }
}

} // namespace kin
