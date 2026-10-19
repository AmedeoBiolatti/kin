#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <kin/platform/log.hpp>
#include <kin/render3d/model3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <limits>
#include <mutex>
#include <vector>

namespace kin {

namespace {
bool valid_bounds(const Bounds3D& b) {
    return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.min.z) && std::isfinite(b.max.x) &&
           std::isfinite(b.max.y) && std::isfinite(b.max.z) && b.min.x <= b.max.x && b.min.y <= b.max.y &&
           b.min.z <= b.max.z;
}
// One copy's box: the sphere round its mesh's bounds, moved and scaled with it, a hair larger (outward rounding). False
// for a copy whose centre or radius is not finite: a group holding one has no bounds.
struct InstanceBox {
    Vec3f lo, hi;
};
bool instance_box(const Vec3f& centre, f32 radius, const Mat4f& t, InstanceBox& out) {
    const Vec3f c = t.apply(centre);
    const f32 sx = t.m[0] * t.m[0] + t.m[1] * t.m[1] + t.m[2] * t.m[2];
    const f32 sy = t.m[4] * t.m[4] + t.m[5] * t.m[5] + t.m[6] * t.m[6];
    const f32 sz = t.m[8] * t.m[8] + t.m[9] * t.m[9] + t.m[10] * t.m[10];
    f32 r = radius * std::sqrt(std::max(std::max(sx, sy), sz));
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(r))
        return false;
    r += 1e-5f * (1 + std::max(std::max(std::abs(c.x), std::abs(c.y)), std::abs(c.z)) + r); // outward rounding margin
    out = {c - Vec3f{r, r, r}, c + Vec3f{r, r, r}};
    return true;
}
constexpr Bounds3D no_bounds{{1, 1, 1}, {-1, -1, -1}};
template <class Transform> Bounds3D instance_bounds_of(const Bounds3D& mesh, std::size_t count, const Transform& at) {
    Bounds3D b{};
    const Vec3f centre = mesh.center();
    const f32 radius = mesh.radius();
    for (std::size_t i = 0; i < count; ++i) {
        InstanceBox box;
        if (!instance_box(centre, radius, at(i), box))
            return no_bounds;
        if (i == 0)
            b = {box.lo, box.hi};
        else {
            b.min = {std::min(b.min.x, box.lo.x), std::min(b.min.y, box.lo.y), std::min(b.min.z, box.lo.z)};
            b.max = {std::max(b.max.x, box.hi.x), std::max(b.max.y, box.hi.y), std::max(b.max.z, box.hi.z)};
        }
    }
    return b;
}

// draw_skinned(Model3D) is given the pose of the first skeleton only, so a part on another skin has nothing to move it.
// Said once for each model (by address: it is only a log, so a reused address costs a missed line, and the list stops
// growing at 64).
void warn_unposed_skin(const Model3D& model, const ModelPart3D& part) {
    static std::mutex mutex;
    static std::vector<const Model3D*> said;
    {
        const std::lock_guard lock{mutex};
        if (said.size() >= 64 || std::find(said.begin(), said.end(), &model) != said.end())
            return;
        said.push_back(&model);
    }
    KIN_LOG_WARN_F(
        "render", "3D: draw_skinned poses a model's first skeleton only: its parts on another skin are not drawn",
        (LogFields{{.name = "part", .value = part.name}, {.name = "skin", .value = std::to_string(part.skin)}}));
}

// Where a part is drawn for a pose of the model's node skeleton: Model3D::part_transform, or its place at rest when its
// node has no matrix among `nodes` or one that is not finite (said once: a pose of the wrong skeleton, or one gone
// wrong).
Mat4f node_posed(const Model3D& model, std::size_t index, std::span<const Mat4f> nodes) {
    const ModelPart3D& part = model.parts[index];
    if (part.node < 0) {
        return part.transform;
    }
    const std::size_t at = static_cast<std::size_t>(part.node);
    if (at < nodes.size() && std::all_of(nodes[at].m, nodes[at].m + 16, [](f32 x) { return std::isfinite(x); })) {
        return model.part_transform(index, nodes);
    }
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        KIN_LOG_WARN_F("render",
                       "3D: a model's node pose lacks a part's matrix or it is not finite: the part is drawn at rest",
                       (LogFields{{.name = "part", .value = part.name},
                                  {.name = "node", .value = std::to_string(part.node)},
                                  {.name = "given", .value = std::to_string(nodes.size())}}));
    }
    return part.transform;
}
} // namespace

