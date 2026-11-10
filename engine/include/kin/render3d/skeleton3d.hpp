#pragma once
#include <kin/render3d/errors3d.hpp>

// Skeletons and poses for kin's experimental 3D (docs/3d_plan.md, P1a): the joints a skinned mesh is bound to, a pose
// (the local placing of each joint), and the matrices the vertex shader applies. Plain data and maths, no GPU. Clips
// that make poses are in kin/render3d/animation3d.hpp; drawing is Scene3D::draw_skinned (kin/render3d/scene3d.hpp).
//
//     const kin::Skeleton3D& skeleton = model.skeletons[0];
//     kin::Pose3D pose = skeleton.rest_pose();
//     pose.joints[elbow].rotation = kin::Quatf::axis_angle({0, 0, 1}, 60.0f);
//     std::vector<kin::Mat4f> matrices;
//     kin::compute_joint_matrices(skeleton, pose, matrices);
//     scene.draw_skinned(model, matrices, transform);

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// A joint's placing in its parent (glTF's node: scale, then rotate, then move).
struct JointPose3D {
    Vec3f translation{};
    Quatf rotation{};
    Vec3f scale{1.0f, 1.0f, 1.0f};

    Mat4f matrix() const { return Mat4f::trs(translation, rotation, scale); }
};

// The local placing of every joint of a skeleton, by joint index.
struct Pose3D {
    std::vector<JointPose3D> joints;
};

class Skeleton3D;

// How much each joint takes part in something (a layer over a base pose, a blend): 0 none, 1 all. By joint index.
struct PoseMask3D {
    std::vector<f32> weights;

    // Every joint at `weight`.
    static PoseMask3D all(const Skeleton3D& skeleton, f32 weight = 1.0f);
    // `joint` and everything below it at `weight`, the rest at 0 (an arm; an upper body). `include_root` false leaves
    // `joint` itself out and takes only its descendants.
    // Reuses weights storage; identical selection to subtree().
    void set_subtree(const Skeleton3D& skeleton, i32 joint, f32 weight = 1.0f, bool include_root = true);
    static PoseMask3D subtree(const Skeleton3D& skeleton, i32 joint, f32 weight = 1.0f, bool include_root = true);
    // The named joints (not their children) at `weight`, the rest at 0. A name that is not a joint is ignored.
    static PoseMask3D joints(const Skeleton3D& skeleton, std::span<const std::string_view> names, f32 weight = 1.0f);
    // Weights of `other` added to this (to a most of 1), for a mask of several subtrees.
    PoseMask3D& add(const PoseMask3D& other);
};

// The joints of a skinned mesh: their names, how they hang together, where they rest, and the matrices that take a
// vertex from the mesh's own space (where it was modelled, bound) to the joint's local space.
class Skeleton3D {
public:
    std::vector<std::string> names;
    // The joint above each one, or -1 for a root. Always lower than the joint's own index (a parent comes first), so a
    // pose is turned to matrices in one pass; the glTF loader orders joints so.
    std::vector<i32> parents;
    // The rest pose, by joint: the joints' local placing in the file.
    std::vector<JointPose3D> rest;
    // The matrix that takes a vertex from the mesh's space at rest to the joint's local space: the inverse of the
    // joint's world matrix at rest.
    std::vector<Mat4f> inverse_bind;
    // Where the roots' parent stands in the model: the nodes above the joints (the file's `Armature` node and what it
    // sits in). Identity for most skeletons. Applied to every root's world matrix.
    Mat4f root_transform{};

    u32 joint_count() const { return static_cast<u32>(parents.size()); }
    // The joint called `name`, or -1.
    i32 find(std::string_view name) const;
    // Whether the arrays agree, parents come first, and there are at most max_skin_joints joints; failure carries
    // contextual diagnostics.
    Status3D validate() const;
    Pose3D rest_pose() const;
    // The first root joint (the one root motion is taken from), or -1 when empty.
    i32 root_joint() const;
    // 1 for `joint` and each of its descendants, else 0.
    std::vector<u8> subtree(i32 joint) const;
    // How many joints are between `joint` and a root (a root is 0).
    u32 depth(i32 joint) const;
};

// Each joint's world matrix (the model's space) for `pose`, a parent before its children.
void compute_world_matrices(const Skeleton3D& skeleton, const Pose3D& pose, std::vector<Mat4f>& out);

// The matrices the vertex shader applies, one a joint: its world matrix in `pose` times its inverse bind matrix. A pose
// that is not the rest pose moves a vertex bound to the joint along with it; at rest they are all the identity (up to
// rounding) when the inverse bind matrices are the inverses of the rest world matrices, as a consistent file's are.
void compute_joint_matrices(const Skeleton3D& skeleton, const Pose3D& pose, std::vector<Mat4f>& out);
// Into room the caller keeps (a frame's matrices written straight into the scene's, say): `out` holds joint_count().
void compute_joint_matrices(const Skeleton3D& skeleton, const Pose3D& pose, std::span<Mat4f> out);

// --- bounds for culling ---------------------------------------------------------------------------------------------

// A sphere a joint's vertices stay within: round the centre of the vertices that joint pulls on (a weight above zero),
// as far as the farthest of them, in the mesh's space at rest. Joint matrices move the sphere with the joint, so a
// bound on a posed mesh is the sphere round the moved spheres (skin_sphere): no vertex can leave it, whatever the pose,
// for a vertex is a mix of the places each of its joints would put it, and each of those lies in that joint's moved
// sphere.
struct SkinBound3D {
    Vec3f center{};
    f32 radius = -1.0f; // negative: no vertex uses the joint
};

// A bound for each of `joints` joints, from the skin of `mesh` (none: empty).
std::vector<SkinBound3D> compute_skin_bounds(const MeshData3D& mesh, u32 joints);

// The sphere, in the mesh's space, that holds every vertex of the mesh posed by `matrices` (one a joint, as
// compute_joint_matrices makes them). A joint matrix that squashes or stretches (non-uniform scale) is allowed for: its
// largest stretch is used. False when no joint is used (nothing to bound). Never smaller than the vertices.
bool skin_sphere(std::span<const SkinBound3D> bounds, std::span<const Mat4f> matrices, Vec3f& center, f32& radius);

// Conservative stretch of a matrix's linear part, including shear and mirrors (no eigenvalue solve). Rounded
// outward; zero for a zero linear part, infinity for nonfinite entries or an unrepresentable float bound.
f32 stretch_bound(const Mat4f& m);

// --- skinning on the CPU --------------------------------------------------------------------------------------------

// `mesh` with every vertex moved as the vertex shader moves it: position = sum of weight times (matrix times position);
// normal = normalised (the linear part of the weighted sum of the matrices) times the normal. The second is right for
// rigid and uniformly scaled joints and approximate for a non-uniform scale. `gpu_weights` uses the weights as the GPU
// holds them (quantised to bytes, quantise_skin_weights). The result has no skin: a plain static mesh to compare with.
MeshData3D skin_mesh(const MeshData3D& mesh, std::span<const Mat4f> matrices, bool gpu_weights = true);

} // namespace kin
