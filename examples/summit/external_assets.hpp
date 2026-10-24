#pragma once

#include "props.hpp"

#include <array>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/model3d.hpp>
#include <vector>

namespace summit {

// A small, fixed selection of authored props. Models are resident once and reused across camps.
class ExternalAssets {
public:
    using Kind = PropKind;
    using enum PropKind;
    ExternalAssets(kin::Assets3D& assets, const SummitLevel& level, bool enabled);
    // `span` is the longest dimension in metres; the model's bottom sits at `feet`.
    bool draw(kin::Scene3D& scene, Kind kind, kin::Vec3f feet, float span, float yaw = 0) const;
    void draw_scenery(kin::Scene3D& scene) const;
    u32 scenery_instances() const { return _scenery_instances; }
    u64 scenery_triangles() const { return _scenery_triangles; }
    u64 instance_record_bytes() const { return _instance_record_bytes; }
    u32 loaded_models() const {
        u32 count = 0;
        for (const auto& model : _models)
            if (model)
                ++count;
        return count;
    }

private:
    std::array<kin::AssetHandle<kin::Model3D>, PropCount> _models;
    std::vector<kin::ModelBatch3D> _batches;
    u32 _scenery_instances = 0;
    u64 _scenery_triangles = 0, _instance_record_bytes = 0;
};

} // namespace summit