Bounds3D instance_bounds(const Mesh3D& mesh, std::span<const Mat4f> transforms) {
    return instance_bounds_of(mesh.bounds(), transforms.size(),
                              [&](std::size_t i) -> const Mat4f& { return transforms[i]; });
}
Bounds3D instance_bounds(const Mesh3D& mesh, std::span<const MeshInstance3D> instances) {
    return instance_bounds_of(mesh.bounds(), instances.size(),
                              [&](std::size_t i) -> const Mat4f& { return instances[i].transform; });
}

struct InstanceBatch3D::Data {
    Diagnostics3D errors;
    // Edits are tracked in blocks of this many instances (see changed_since).
    static constexpr u32 block = 16;
    // What update() learns about a group before it overwrites the records.
    struct Overlap {
        u32 group, begin, end, old_bad;
        u32 faces;
        bool whole;
    };
    Mesh3D mesh;
    std::vector<MeshInstance3D> instances;
    std::vector<InstanceGroup3D> groups;
    u64 id = 0, revision = 1, layout_revision = 1;
    std::array<ChangedRange, 16> changes{}; // the last edits exactly
    std::vector<u64>
        block_edit; // per block: the revision of its last edit (made at the first edit: a batch never edited has none)
    // Per group: how many of its copies have no finite box (the group has no bounds while any does), and whether
    // `world_bounds` is out of date (an edit may have moved the copy that set a face: found again, once, when the
    // bounds are read).
    std::vector<u32> bad;
    std::vector<u8> stale;
    std::vector<u32> stale_groups;
    std::atomic<bool> any_stale{false};
    std::mutex resolving;
    std::vector<Overlap> overlaps; // update()'s scratch
};

// Finds again the bounds of the groups an edit may have shrunk. Once for all the edits of a frame, whoever reads first.
void InstanceBatch3D::resolve_bounds() const {
    Data& d = *_data;
    if (!d.any_stale.load(std::memory_order_acquire))
        return;
    const std::lock_guard lock{d.resolving};
    if (!d.any_stale.load(std::memory_order_relaxed))
        return;
    for (const u32 g : d.stale_groups) {
        InstanceGroup3D& group = d.groups[g];
        group.world_bounds =
            d.bad[g] ? no_bounds
                     : instance_bounds(d.mesh,
                                       std::span<const MeshInstance3D>(d.instances).subspan(group.first, group.count));
        d.stale[g] = 0;
    }
    d.stale_groups.clear();
    d.any_stale.store(false, std::memory_order_release);
}

