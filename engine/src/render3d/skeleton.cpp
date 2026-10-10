#include <kin/render3d/skeleton.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kin {

// --- PoseMask --------------------------------------------------------------------------------------------------------

PoseMask PoseMask::all(const Skeleton& skeleton, f32 weight) {
    return PoseMask{std::vector<f32>(skeleton.joint_count(), weight)};
}

PoseMask PoseMask::subtree(const Skeleton& skeleton, i32 joint, f32 weight, bool include_root) {
    PoseMask mask{std::vector<f32>(skeleton.joint_count(), 0.0f)};
    if (joint < 0 || joint >= static_cast<i32>(skeleton.joint_count())) {
        return mask;
    }
    const std::vector<u8> below = skeleton.subtree(joint);
    for (std::size_t i = 0; i < below.size(); ++i) {
        mask.weights[i] = below[i] && (include_root || static_cast<i32>(i) != joint) ? weight : 0.0f;
    }
    return mask;
}

PoseMask PoseMask::joints(const Skeleton& skeleton, std::span<const std::string_view> names, f32 weight) {
    PoseMask mask{std::vector<f32>(skeleton.joint_count(), 0.0f)};
    for (const std::string_view name : names) {
        const i32 at = skeleton.find(name);
        if (at >= 0) {
            mask.weights[static_cast<std::size_t>(at)] = weight;
        }
    }
    return mask;
}

PoseMask& PoseMask::add(const PoseMask& other) {
    if (weights.size() < other.weights.size()) {
        weights.resize(other.weights.size(), 0.0f);
    }
    for (std::size_t i = 0; i < other.weights.size(); ++i) {
        weights[i] = std::min(1.0f, weights[i] + other.weights[i]);
    }
    return *this;
}

// --- Skeleton --------------------------------------------------------------------------------------------------------

i32 Skeleton::find(std::string_view name) const {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

bool Skeleton::validate(std::string* error) const {
    const auto fail = [&](std::string why) {
        if (error) {
            *error = std::move(why);
        }
        return false;
    };
    const std::size_t n = parents.size();
    if (n == 0) {
        return fail("no joints");
    }
    if (n > max_skin_joints) {
        return fail(std::to_string(n) + " joints, more than the " + std::to_string(max_skin_joints) + " a skin can have");
    }
    if (rest.size() != n || inverse_bind.size() != n || (!names.empty() && names.size() != n)) {
        return fail("the joints' arrays differ in length");
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (parents[i] >= static_cast<i32>(i)) {
            return fail("joint " + std::to_string(i) + " has a parent that does not come before it");
        }
    }
    return true;
}

Pose Skeleton::rest_pose() const {
    return Pose{rest};
}

i32 Skeleton::root_joint() const {
    for (std::size_t i = 0; i < parents.size(); ++i) {
        if (parents[i] < 0) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

std::vector<u8> Skeleton::subtree(i32 joint) const {
    std::vector<u8> in(parents.size(), 0);
    if (joint < 0 || joint >= static_cast<i32>(parents.size())) {
        return in;
    }
    in[static_cast<std::size_t>(joint)] = 1;
    for (std::size_t i = static_cast<std::size_t>(joint) + 1; i < parents.size(); ++i) {
        const i32 p = parents[i];
        in[i] = p >= 0 && in[static_cast<std::size_t>(p)] ? 1 : 0;
    }
    return in;
}

u32 Skeleton::depth(i32 joint) const {
    u32 d = 0;
    while (joint >= 0 && joint < static_cast<i32>(parents.size()) && parents[static_cast<std::size_t>(joint)] >= 0) {
        joint = parents[static_cast<std::size_t>(joint)];
        ++d;
    }
    return d;
}

void compute_world_matrices(const Skeleton& skeleton, const Pose& pose, std::vector<Mat4f>& out) {
    const std::size_t n = skeleton.joint_count();
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Mat4f local = i < pose.joints.size() ? pose.joints[i].matrix() : skeleton.rest[i].matrix();
        const i32 p = skeleton.parents[i];
        out[i] = (p < 0 ? skeleton.root_transform : out[static_cast<std::size_t>(p)]) * local;
    }
}

void compute_joint_matrices(const Skeleton& skeleton, const Pose& pose, std::span<Mat4f> out) {
    const std::size_t n = std::min<std::size_t>(skeleton.joint_count(), out.size());
    // The world matrices go into `out` first, then each is multiplied by its inverse bind matrix; a parent's world matrix
    // is needed unchanged for its children, so go from the last joint to the first when finishing (parents come first).
    for (std::size_t i = 0; i < n; ++i) {
        const Mat4f local = i < pose.joints.size() ? pose.joints[i].matrix() : skeleton.rest[i].matrix();
        const i32 p = skeleton.parents[i];
        out[i] = (p < 0 ? skeleton.root_transform : out[static_cast<std::size_t>(p)]) * local;
    }
    for (std::size_t i = n; i-- > 0;) {
        out[i] = out[i] * skeleton.inverse_bind[i];
    }
}

void compute_joint_matrices(const Skeleton& skeleton, const Pose& pose, std::vector<Mat4f>& out) {
    out.resize(skeleton.joint_count());
    compute_joint_matrices(skeleton, pose, std::span<Mat4f>{out});
}

// --- bounds ----------------------------------------------------------------------------------------------------------

std::vector<SkinBound> compute_skin_bounds(const MeshData& mesh, u32 joints) {
    std::vector<SkinBound> bounds(joints);
    if (mesh.skin.size() != mesh.vertices.size() || joints == 0) {
        return bounds;
    }
    // The box of the vertices each joint pulls on gives the centre; the farthest of them the radius.
    std::vector<Vec3f> lo(joints, Vec3f{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()});
    std::vector<Vec3f> hi(joints, Vec3f{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max()});
    const auto uses = [&](const MeshSkinVertex& s, std::size_t k) { return s.weights[k] > 0.0f && s.joints[k] < joints; };
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const Vec3f p = mesh.vertices[v].position;
        for (std::size_t k = 0; k < 4; ++k) {
            if (!uses(mesh.skin[v], k)) {
                continue;
            }
            const std::size_t j = mesh.skin[v].joints[k];
            lo[j] = {std::min(lo[j].x, p.x), std::min(lo[j].y, p.y), std::min(lo[j].z, p.z)};
            hi[j] = {std::max(hi[j].x, p.x), std::max(hi[j].y, p.y), std::max(hi[j].z, p.z)};
        }
    }
    for (u32 j = 0; j < joints; ++j) {
        if (lo[j].x <= hi[j].x) {
            bounds[j].center = (lo[j] + hi[j]) * 0.5f;
            bounds[j].radius = 0.0f;
        }
    }
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        for (std::size_t k = 0; k < 4; ++k) {
            if (uses(mesh.skin[v], k)) {
                SkinBound& b = bounds[mesh.skin[v].joints[k]];
                b.radius = std::max(b.radius, length(mesh.vertices[v].position - b.center));
            }
        }
    }
    // A little room for rounding, as the vertices are exact only before they are posed.
    for (SkinBound& b : bounds) {
        if (b.radius >= 0.0f) {
            b.radius = b.radius * 1.0001f + 1e-5f;
        }
    }
    return bounds;
}

