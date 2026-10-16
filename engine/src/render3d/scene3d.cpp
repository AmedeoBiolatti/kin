#include <kin/render3d/scene3d.hpp>

#include <kin/platform/log.hpp>
#include <kin/render3d/model.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace kin {

namespace {
bool valid_bounds(const Bounds3& b) {
    return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.min.z) &&
           std::isfinite(b.max.x) && std::isfinite(b.max.y) && std::isfinite(b.max.z) &&
           b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}
template <class Transform> Bounds3 instance_bounds_of(const Bounds3& mesh, std::size_t count, const Transform& at) {
    Bounds3 b{};
    const Vec3f centre = mesh.center();
    const f32 radius = mesh.radius();
    for (std::size_t i = 0; i < count; ++i) {
        const Mat4f& t = at(i);
        const Vec3f c = t.apply(centre);
        const f32 sx = t.m[0] * t.m[0] + t.m[1] * t.m[1] + t.m[2] * t.m[2];
        const f32 sy = t.m[4] * t.m[4] + t.m[5] * t.m[5] + t.m[6] * t.m[6];
        const f32 sz = t.m[8] * t.m[8] + t.m[9] * t.m[9] + t.m[10] * t.m[10];
        f32 r = radius * std::sqrt(std::max({sx, sy, sz}));
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(r)) return {{1, 1, 1}, {-1, -1, -1}};
        r += 1e-5f * (1 + std::max({std::abs(c.x), std::abs(c.y), std::abs(c.z)}) + r); // outward rounding margin
        const Vec3f lo = c - Vec3f{r, r, r}, hi = c + Vec3f{r, r, r};
        if (i == 0) b = {lo, hi};
        else {
            b.min = {std::min(b.min.x, lo.x), std::min(b.min.y, lo.y), std::min(b.min.z, lo.z)};
            b.max = {std::max(b.max.x, hi.x), std::max(b.max.y, hi.y), std::max(b.max.z, hi.z)};
        }
    }
    return b;
}
} // namespace

Bounds3 instance_bounds(const Mesh& mesh, std::span<const Mat4f> transforms) {
    return instance_bounds_of(mesh.bounds(), transforms.size(), [&](std::size_t i) -> const Mat4f& { return transforms[i]; });
}
Bounds3 instance_bounds(const Mesh& mesh, std::span<const MeshInstance> instances) {
    return instance_bounds_of(mesh.bounds(), instances.size(), [&](std::size_t i) -> const Mat4f& { return instances[i].transform; });
}

struct InstanceBatch3D::Data {
    Mesh mesh;
    std::vector<MeshInstance> instances;
    std::vector<InstanceGroup3D> groups;
    u64 id = 0, revision = 1, layout_revision = 1;
    std::array<ChangedRange, 16> changes{}; // bounded history, no per-instance dirty array
};
InstanceBatch3D::InstanceBatch3D(Mesh mesh, std::vector<MeshInstance> instances, std::vector<InstanceGroup3D> groups) {
    if (instances.size() > std::numeric_limits<u32>::max()) throw std::length_error("instance batch too large");
    u32 end = 0;
    for (auto& group : groups) {
        if (!group.count || group.first < end || group.first > instances.size() || group.count > instances.size() - group.first)
            throw std::invalid_argument("instance batch groups must be ordered, nonempty and disjoint");
        group.world_bounds = instance_bounds(mesh, std::span<const MeshInstance>(instances).subspan(group.first, group.count));
        end = group.first + group.count;
    }
    static std::atomic<u64> next{1};
    _data = std::make_shared<Data>(Data{std::move(mesh), std::move(instances), std::move(groups), next.fetch_add(1), 1});
}
const Mesh& InstanceBatch3D::mesh() const { static const Mesh empty; return _data ? _data->mesh : empty; }
std::span<const MeshInstance> InstanceBatch3D::instances() const { return _data ? std::span<const MeshInstance>(_data->instances) : std::span<const MeshInstance>{}; }
std::span<const InstanceGroup3D> InstanceBatch3D::groups() const { return _data ? std::span<const InstanceGroup3D>(_data->groups) : std::span<const InstanceGroup3D>{}; }
u64 InstanceBatch3D::identity() const { return _data ? _data->id : 0; }
u64 InstanceBatch3D::revision() const { return _data ? _data->revision : 0; }
u64 InstanceBatch3D::layout_revision() const { return _data ? _data->layout_revision : 0; }
InstanceBatch3D::ChangedRange InstanceBatch3D::changed_since(u64 revision) const {
    if (!_data || revision == _data->revision) return {};
    const u32 count = static_cast<u32>(_data->instances.size());
    if (!revision || revision > _data->revision || _data->revision - revision > _data->changes.size()) return {0, count};
    u32 first = count, end = 0;
    const u64 edits = _data->revision - revision;
    for (u64 i = 0; i < edits; ++i) {
        const auto& range = _data->changes[(_data->revision - i) % _data->changes.size()];
        first = std::min(first, range.first);
        end = std::max(end, range.first + range.count);
    }
    return {first, end - first};
}
void InstanceBatch3D::update(u32 first, std::span<const MeshInstance> instances) {
    if (!_data || first > _data->instances.size() || instances.size() > _data->instances.size() - first)
        throw std::out_of_range("instance batch update");
    if (instances.empty()) return;
    // Copy via memmove so a range from this batch may also be used as the source.
    std::memmove(_data->instances.data() + first, instances.data(), instances.size_bytes());
    bool layout_changed = false;
    for (auto& group : _data->groups) if (group.first < first + instances.size() && first < group.first + group.count) {
        const bool was_valid = valid_bounds(group.world_bounds);
        group.world_bounds = instance_bounds(_data->mesh, std::span<const MeshInstance>(_data->instances).subspan(group.first, group.count));
        layout_changed = layout_changed || was_valid != valid_bounds(group.world_bounds);
    }
    if (layout_changed) ++_data->layout_revision;
    ++_data->revision;
    _data->changes[_data->revision % _data->changes.size()] = {first, static_cast<u32>(instances.size())};
}
Scene3D::DrawStamp::DrawStamp() {
    // Once per scene/move, never once per submitted draw. Address reuse cannot revive a layout.
    static std::atomic<u64> next{1};
    identity = next.fetch_add(1, std::memory_order_relaxed);
}
Scene3D::DrawStamp::DrawStamp(DrawStamp&& other) noexcept : DrawStamp() { ++other.revision; }
Scene3D::DrawStamp& Scene3D::DrawStamp::operator=(DrawStamp&& other) noexcept {
    DrawStamp fresh;
    identity = fresh.identity; revision = 0;
    ++other.revision;
    return *this;
}