InstanceBatch3D::InstanceBatch3D(Mesh3D mesh, std::vector<MeshInstance3D> instances,
                                 std::vector<InstanceGroup3D> groups) {
    auto data = std::make_shared<Data>();
    const auto reject = [&](std::string_view why) {
        fail3d(data->errors, "InstanceBatch3D", why);
        _data = data;
    };
    if (instances.size() > std::numeric_limits<u32>::max()) {
        reject("instance batch too large");
        return;
    }
    u32 end = 0;
    for (auto& group : groups) {
        if (!group.count || group.first < end || group.first > instances.size() ||
            group.count > instances.size() - group.first) {
            reject("instance batch groups must be ordered, nonempty and disjoint");
            return;
        }
        end = group.first + group.count;
    }
    static std::atomic<u64> next{1};
    data->mesh = std::move(mesh);
    data->instances = std::move(instances);
    data->groups = std::move(groups);
    data->id = next.fetch_add(1);
    data->bad.assign(data->groups.size(), 0);
    data->stale.assign(data->groups.size(), 0);
    const Vec3f centre = data->mesh.bounds().center();
    const f32 radius = data->mesh.bounds().radius();
    for (std::size_t g = 0; g < data->groups.size(); ++g) {
        InstanceGroup3D& group = data->groups[g];
        for (u32 i = group.first; i < group.first + group.count; ++i) {
            InstanceBox box;
            data->bad[g] += !instance_box(centre, radius, data->instances[i].transform, box);
        }
        group.world_bounds =
            data->bad[g]
                ? no_bounds
                : instance_bounds(data->mesh,
                                  std::span<const MeshInstance3D>(data->instances).subspan(group.first, group.count));
    }
    _data = std::move(data);
}
InstanceBatch3D::operator bool() const {
    return _data && _data->id != 0;
}
const Diagnostics3D& InstanceBatch3D::diagnostics() const {
    static const Diagnostics3D empty;
    return _data ? _data->errors : empty;
}
const Mesh3D& InstanceBatch3D::mesh() const {
    static const Mesh3D empty;
    return _data ? _data->mesh : empty;
}
std::span<const MeshInstance3D> InstanceBatch3D::instances() const {
    return _data ? std::span<const MeshInstance3D>(_data->instances) : std::span<const MeshInstance3D>{};
}
std::span<const InstanceGroup3D> InstanceBatch3D::groups() const {
    if (!_data)
        return {};
    resolve_bounds();
    return _data->groups;
}
u64 InstanceBatch3D::identity() const {
    return _data ? _data->id : 0;
}
u64 InstanceBatch3D::revision() const {
    return _data ? _data->revision : 0;
}
u64 InstanceBatch3D::layout_revision() const {
    return _data ? _data->layout_revision : 0;
}
InstanceBatch3D::ChangedRange InstanceBatch3D::changed_since(u64 revision) const {
    if (!_data || revision == _data->revision)
        return {};
    const Data& d = *_data;
    const u32 count = static_cast<u32>(d.instances.size());
    if (!revision || revision > d.revision)
        return {0, count};
    u32 first = count, end = 0;
    if (d.revision - revision <= d.changes.size()) {
        for (u64 i = 0, edits = d.revision - revision; i < edits; ++i) {
            const auto& range = d.changes[(d.revision - i) % d.changes.size()];
            first = std::min(first, range.first);
            end = std::max(end, range.first + range.count);
        }
    } else {
        // Further back than the exact history: the blocks edited since, from the first to the last.
        const std::size_t blocks = d.block_edit.size();
        std::size_t lo = 0, hi = blocks;
        while (lo < hi && d.block_edit[lo] <= revision)
            ++lo;
        while (hi > lo && d.block_edit[hi - 1] <= revision)
            --hi;
        if (lo == hi)
            return {};
        first = static_cast<u32>(lo * Data::block);
        end = static_cast<u32>(std::min<std::size_t>(hi * Data::block, count));
    }
    return {first, end - first};
}
void InstanceBatch3D::changed_ranges_since(u64 revision, std::vector<ChangedRange>& out) const {
    if (!_data || revision == _data->revision)
        return;
    const Data& d = *_data;
    const u32 count = static_cast<u32>(d.instances.size());
    if (!revision || revision > d.revision) {
        out.push_back({0, count});
        return;
    }
    const std::size_t at = out.size();
    if (d.revision - revision <= d.changes.size()) {
        for (u64 i = 0, edits = d.revision - revision; i < edits; ++i)
            out.push_back(d.changes[(d.revision - i) % d.changes.size()]);
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(at), out.end(),
                  [](const ChangedRange& a, const ChangedRange& b) { return a.first < b.first; });
        std::size_t kept = at;
        for (std::size_t i = at; i < out.size(); ++i) {
            if (kept > at && out[i].first <= out[kept - 1].first + out[kept - 1].count)
                out[kept - 1].count = std::max(out[kept - 1].count, out[i].first + out[i].count - out[kept - 1].first);
            else
                out[kept++] = out[i];
        }
        out.resize(kept);
        return;
    }
    for (std::size_t b = 0; b < d.block_edit.size(); ++b)
        if (d.block_edit[b] > revision) {
            const u32 first = static_cast<u32>(b * Data::block);
            const u32 end = static_cast<u32>(std::min<std::size_t>((b + 1) * Data::block, count));
            if (out.size() > at && out.back().first + out.back().count == first)
                out.back().count = end - out.back().first;
            else
                out.push_back({first, end - first});
        }
}
bool InstanceBatch3D::update(u32 first, std::span<const MeshInstance3D> instances) {
    if (!_data)
        _data = std::make_shared<Data>();
    _data->errors.clear();
    if (!_data->id || first > _data->instances.size() || instances.size() > _data->instances.size() - first)
        return fail3d(_data->errors, "InstanceBatch3D::update", "instance batch update outside fixed records");
    if (instances.empty())
        return true;
    Data& d = *_data;
    const u32 end = first + static_cast<u32>(instances.size());
    const Vec3f centre = d.mesh.bounds().center();
    const f32 radius = d.mesh.bounds().radius();
    // Before the records are overwritten: for each group the edit reaches, how many of the old copies had no box, and
    // which faces of the group's bounds one of them stands on (taking it away could pull that face in; a copy strictly
    // inside cannot).
    d.overlaps.clear();
    const auto reached = std::partition_point(d.groups.begin(), d.groups.end(),
                                              [&](const InstanceGroup3D& g) { return g.first + g.count <= first; });
    for (auto group = reached; group != d.groups.end() && group->first < end; ++group) {
        const u32 g = static_cast<u32>(group - d.groups.begin());
        const u32 begin = std::max(first, group->first), stop = std::min(end, group->first + group->count);
        const Bounds3D& b = group->world_bounds;
        const bool whole =
            begin == group->first && stop == group->first + group->count; // nothing of the old copies is kept
        const bool tracked = !d.bad[g] && !d.stale[g];
        u32 old_bad = 0, faces = 0; // bits: min x, y, z, max x, y, z
        for (u32 i = begin; i < stop && !whole; ++i) {
            InstanceBox box;
            if (!instance_box(centre, radius, d.instances[i].transform, box)) {
                ++old_bad;
                continue;
            }
            if (tracked)
                faces |= (box.lo.x <= b.min.x) | (box.lo.y <= b.min.y) << 1 | (box.lo.z <= b.min.z) << 2 |
                         (box.hi.x >= b.max.x) << 3 | (box.hi.y >= b.max.y) << 4 | (box.hi.z >= b.max.z) << 5;
        }
        d.overlaps.push_back({g, begin, stop, old_bad, faces, whole});
    }
    // Copy via memmove so a range from this batch may also be used as the source.
    std::memmove(d.instances.data() + first, instances.data(), instances.size_bytes());
    bool layout_changed = false;
    for (const Data::Overlap& o : d.overlaps) {
        InstanceGroup3D& group = d.groups[o.group];
        const bool was_valid = !d.bad[o.group];
        Bounds3D grown{
            {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()},
            {std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(),
             std::numeric_limits<f32>::lowest()}};
        u32 new_bad = 0;
        for (u32 i = o.begin; i < o.end; ++i) {
            InstanceBox box;
            if (!instance_box(centre, radius, d.instances[i].transform, box)) {
                ++new_bad;
                continue;
            }
            grown.min = {std::min(grown.min.x, box.lo.x), std::min(grown.min.y, box.lo.y),
                         std::min(grown.min.z, box.lo.z)};
            grown.max = {std::max(grown.max.x, box.hi.x), std::max(grown.max.y, box.hi.y),
                         std::max(grown.max.z, box.hi.z)};
        }
        d.bad[o.group] = o.whole ? new_bad : d.bad[o.group] - o.old_bad + new_bad;
        const bool is_valid = !d.bad[o.group];
        layout_changed = layout_changed || was_valid != is_valid;
        bool make_stale = false;
        if (!is_valid)
            group.world_bounds = no_bounds;
        else if (o.whole)
            group.world_bounds = grown; // the group is all new copies: their box is its bounds
        else if (!was_valid || d.stale[o.group])
            make_stale = true;
        else if (const Bounds3D& b = group.world_bounds;
                 ((o.faces & 1) && grown.min.x > b.min.x) || ((o.faces & 2) && grown.min.y > b.min.y) ||
                 ((o.faces & 4) && grown.min.z > b.min.z) || ((o.faces & 8) && grown.max.x < b.max.x) ||
                 ((o.faces & 16) && grown.max.y < b.max.y) || ((o.faces & 32) && grown.max.z < b.max.z))
            make_stale = true; // a copy that set a face has moved in from it, and another may or may not stand there:
                               // measured when read
        else { // every face is set by a copy the edit did not take, or by one it put there: the new boxes can only
               // widen the bounds
            group.world_bounds.min = {std::min(group.world_bounds.min.x, grown.min.x),
                                      std::min(group.world_bounds.min.y, grown.min.y),
                                      std::min(group.world_bounds.min.z, grown.min.z)};
            group.world_bounds.max = {std::max(group.world_bounds.max.x, grown.max.x),
                                      std::max(group.world_bounds.max.y, grown.max.y),
                                      std::max(group.world_bounds.max.z, grown.max.z)};
        }
        if (make_stale && !d.stale[o.group]) {
            d.stale[o.group] = 1;
            d.stale_groups.push_back(o.group);
            d.any_stale.store(true, std::memory_order_release);
        }
    }
    if (layout_changed)
        ++d.layout_revision;
    ++d.revision;
    d.changes[d.revision % d.changes.size()] = {first, static_cast<u32>(instances.size())};
    if (d.block_edit.empty())
        d.block_edit.assign((d.instances.size() + Data::block - 1) / Data::block, 0);
    std::fill(d.block_edit.begin() + first / Data::block, d.block_edit.begin() + (end - 1) / Data::block + 1,
              d.revision);
    return true;
}
Scene3D::DrawStamp::DrawStamp() {
    // Once per scene/move, never once per submitted draw. Address reuse cannot revive a layout.
    static std::atomic<u64> next{1};
    identity = next.fetch_add(1, std::memory_order_relaxed);
}
Scene3D::DrawStamp::DrawStamp(DrawStamp&& other) noexcept : DrawStamp() {
    ++other.revision;
}
Scene3D::DrawStamp& Scene3D::DrawStamp::operator=(DrawStamp&& other) noexcept {
    DrawStamp fresh;
    identity = fresh.identity;
    revision = 0;
    ++other.revision;
    return *this;
}

