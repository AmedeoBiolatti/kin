#include <kin/render3d/scene3d.hpp>

#include <kin/platform/log.hpp>
#include <kin/render3d/model.hpp>

#include <algorithm>

namespace kin {

std::span<MeshInstance> Scene3D::add_instances(const Mesh& mesh, std::size_t count, const Material3D& material) {
    return add_instances(mesh, count, material, false, 0);
}

std::span<MeshInstance> Scene3D::add_instances(const Mesh& mesh, std::size_t count, const Material3D& material, bool skinned, u32 skin_first) {
    if (count == 0) {
        return {};
    }
    const u32 first = static_cast<u32>(_instances.size());
    _instances.resize(_instances.size() + count);
    // Straight after a draw of the same mesh and material: one draw (skinned copies only with skinned ones: the pose of
    // each is its own, and a plain copy of the mesh has none).
    if (!_draws.empty() && _draws.back().mesh == mesh && _draws.back().material == material &&
        _draws.back().first + _draws.back().count == first && _draws.back().skinned == skinned) {
        _draws.back().count += static_cast<u32>(count);
    } else {
        Draw draw{mesh, material, first, static_cast<u32>(count)};
        draw.skinned = skinned;
        draw.skin_first = skin_first;
        _draws.push_back(std::move(draw));
    }
    return {_instances.data() + first, count};
}

void Scene3D::add_skinned_copy(const Mesh& mesh, u32 first_joint, const Mat4f& transform, const Material3D& material, Color tint, u32 id) {
    const u32 first_copy = static_cast<u32>(_skin_offsets.size()); // a draw of its own starts here
    _skin_offsets.push_back(first_joint);
    add_instances(mesh, 1, material, true, first_copy)[0] = {transform, tint, id};
}

void Scene3D::draw_skinned(const Mesh& mesh, std::span<const Mat4f> joint_matrices, const Mat4f& transform, const Material3D& material,
                           Color tint, u32 id) {
    if (!mesh.skinned()) {
        draw(mesh, transform, material, tint, id);
        return;
    }
    if (joint_matrices.size() < mesh.skin_joint_count()) {
        if (!_warned_skin) {
            _warned_skin = true;
            KIN_LOG_WARN_F("render", "3D: draw_skinned with too few joint matrices: not drawn",
                           (LogFields{{.name = "given", .value = std::to_string(joint_matrices.size())},
                                      {.name = "needed", .value = std::to_string(mesh.skin_joint_count())}}));
        }
        return;
    }
    // Only the joints the mesh uses are kept: a skeleton may have more joints than the mesh (it is the mesh's, as the skin).
    const std::size_t keep = mesh.skin_joint_count();
    const u32 first_joint = static_cast<u32>(_joints.size());
    _joints.insert(_joints.end(), joint_matrices.begin(), joint_matrices.begin() + static_cast<std::ptrdiff_t>(keep));
    add_skinned_copy(mesh, first_joint, transform, material, tint, id);
}

void Scene3D::draw_skinned(const Model& model, std::span<const Mat4f> joint_matrices, const Mat4f& transform, Color tint, u32 id) {
    // One range for the pose, as many matrices as the most any part uses (a part uses the first of them: its joints are the
    // skeleton's, in the skeleton's order).
    std::size_t need = 0;
    for (const ModelPart& part : model.parts) {
        if (part.mesh.skinned() && part.skin <= 0) { // the first skeleton's: the pose given is for it
            need = std::max<std::size_t>(need, part.mesh.skin_joint_count());
        }
    }
    u32 first_joint = 0;
    bool copied = false;
    for (const ModelPart& part : model.parts) {
        if (part.mesh.skinned()) {
            if (part.skin > 0) {
                continue;
            }
            if (joint_matrices.size() < need) {
                if (!_warned_skin) {
                    _warned_skin = true;
                    KIN_LOG_WARN_F("render", "3D: draw_skinned with too few joint matrices: not drawn",
                                   (LogFields{{.name = "given", .value = std::to_string(joint_matrices.size())},
                                              {.name = "needed", .value = std::to_string(need)}}));
                }
                continue;
            }
            if (!copied) {
                copied = true;
                first_joint = static_cast<u32>(_joints.size());
                _joints.insert(_joints.end(), joint_matrices.begin(), joint_matrices.begin() + static_cast<std::ptrdiff_t>(need));
            }
            add_skinned_copy(part.mesh, first_joint, transform, part.material, tint, id);
        } else {
            draw(part.mesh, transform * part.transform, part.material, tint, id);
        }
    }
}

void Scene3D::draw(const Mesh& mesh, const Mat4f& transform, const Material3D& material, Color tint, u32 id) {
    add_instances(mesh, 1, material)[0] = {transform, tint, id};
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
        room[i] = {transforms[i], colors::white, 0};
    }
}

Scene3D& Scene3D::overlay() {
    if (!_overlay) {
        _overlay = std::make_unique<Scene3D>();
    }
    return *_overlay;
}

void Scene3D::draw_particles(const ParticleLook3D& look, std::span<const ParticleSprite3D> sprites) {
    if (sprites.empty()) {
        return;
    }
    const u32 first = static_cast<u32>(_particle_sprites.size());
    _particle_sprites.insert(_particle_sprites.end(), sprites.begin(), sprites.end());
    _particle_draws.push_back({look, first, static_cast<u32>(sprites.size())});
}

void Scene3D::draw_particles(const ParticleSystem3D& system) {
    const u32 first = static_cast<u32>(_particle_sprites.size());
    system.sprites(_particle_sprites);
    const u32 count = static_cast<u32>(_particle_sprites.size()) - first;
    if (count > 0) {
        _particle_draws.push_back({system.desc.look, first, count});
    }
}

void Scene3D::clear() {
    _draws.clear();
    _instances.clear();
    _joints.clear();
    _skin_offsets.clear();
    _particle_draws.clear();
    _particle_sprites.clear();
    lights.points.clear();
    lights.spots.clear();
    if (_overlay) {
        _overlay->clear();
    }
}

} // namespace kin