f32 stretch_bound(const Mat4f& m) {
    // The largest eigenvalue of A^T A is at most the largest row sum of its absolute values (Gershgorin); its square root
    // bounds how much A stretches. Exact (1) for a rotation, and for a uniform scale.
    f32 g[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            g[i][j] = m.at(0, i) * m.at(0, j) + m.at(1, i) * m.at(1, j) + m.at(2, i) * m.at(2, j);
        }
    }
    f32 worst = 0.0f;
    for (int i = 0; i < 3; ++i) {
        worst = std::max(worst, std::fabs(g[i][0]) + std::fabs(g[i][1]) + std::fabs(g[i][2]));
    }
    return std::sqrt(worst);
}

bool skin_sphere(std::span<const SkinBound> bounds, std::span<const Mat4f> matrices, Vec3f& center, f32& radius) {
    const std::size_t n = std::min(bounds.size(), matrices.size());
    Vec3f lo{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    Vec3f hi{-lo.x, -lo.y, -lo.z};
    struct Ball {
        Vec3f c;
        f32 r;
    };
    // The moved balls, small enough on the stack for a skeleton of any size (at most max_skin_joints).
    Ball balls[max_skin_joints];
    std::size_t used = 0;
    for (std::size_t j = 0; j < n && used < max_skin_joints; ++j) {
        if (bounds[j].radius < 0.0f) {
            continue;
        }
        Ball b{matrices[j].apply(bounds[j].center), bounds[j].radius * stretch_bound(matrices[j])};
        if (!std::isfinite(b.c.x) || !std::isfinite(b.c.y) || !std::isfinite(b.c.z) || !std::isfinite(b.r)) {
            continue; // a broken matrix: its joint is not trusted to bound anything (the others still do)
        }
        lo = {std::min(lo.x, b.c.x - b.r), std::min(lo.y, b.c.y - b.r), std::min(lo.z, b.c.z - b.r)};
        hi = {std::max(hi.x, b.c.x + b.r), std::max(hi.y, b.c.y + b.r), std::max(hi.z, b.c.z + b.r)};
        balls[used++] = b;
    }
    if (used == 0) {
        return false;
    }
    center = (lo + hi) * 0.5f;
    radius = 0.0f;
    for (std::size_t i = 0; i < used; ++i) {
        radius = std::max(radius, length(balls[i].c - center) + balls[i].r);
    }
    radius = radius * 1.0001f + 1e-5f;
    return true;
}

// --- CPU skinning ----------------------------------------------------------------------------------------------------

MeshData skin_mesh(const MeshData& mesh, std::span<const Mat4f> matrices, bool gpu_weights) {
    MeshData out;
    out.vertices = mesh.vertices;
    out.indices = mesh.indices;
    if (mesh.skin.size() != mesh.vertices.size()) {
        return out;
    }
    for (std::size_t v = 0; v < out.vertices.size(); ++v) {
        const MeshSkinVertex& s = mesh.skin[v];
        std::array<f32, 4> w = s.weights;
        if (gpu_weights) {
            const std::array<u8, 4> q = quantise_skin_weights(s.weights);
            for (std::size_t k = 0; k < 4; ++k) {
                w[k] = static_cast<f32>(q[k]) / 255.0f;
            }
        }
        Vec3f position{};
        Mat4f sum;
        for (f32& e : sum.m) {
            e = 0.0f;
        }
        const Vec3f p = mesh.vertices[v].position;
        for (std::size_t k = 0; k < 4; ++k) {
            if (w[k] == 0.0f) {
                continue;
            }
            const Mat4f& m = s.joints[k] < matrices.size() ? matrices[s.joints[k]] : Mat4f{};
            position = position + m.apply(p) * w[k];
            for (int e = 0; e < 16; ++e) {
                sum.m[e] += m.m[e] * w[k];
            }
        }
        out.vertices[v].position = position;
        const Vec3f n = sum.apply_vector(mesh.vertices[v].normal);
        out.vertices[v].normal = length_squared(n) > 0.0f ? normalize(n) : mesh.vertices[v].normal;
    }
    return out;
}

} // namespace kin