bool Scene3D::draw_instanced(const InstanceBatch3D& batch, const Material3D& material) {
    if (!_errors.empty())
        return false;
    if (!batch) {
        if (!batch.diagnostics().empty())
            return fail_submission(batch.diagnostics().front());
        return true;
    }
    if (batch.instances().empty())
        return true;
    touch_draws();
    Draw draw{batch.mesh(), material, 0, static_cast<u32>(batch.instances().size())};
    draw.batch = batch;
    _draws.push_back(std::move(draw));
    return _errors.empty();
}

std::span<MeshInstance3D> Scene3D::add_instances(const Mesh3D& mesh, std::size_t count, const Material3D& material) {
    return add_instances(mesh, count, material, false, 0);
}

std::span<MeshInstance3D> Scene3D::add_instances(const Mesh3D& mesh, std::size_t count, const Material3D& material,
                                                 bool skinned, u32 skin_first) {
    if (!_errors.empty())
        return {};
    if (count == 0)
        return {};
    if (count > std::numeric_limits<u32>::max() || _instances.size() > std::numeric_limits<u32>::max() - count) {
        fail_submission("instance count exceeds the draw index range");
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

void Scene3D::add_skinned_copy(const Mesh3D& mesh, u32 first_joint, const Mat4f& transform, const Material3D& material,
                               Color tint, u32 id) {
    const u32 first_copy = static_cast<u32>(_skin_offsets.size()); // a draw of its own starts here
    _skin_offsets.push_back(first_joint);
    add_instances(mesh, 1, material, true, first_copy)[0] = {transform, tint, id};
}

bool Scene3D::draw_skinned(const Mesh3D& mesh, std::span<const Mat4f> joint_matrices, const Mat4f& transform,
                           const Material3D& material, Color tint, u32 id) {
    if (!_errors.empty())
        return false;
    if (!mesh.skinned()) {
        return draw(mesh, transform, material, tint, id);
    }
    if (joint_matrices.size() < mesh.skin_joint_count()) {
        return fail_submission("draw_skinned needs at least the mesh's joint matrix count");
    }
    if (_joints.size() > std::numeric_limits<u32>::max() - mesh.skin_joint_count() ||
        _instances.size() >= std::numeric_limits<u32>::max() || _skin_offsets.size() >= std::numeric_limits<u32>::max())
        return fail_submission("skinned submission exceeds the draw index range");
    // Only the joints the mesh uses are kept: a skeleton may have more joints than the mesh (it is the mesh's, as the
    // skin).
    const std::size_t keep = mesh.skin_joint_count();
    const u32 first_joint = static_cast<u32>(_joints.size());
    _joints.insert(_joints.end(), joint_matrices.begin(), joint_matrices.begin() + static_cast<std::ptrdiff_t>(keep));
    add_skinned_copy(mesh, first_joint, transform, material, tint, id);
    return _errors.empty();
}

bool Scene3D::draw_skinned(const Model3D& model, std::span<const Mat4f> joint_matrices, const Mat4f& transform,
                           Color tint, u32 id) {
    if (!_errors.empty())
        return false;
    draw_skinned(model, joint_matrices, transform, tint, id, {});
    return _errors.empty();
}
bool Scene3D::draw_skinned(const Model3D& model, std::span<const Mat4f> joint_matrices, const Mat4f& transform,
                           Color tint, u32 id, std::span<const Material3D> materials) {
    if (!_errors.empty())
        return false;
    draw_skinned(model, joint_matrices, transform, tint, id, materials, {});
    return _errors.empty();
}
bool Scene3D::draw(const Model3D& model, const Mat4f& transform, std::span<const Mat4f> node_matrices, Color tint,
                   u32 id) {
    if (!_errors.empty())
        return false;
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        const ModelPart3D& part = model.parts[i];
        draw(part.mesh, transform * node_posed(model, i, node_matrices), part.material, tint, id);
    }
    return _errors.empty();
}
bool Scene3D::draw_skinned(const Model3D& model, std::span<const Mat4f> joint_matrices, const Mat4f& transform,
                           Color tint, u32 id, std::span<const Material3D> materials,
                           std::span<const Mat4f> node_matrices) {
    if (!_errors.empty())
        return false;
    if (!materials.empty() && materials.size() != model.parts.size())
        return fail_submission("material count must match model parts");
    // One range for the pose, as many matrices as the most any part uses (a part uses the first of them: its joints are
    // the skeleton's, in the skeleton's order).
    std::size_t need = 0;
    for (const ModelPart3D& part : model.parts) {
        if (part.mesh.skinned() && part.skin <= 0) { // the first skeleton's: the pose given is for it
            need = std::max<std::size_t>(need, part.mesh.skin_joint_count());
        }
    }
    if (model.parts.size() > std::numeric_limits<u32>::max() ||
        _instances.size() > std::numeric_limits<u32>::max() - model.parts.size() ||
        _skin_offsets.size() > std::numeric_limits<u32>::max() - model.parts.size() ||
        _joints.size() > std::numeric_limits<u32>::max() - need)
        return fail_submission("model submission exceeds the draw index range");
    if (joint_matrices.size() < need)
        return fail_submission("draw_skinned needs at least the model's joint matrix count");
    u32 first_joint = 0;
    bool copied = false;
    for (size_t part_index = 0; part_index < model.parts.size(); ++part_index) {
        const auto& part = model.parts[part_index];
        const auto& material = materials.empty() ? part.material : materials[part_index];
        if (part.mesh.skinned()) {
            if (part.skin > 0) {
                warn_unposed_skin(model, part);
                continue;
            }
            if (!copied) {
                copied = true;
                first_joint = static_cast<u32>(_joints.size());
                _joints.insert(_joints.end(), joint_matrices.begin(),
                               joint_matrices.begin() + static_cast<std::ptrdiff_t>(need));
            }
            add_skinned_copy(part.mesh, first_joint, transform, material, tint, id);
        } else {
            draw(part.mesh,
                 transform * (node_matrices.empty() ? part.transform : node_posed(model, part_index, node_matrices)),
                 material, tint, id);
        }
    }
    return _errors.empty();
}

bool Scene3D::draw(const Mesh3D& mesh, const Mat4f& transform, const Material3D& material, Color tint, u32 id) {
    if (!_errors.empty())
        return false;
    const auto room = add_instances(mesh, 1, material);
    if (room.empty())
        return false;
    room[0] = {transform, tint, id};
    return _errors.empty();
}

bool Scene3D::draw_instanced(const Mesh3D& mesh, std::span<const MeshInstance3D> instances,
                             const Material3D& material) {
    if (!_errors.empty())
        return false;
    if (instances.empty()) {
        return true;
    }
    const std::span<MeshInstance3D> room = add_instances(mesh, instances.size(), material);
    if (room.size() != instances.size())
        return false;
    std::copy(instances.begin(), instances.end(), room.begin());
    return _errors.empty();
}

bool Scene3D::draw_instanced(const Mesh3D& mesh, std::span<const Mat4f> transforms, const Material3D& material) {
    if (!_errors.empty())
        return false;
    if (transforms.empty()) {
        return true;
    }
    const std::span<MeshInstance3D> room = add_instances(mesh, transforms.size(), material);
    if (room.size() != transforms.size())
        return false;
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        room[i] = {transforms[i], colors::white, 0};
    }
    return _errors.empty();
}

