#include <algorithm>
#include <cmath>
#include <kin/ecs/render3d.hpp>

namespace kin {
namespace {
bool finite_matrix(const Mat4f& m) {
    for (auto value : m.m)
        if (!std::isfinite(value))
            return false;
    return true;
}
// One ancestor chain, no per-socket copy of the skeleton's pose/matrix vectors.
std::optional<Mat4f> joint_matrix(const Skeleton3D& skeleton, const Pose3D* pose, i32 joint) {
    Mat4f result;
    for (u32 depth = 0; depth < max_skin_joints; ++depth) {
        if (joint < 0)
            return skeleton.root_transform * result;
        const auto index = std::size_t(joint);
        if (index >= skeleton.rest.size() || index >= skeleton.parents.size() || (pose && index >= pose->joints.size()))
            return {};
        result = (pose ? pose->joints[index] : skeleton.rest[index]).matrix() * result;
        joint = skeleton.parents[index];
    }
    // A valid maximum-depth chain may have consumed exactly max_skin_joints.
    return joint < 0 ? std::optional<Mat4f>{skeleton.root_transform * result} : std::nullopt;
}
} // namespace
bool WorldRenderState3D::attachment_pose_prepared(EcsId entity) const {
    return std::find(_attachment_poses.begin(), _attachment_poses.begin() + _attachment_pose_count, entity) !=
           _attachment_poses.begin() + _attachment_pose_count;
}
void WorldRenderState3D::prepare_joint_attachments() {
    ++_attachment_frame;
    _attachment_pose_count = 0;
    _active_attachments = 0;
    std::size_t processed = 0;
    _attachments.each([&](flecs::entity root, JointAttachment3D& attachment) {
        using Status = JointAttachmentStatus3D;
        attachment._frame = _attachment_frame;
        attachment._status = Status::Unbound;
        if (!attachment.enabled) {
            attachment._status = Status::Disabled;
            return;
        }
        ++_active_attachments;
        if (processed++ >= max_joint_attachments3d) {
            attachment._status = Status::LimitExceeded;
            return;
        }
        const auto* local = root.get<Transform3D>();
        if (!local) {
            attachment._status = Status::InvalidTransform;
            return;
        }
        auto target = _ecs ? attachment._target.resolve(*_ecs) : EcsEntity{};
        if (!target) {
            attachment._status = Status::MissingTarget;
            return;
        }
        auto ancestor = target.raw();
        for (std::size_t depth = 0; ancestor; ++depth, ancestor = ancestor.parent()) {
            if (depth >= 64 || (ancestor.owns<JointAttachment3D>() && ancestor.get<JointAttachment3D>()->enabled)) {
                attachment._status = Status::UnsupportedTarget;
                return;
            }
            if (!ancestor.enabled()) {
                attachment._status = Status::HiddenTarget;
                return;
            }
        }
        const auto* renderer = target.get<SkinnedMeshRenderer3D>();
        const auto* world = target.get<WorldTransform3D>();
        if (!renderer || !renderer->model || renderer->model->skeletons.empty() || !world) {
            attachment._status = Status::UnsupportedTarget;
            return;
        }
        if (!renderer->visible) {
            attachment._status = Status::HiddenTarget;
            return;
        }
        const auto& model = renderer->model;
        const auto& skeleton = model->skeletons[0];
        if (attachment._model.lock() != model) {
            attachment._model = model;
            attachment._index = skeleton.validate() ? skeleton.find(attachment.joint()) : -2;
        }
        if (attachment._index == -2) {
            attachment._status = Status::UnsupportedTarget;
            return;
        }
        if (attachment._index < 0) {
            attachment._status = Status::MissingJoint;
            return;
        }
        Animator3D* animator = nullptr;
        if (target.has<Animator3D>()) {
            if (!target.raw().owns<Animator3D>()) {
                attachment._status = Status::UnsupportedTarget;
                return;
            }
            animator = target.raw().get_mut<Animator3D>();
        }
        const bool playing = animator && animator->bound_to(model) && animator->player.skeleton() == &skeleton;
        const bool solved = playing && (!animator->ik_constraints.empty() || !animator->pose_override.empty());
        std::optional<Mat4f> joint;
        if (solved) {
            if (!attachment_pose_prepared(target.id())) {
                solve_pose(*animator, skeleton, animator->player.pose(), world->matrix, target.raw().world(), _epoch);
                _attachment_poses[_attachment_pose_count++] = target.id();
            }
            if (std::size_t(attachment._index) < animator->ik_world.size())
                joint = animator->ik_world[std::size_t(attachment._index)];
        } else
            joint = joint_matrix(skeleton, playing ? &animator->player.pose() : nullptr, attachment._index);
        if (!joint) {
            attachment._status = Status::InvalidTransform;
            return;
        }
        const auto* presentation = target.get<RenderTransform3D>();
        const auto root_placement = presentation && presentation->enabled ? presentation->matrix
                                    : _presentation && *_presentation
                                        ? (*_presentation)(target.raw()).value_or(world->matrix)
                                        : world->matrix;
        const auto placement = root_placement * *joint * attachment._offset * local->matrix();
        if (!finite_matrix(placement)) {
            attachment._status = Status::InvalidTransform;
            return;
        }
        attachment._placement = placement;
        attachment._status = Status::Ready;
    });
}
std::optional<Mat4f> WorldRenderState3D::draw_placement(flecs::entity entity, const WorldTransform3D& world,
                                                        const RenderTransform3D* presentation) const {
    if (_active_attachments) {
        auto root = entity;
        for (std::size_t depth = 0; root; ++depth, root = root.parent()) {
            if (depth >= 64)
                return {}; // bounded traversal, also protects malformed graphs
            if (!root.owns<JointAttachment3D>())
                continue;
            const auto* attachment = root.get<JointAttachment3D>();
            if (!attachment->enabled)
                continue;
            if (attachment->_frame != _attachment_frame || attachment->_status != JointAttachmentStatus3D::Ready)
                return {};
            Mat4f relative;
            for (auto child = entity; child != root; child = child.parent()) {
                if (const auto* transform = child.get<Transform3D>())
                    relative = transform->matrix() * relative;
            }
            const auto result = attachment->_placement * relative;
            return finite_matrix(result) ? std::optional<Mat4f>{result} : std::nullopt;
        }
    }
    if (presentation && presentation->enabled)
        return presentation->matrix;
    return _presentation && *_presentation ? (*_presentation)(entity).value_or(world.matrix) : world.matrix;
}
} // namespace kin