void Scene3D::draw_instanced(const InstanceBatch3D& batch, const Material3D& material) {
    if (!batch || batch.instances().empty()) return;
    touch_draws();
    Draw draw{batch.mesh(), material, 0, static_cast<u32>(batch.instances().size())};
    draw.batch = batch;
    _draws.push_back(std::move(draw));
}

std::span<MeshInstance> Scene3D::add_instances(const Mesh& mesh, std::size_t count, const Material3D& material) {
    return add_instances(mesh, count, material, false, 0);
}

std::span<MeshInstance> Scene3D::add_instances(const Mesh& mesh, std::size_t count, const Material3D& material, bool skinned, u32 skin_first) {
    if (count == 0) {
        return {};
    }
    touch_draws();
    const u32 first = static_cast<u32>(_instances.size());
    _instances.resize(_instances.size() + count);
    // Straight after a draw of the same mesh and material: one draw (skinned copies only with skinned ones: the pose of
    // each is its own, and a plain copy of the mesh has none).
    if (!_draws.empty() && !_draws.back().batch && _draws.back().mesh == mesh && _draws.back().material == material &&
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
    draw_skinned(model, joint_matrices, transform, tint, id, {});
}
void Scene3D::draw_skinned(const Model& model, std::span<const Mat4f> joint_matrices, const Mat4f& transform, Color tint, u32 id,
                           std::span<const Material3D> materials) {
    if (!materials.empty() && materials.size() != model.parts.size()) throw std::invalid_argument("material count must match model parts");
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
    for (size_t part_index = 0; part_index < model.parts.size(); ++part_index) {
        const auto& part = model.parts[part_index];
        const auto& material = materials.empty() ? part.material : materials[part_index];
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
            add_skinned_copy(part.mesh, first_joint, transform, material, tint, id);
        } else {
            draw(part.mesh, transform * part.transform, material, tint, id);
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

void Scene3D::add_group(u32 first, u32 count, const Bounds3& bounds) {
    if (!count || !valid_bounds(bounds)) return;
    Draw& draw = _draws.back();
    if (!draw.group_count) draw.group_first = static_cast<u32>(_instance_groups.size());
    _instance_groups.push_back({first - draw.first, count, bounds});
    ++draw.group_count;
}
void Scene3D::draw_instanced_group(const Mesh& mesh, std::span<const Mat4f> transforms, const Bounds3& bounds, const Material3D& material) {
    const u32 first = static_cast<u32>(_instances.size());
    draw_instanced(mesh, transforms, material);
    add_group(first, static_cast<u32>(transforms.size()), bounds);
}
void Scene3D::draw_instanced_group(const Mesh& mesh, std::span<const MeshInstance> instances, const Bounds3& bounds, const Material3D& material) {
    const u32 first = static_cast<u32>(_instances.size());
    draw_instanced(mesh, instances, material);
    add_group(first, static_cast<u32>(instances.size()), bounds);
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

void Scene3D::update_joint_matrices(u32 first, std::span<const Mat4f> matrices) {
    if (first > _joints.size() || matrices.size() > _joints.size() - first)
        throw std::out_of_range("joint matrix update outside submitted poses");
    if (!matrices.empty())
        std::memmove(_joints.data() + first, matrices.data(), matrices.size_bytes());
}

void Scene3D::clear() {
    touch_draws();
    _draws.clear();
    _instances.clear();
    _instance_groups.clear();
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