Scene3D& Scene3D::overlay() {
    if (!_overlay) {
        _overlay = std::make_unique<Scene3D>();
    }
    return *_overlay;
}

void Scene3D::add_group(u32 first, u32 count, const Bounds3D& bounds) {
    if (!count || !valid_bounds(bounds))
        return;
    Draw& draw = _draws.back();
    if (!draw.group_count)
        draw.group_first = static_cast<u32>(_instance_groups.size());
    _instance_groups.push_back({first - draw.first, count, bounds});
    ++draw.group_count;
}
bool Scene3D::draw_instanced_group(const Mesh3D& mesh, std::span<const Mat4f> transforms, const Bounds3D& bounds,
                                   const Material3D& material) {
    if (!_errors.empty())
        return false;
    const u32 first = static_cast<u32>(_instances.size());
    if (!draw_instanced(mesh, transforms, material))
        return false;
    add_group(first, static_cast<u32>(transforms.size()), bounds);
    return _errors.empty();
}
bool Scene3D::draw_instanced_group(const Mesh3D& mesh, std::span<const MeshInstance3D> instances,
                                   const Bounds3D& bounds, const Material3D& material) {
    if (!_errors.empty())
        return false;
    const u32 first = static_cast<u32>(_instances.size());
    if (!draw_instanced(mesh, instances, material))
        return false;
    add_group(first, static_cast<u32>(instances.size()), bounds);
    return _errors.empty();
}

