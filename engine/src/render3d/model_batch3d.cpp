#include "history3d.hpp"

#include <algorithm>
#include <cmath>
#include <kin/render3d/model3d.hpp>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace kin {
namespace {
bool finite(const Mat4f& m) {
    return std::all_of(std::begin(m.m), std::end(m.m), [](f32 v) { return std::isfinite(v); });
}
} // namespace

Result3D<Mat4f> Model3D::placement(const ModelPlacement3D& options) const {
    if (!std::isfinite(options.size) || options.size < 0)
        return {std::nullopt, {"Model3D::placement: Model3D placement size must be finite and non-negative"}};
    if (options.pivot != ModelPivot3D::Origin && options.pivot != ModelPivot3D::Center &&
        options.pivot != ModelPivot3D::BottomCenter)
        return {std::nullopt, {"Model3D::placement: Unknown model placement pivot"}};
    if (options.size > 0 || options.pivot != ModelPivot3D::Origin) {
        for (const auto v : {bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z})
            if (!std::isfinite(v))
                return {std::nullopt, {"Model3D::placement: Model3D bounds must be finite"}};
        if (bounds.max.x < bounds.min.x || bounds.max.y < bounds.min.y || bounds.max.z < bounds.min.z)
            return {std::nullopt, {"Model3D::placement: Model3D bounds must be ordered"}};
    }
    Vec3f pivot{};
    if (options.pivot == ModelPivot3D::Center)
        pivot = bounds.center();
    else if (options.pivot == ModelPivot3D::BottomCenter)
        pivot = {bounds.center().x, bounds.min.y, bounds.center().z};
    f32 fit = 1;
    if (options.size > 0) {
        const Vec3f size = bounds.size();
        const f32 extent = std::max({size.x, size.y, size.z});
        if (!std::isfinite(extent) || extent <= 0)
            return {std::nullopt, {"Model3D::placement: Cannot fit a model with non-finite or degenerate bounds"}};
        fit = options.size / extent;
    }
    const Mat4f result =
        Mat4f::trs(options.position, options.rotation, options.scale * fit) * Mat4f::translation(-pivot);
    if (!finite(result))
        return {std::nullopt, {"Model3D::placement: Model3D placement must produce a finite transform"}};
    return result3d(result);
}

Result3D<ModelBatch3D> ModelBatch3D::create(const Model3D& model, std::span<const MeshInstance3D> instances,
                                            const ModelBatchOptions3D& options) {
    ModelBatch3D value(model, instances, options);
    if (!value.diagnostics().empty())
        return {std::nullopt, value.diagnostics()};
    return result3d(std::move(value));
}

ModelBatch3D::ModelBatch3D(const Model3D& model, std::span<const MeshInstance3D> instances,
                           const ModelBatchOptions3D& options) {
    const auto reject = [&](std::string_view reason) {
        _parts.clear();
        _instances = 0;
        _triangles = 0;
        _record_bytes = 0;
        fail3d(_errors, "ModelBatch3D", reason);
    };
    if (!std::isfinite(options.cell_size) || options.cell_size < 0) {
        reject("Model3D batch cell size must be finite and non-negative");
        return;
    }
    if (instances.size() > std::numeric_limits<u32>::max()) {
        reject("Too many model instances");
        return;
    }
    for (const auto& part : model.parts) {
        if (part.skin >= 0 || part.mesh.skinned()) {
            reject("ModelBatch3D requires a static model");
            return;
        }
        if (!finite(part.transform)) {
            reject("Model3D part transform must be finite");
            return;
        }
    }
    for (const auto& copy : instances)
        if (!finite(copy.transform)) {
            reject("Model3D instance transform must be finite");
            return;
        }
    if (instances.empty() || !model)
        return;
    std::vector<u32> order;
    std::vector<InstanceGroup3D> groups;
    order.reserve(instances.size());
    if (options.cell_size > 0) {
        // Double keys avoid undefined integer casts for huge finite coordinates
        // or tiny cell sizes. Bounds are recomputed from the actual mesh copies.
        using Cell = std::tuple<double, double, double>;
        std::map<Cell, std::vector<u32>> cells;
        for (u32 i = 0; i < instances.size(); ++i) {
            const auto& t = instances[i].transform;
            const auto cell = [&](int k) { return std::floor(double(t.m[k]) / double(options.cell_size)); };
            cells[{cell(12), cell(13), cell(14)}].push_back(i);
        }
        for (const auto& [cell, indices] : cells) {
            (void)cell;
            groups.push_back({u32(order.size()), u32(indices.size())});
            order.insert(order.end(), indices.begin(), indices.end());
        }
    } else {
        order.resize(instances.size());
        std::iota(order.begin(), order.end(), 0u);
    }
    for (const auto& part : model.parts) {
        if (!part.mesh)
            continue;
        std::vector<MeshInstance3D> records;
        records.reserve(instances.size());
        for (u32 index : order) {
            const auto& copy = instances[index];
            const Mat4f transform = copy.transform * part.transform;
            if (!finite(transform)) {
                reject("Model3D part/instance composition must be finite");
                return;
            }
            records.push_back(
                {transform, copy.tint, copy.id, detail::part_history3d(copy.history_id, &part - model.parts.data())});
        }
        _triangles += u64(records.size()) * part.mesh.triangle_count();
        _record_bytes += records.size() * sizeof(MeshInstance3D);
        _parts.push_back({InstanceBatch3D::create(part.mesh, std::move(records), groups).value_or({}),
                          options.material.value_or(part.material)});
    }
    if (!_parts.empty())
        _instances = u32(instances.size());
}

Status3D Scene3D::draw_instanced(const ModelBatch3D& batch) {
    const bool accepted = draw_instanced_operation(batch);
    return status3d(accepted, _errors);
}

bool Scene3D::draw_instanced_operation(const ModelBatch3D& batch) {
    if (!_errors.empty())
        return false;
    if (!batch.diagnostics().empty())
        return fail_submission(batch.diagnostics().front());
    for (const auto& part : batch.parts())
        draw_instanced_operation(part.instances, part.material);
    return _errors.empty();
}

} // namespace kin
