#pragma once

#include <array>
#include <kin/core/math3d.hpp>
#include <kin/scene/scene_entity3d.hpp>
#include <memory>
#include <optional>
#include <string_view>

namespace kin {
class Model3D;
class WorldRenderState3D;
enum class JointAttachmentStatus3D : u8 {
    Unbound,
    Ready,
    Disabled,
    MissingTarget,
    HiddenTarget,
    MissingJoint,
    UnsupportedTarget,
    InvalidTransform,
    LimitExceeded
};
inline constexpr std::size_t max_joint_attachments3d = 64;
inline constexpr std::size_t max_attachment_joint_name3d = 95;
// Native, presentation-only binding on a prefab root. The root's Transform3D
// is relative to offset * joint; descendants keep their own local transforms.
// Meshes/models/lights follow. Simulation, hierarchy and physics never move.
// Set on each actual instance; this runtime component is not an authored schema.
class JointAttachment3D {
public:
    JointAttachment3D() = default;
    JointAttachment3D(SceneEntity3D target, std::string_view joint, Mat4f offset = {});
    bool configure(SceneEntity3D target, std::string_view joint, Mat4f offset = {});
    SceneEntity3D target() const { return _target; }
    std::string_view joint() const { return {_joint.data(), _joint_length}; }
    const Mat4f& offset() const { return _offset; }
    bool enabled = true; // false: ordinary world placement; removing detaches too
    JointAttachmentStatus3D status() const { return _status; }
    // Snapshot from the last collect, including the prefab root's local transform.
    std::optional<Mat4f> placement() const {
        return _status == JointAttachmentStatus3D::Ready ? std::optional<Mat4f>{_placement} : std::nullopt;
    }

private:
    SceneEntity3D _target;
    std::array<char, max_attachment_joint_name3d + 1> _joint{};
    std::size_t _joint_length = 0;
    Mat4f _offset{}, _placement{};
    std::weak_ptr<const Model3D> _model; // cache identity only, never retains art
    i32 _index = -1;
    u64 _frame = 0;
    JointAttachmentStatus3D _status = JointAttachmentStatus3D::Unbound;
    friend class WorldRenderState3D;
};
} // namespace kin