bool Scene3D::draw_particles(const ParticleLook3D& look, std::span<const ParticleSprite3D> sprites) {
    if (!_errors.empty())
        return false;
    if (sprites.empty()) {
        return true;
    }
    if (sprites.size() > std::numeric_limits<u32>::max() ||
        _particle_sprites.size() > std::numeric_limits<u32>::max() - sprites.size())
        return fail_submission("particle count exceeds the draw index range");
    const u32 first = static_cast<u32>(_particle_sprites.size());
    _particle_sprites.insert(_particle_sprites.end(), sprites.begin(), sprites.end());
    _particle_draws.push_back({look, first, static_cast<u32>(sprites.size())});
    return _errors.empty();
}

bool Scene3D::draw_particles(const ParticleSystem3D& system) {
    if (!_errors.empty())
        return false;
    const u32 first = static_cast<u32>(_particle_sprites.size());
    system.sprites(_particle_sprites);
    const u32 count = static_cast<u32>(_particle_sprites.size()) - first;
    if (count > 0) {
        _particle_draws.push_back({system.desc.look, first, count});
    }
    return _errors.empty();
}

bool Scene3D::update_joint_matrices(u32 first, std::span<const Mat4f> matrices) {
    if (!_errors.empty())
        return false;
    if (first > _joints.size() || matrices.size() > _joints.size() - first)
        return fail_submission("joint matrix update outside submitted poses");
    if (!matrices.empty())
        std::memmove(_joints.data() + first, matrices.data(), matrices.size_bytes());
    return _errors.empty();
}

void Scene3D::clear() {
    _errors.clear();
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
