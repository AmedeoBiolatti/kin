#pragma once

// Skinned meshes and glTF files made in code, so that no test of skinning depends on a download: a tube that bends over
// three joints (MeshData + Skeleton), and a tiny .gltf with embedded base64 buffers (a skinned strip, a skin, clips) whose
// accessor types and skin size the test chooses.

#include <kin/core/math3d.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/skeleton.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace skin_fixtures {

using kin::Mat4f;
using kin::Vec3f;

// A tube along +Y from 0 to `length`, `rings` rings of `segments` vertices (smooth normals), bound to a chain of three joints
// at 0, length / 3 and 2 length / 3 (weights fall off linearly between neighbours; above the last, all of it). Joint 0 is the
// root. The skeleton rests straight: local translations (0, 0, 0), (0, length / 3, 0) twice; inverse binds the matching
// translations down.
struct Tube {
    kin::MeshData mesh;
    kin::Skeleton skeleton;
    float length = 3.0f;
};

inline Tube make_tube(int rings = 25, int segments = 12, float length = 3.0f, float radius = 0.35f) {
    Tube tube;
    tube.length = length;
    const float third = length / 3.0f;
    kin::MeshData& m = tube.mesh;
    for (int r = 0; r < rings; ++r) {
        const float y = length * static_cast<float>(r) / static_cast<float>(rings - 1);
        const float v = static_cast<float>(r) / static_cast<float>(rings - 1);
        for (int s = 0; s <= segments; ++s) {
            const float a = 2.0f * 3.14159265f * static_cast<float>(s) / static_cast<float>(segments);
            const Vec3f n{std::cos(a), 0.0f, -std::sin(a)};
            m.vertices.push_back({{n.x * radius, y, n.z * radius}, n, {static_cast<float>(s) / static_cast<float>(segments), v}});
            std::array<float, 3> w{};
            float sum = 0.0f;
            for (int j = 0; j < 3; ++j) {
                w[static_cast<std::size_t>(j)] = std::max(0.0f, 1.0f - std::fabs(y - third * static_cast<float>(j)) / third);
                sum += w[static_cast<std::size_t>(j)];
            }
            kin::MeshSkinVertex k;
            if (y > 2.0f * third) {
                w = {0.0f, 0.0f, 1.0f};
                sum = 1.0f;
            }
            // The strongest two (hats overlap in two joints at most).
            k.joints = {0, 1, 2, 0};
            k.weights = {w[0] / sum, w[1] / sum, w[2] / sum, 0.0f};
            m.skin.push_back(k);
        }
    }
    const std::uint32_t row = static_cast<std::uint32_t>(segments) + 1;
    for (std::uint32_t r = 0; r + 1 < static_cast<std::uint32_t>(rings); ++r) {
        for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(segments); ++s) {
            const std::uint32_t a = r * row + s, b = a + 1, c = a + row + 1, d = a + row;
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    }
    // Wound counter-clockwise seen from outside? The ring runs from +x toward -z (as meshes::cylinder), going up: a, b, c with
    // b the next around and c above it is anticlockwise from outside.
    kin::Skeleton& sk = tube.skeleton;
    sk.names = {"base", "middle", "top"};
    sk.parents = {-1, 0, 1};
    sk.rest.resize(3);
    sk.rest[1].translation = {0.0f, third, 0.0f};
    sk.rest[2].translation = {0.0f, third, 0.0f};
    sk.inverse_bind = {Mat4f{}, Mat4f::translation({0.0f, -third, 0.0f}), Mat4f::translation({0.0f, -2.0f * third, 0.0f})};
    return tube;
}

// A pose bending the tube: the middle joint turned `a` degrees about Z, the top `b` about Z, and the base moved by `base`.
inline kin::Pose bent(const Tube& tube, float a, float b, Vec3f base = {}) {
    kin::Pose pose = tube.skeleton.rest_pose();
    pose.joints[0].translation = base;
    pose.joints[1].rotation = kin::Quatf::axis_angle({0.0f, 0.0f, 1.0f}, a);
    pose.joints[2].rotation = kin::Quatf::axis_angle({0.0f, 0.0f, 1.0f}, b);
    return pose;
}

// --- a glTF in memory ----------------------------------------------------------------------------------------------------

inline std::string base64(const std::vector<std::uint8_t>& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) | (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0u) |
                                (i + 2 < bytes.size() ? bytes[i + 2] : 0u);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? table[n & 63] : '=';
    }
    return out;
}

struct GltfOptions {
    enum class Joints { U8, U16 } joints = Joints::U8;
    enum class Weights { Float, U8, U16 } weights = Weights::Float;
    int joint_count = 2;          // joints of the skin: a chain; 2 uses the two the strip is bound to
    bool reversed = false;        // the skin lists its joints child first (parents after children)
    bool morph_channel = false;   // an animation channel on a node's weights
    bool bad_index = false;       // the last vertex names a joint past the skin, with weight
    bool no_weights = false;      // the first vertex has weights of zero
    bool unnormalised = false;    // weights that do not sum to 1 (scaled by 2)
    bool node_animation = false;  // an animation of a node that is no joint
    bool attachment = false;      // a mesh on the child joint
    bool two_influences = true;   // the strip's middle vertices share two joints, else all bound to the lower
};

struct Gltf {
    std::string json;
    // The expected values for the strip's vertices.
    std::array<std::array<float, 2>, 6> weight{}; // [vertex] weight of joint 0 and of joint 1 (after renormalising)
};

// A strip of four triangles (six vertices, y 0, 1, 2) bound to joint 0 (at the origin, root) and joint 1 (above it by 1);
// the bottom pair all joint 0, the top all joint 1, the middle pair half and half. A clip "bend": the child joint's rotation
// 0, 90, 0 degrees about Z at 0, 1, 2 s (linear), its translation y 1, 2 stepped, and the root's translation cubic.
inline Gltf make_gltf(const GltfOptions& o = {}) {
    std::vector<std::uint8_t> buffer;
    struct View {
        std::size_t offset, size;
    };
    std::vector<View> views;
    const auto add = [&](const void* data, std::size_t size) {
        while (buffer.size() % 4 != 0) buffer.push_back(0);
        views.push_back({buffer.size(), size});
        const auto* p = static_cast<const std::uint8_t*>(data);
        buffer.insert(buffer.end(), p, p + size);
        return views.size() - 1;
    };
    std::ostringstream accessors;
    int accessor_count = 0;
    const auto accessor = [&](std::size_t view, int component, std::size_t count, const char* type, bool normalized, const std::string& extra = "") {
        if (accessor_count++) accessors << ",";
        accessors << "{\"bufferView\":" << view << ",\"componentType\":" << component << ",\"count\":" << count << ",\"type\":\"" << type << "\""
                  << (normalized ? ",\"normalized\":true" : "") << extra << "}";
        return accessor_count - 1;
    };
    Gltf out;
    // The strip.
    const std::vector<float> positions = {-0.5f, 0, 0, 0.5f, 0, 0, -0.5f, 1, 0, 0.5f, 1, 0, -0.5f, 2, 0, 0.5f, 2, 0};
    std::vector<float> normals;
    for (int i = 0; i < 6; ++i) normals.insert(normals.end(), {0.0f, 0.0f, 1.0f});
    const std::vector<std::uint16_t> indices = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5};
    // Weights of joint 1 by vertex: 0, 0, .5, .5, 1, 1 (or all 0 with one influence).
    std::array<float, 6> upper = {0, 0, 0.5f, 0.5f, 1, 1};
    if (!o.two_influences) upper = {0, 0, 0, 0, 0, 0};
    std::vector<std::array<std::uint32_t, 4>> joint_ids(6);
    std::vector<std::array<float, 4>> weights(6);
    for (int i = 0; i < 6; ++i) {
        // With the skin listed child first, the file's index of joint 0 (the root) is joint_count - 2 + ... : see `index_of`.
        const std::uint32_t j0 = o.reversed ? static_cast<std::uint32_t>(o.joint_count - 1) : 0u;
        const std::uint32_t j1 = o.reversed ? static_cast<std::uint32_t>(o.joint_count - 2) : 1u;
        joint_ids[static_cast<std::size_t>(i)] = {j0, j1, 0, 0};
        weights[static_cast<std::size_t>(i)] = {1.0f - upper[static_cast<std::size_t>(i)], upper[static_cast<std::size_t>(i)], 0, 0};
        out.weight[static_cast<std::size_t>(i)] = {1.0f - upper[static_cast<std::size_t>(i)], upper[static_cast<std::size_t>(i)]};
    }
    if (o.unnormalised) {
        for (auto& w : weights) for (float& x : w) x *= 2.0f;
    }
    if (o.no_weights) {
        weights[0] = {0, 0, 0, 0};
        out.weight[0] = {1.0f, 0.0f}; // goes to joint 0
    }
    if (o.bad_index) {
        joint_ids[5][1] = 200; // past the skin, with weight 1: dropped, the vertex keeps joint 0 alone
        weights[5] = {0.5f, 0.5f, 0, 0};
        out.weight[5] = {1.0f, 0.0f};
    }
    const auto v_pos = add(positions.data(), positions.size() * 4);
    const auto v_nor = add(normals.data(), normals.size() * 4);
    const int a_pos = accessor(v_pos, 5126, 6, "VEC3", false, ",\"min\":[-0.5,0,0],\"max\":[0.5,2,0]");
    const int a_nor = accessor(v_nor, 5126, 6, "VEC3", false);
    std::vector<std::uint8_t> j_bytes;
    for (const auto& ids : joint_ids) {
        for (std::uint32_t id : ids) {
            if (o.joints == GltfOptions::Joints::U8) {
                j_bytes.push_back(static_cast<std::uint8_t>(id));
            } else {
                j_bytes.push_back(static_cast<std::uint8_t>(id & 255));
                j_bytes.push_back(static_cast<std::uint8_t>(id >> 8));
            }
        }
    }
    const auto v_joint = add(j_bytes.data(), j_bytes.size());
    const int a_joint = accessor(v_joint, o.joints == GltfOptions::Joints::U8 ? 5121 : 5123, 6, "VEC4", false);
    std::vector<std::uint8_t> w_bytes;
    for (const auto& w : weights) {
        for (float x : w) {
            if (o.weights == GltfOptions::Weights::Float) {
                std::uint8_t b[4];
                std::memcpy(b, &x, 4);
                w_bytes.insert(w_bytes.end(), b, b + 4);
            } else if (o.weights == GltfOptions::Weights::U8) {
                w_bytes.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)));
            } else {
                const auto v = static_cast<std::uint16_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f));
                w_bytes.push_back(static_cast<std::uint8_t>(v & 255));
                w_bytes.push_back(static_cast<std::uint8_t>(v >> 8));
            }
        }
    }
    const auto v_weight = add(w_bytes.data(), w_bytes.size());
    const int a_weight = accessor(v_weight, o.weights == GltfOptions::Weights::Float ? 5126 : o.weights == GltfOptions::Weights::U8 ? 5121 : 5123, 6, "VEC4",
                                  o.weights != GltfOptions::Weights::Float);
    const auto v_index = add(indices.data(), indices.size() * 2);
    const int a_index = accessor(v_index, 5123, indices.size(), "SCALAR", false);

    // The skin: `joint_count` nodes in a chain. Node numbering: 0 the armature (moved), 1.. the joints in the chain, the
    // skinned mesh node last. Chain joint k (0 root) is node 1 + k.
    const int n = o.joint_count;
    std::vector<float> ibm;
    for (int k = 0; k < n; ++k) {
        Mat4f m = Mat4f::translation({0.0f, -static_cast<float>(k), 0.0f});
        ibm.insert(ibm.end(), m.m, m.m + 16);
    }
    // Listed child first when reversed: the file's joint i is chain joint n - 1 - i.
    if (o.reversed) {
        std::vector<float> r;
        for (int i = 0; i < n; ++i) r.insert(r.end(), ibm.begin() + (n - 1 - i) * 16, ibm.begin() + (n - i) * 16);
        ibm = r;
    }
    const auto v_ibm = add(ibm.data(), ibm.size() * 4);
    const int a_ibm = accessor(v_ibm, 5126, static_cast<std::size_t>(n), "MAT4", false);

    // Animations. Child (chain joint 1): rotation linear 0, 90, 0 about Z at 0, 1, 2 s.
    const std::vector<float> times = {0.0f, 1.0f, 2.0f};
    const auto q = [](float deg) {
        const auto r = kin::Quatf::axis_angle({0, 0, 1}, deg);
        return std::array<float, 4>{r.x, r.y, r.z, r.w};
    };
    std::vector<float> rot;
    for (float deg : {0.0f, 90.0f, 0.0f}) {
        const auto r = q(deg);
        rot.insert(rot.end(), r.begin(), r.end());
    }
    // The root: a cubic spline of translation over two keys (0 and 1 s): in-tangent, value, out-tangent each.
    const std::vector<float> times2 = {0.0f, 1.0f};
    const std::vector<float> spline = {0, 0, 0, 0, 0, 0, 1, 0, 0,  // key 0: in (unused), value (0,0,0), out (1,0,0)
                                       0, 0, 0, 2, 0, 0, 0, 0, 0}; // key 1: in (0,0,0), value (2,0,0), out (unused)
    // The child's translation, stepped.
    const std::vector<float> step_values = {0, 1, 0, 0, 2, 0, 0, 1, 0};
    const auto v_t3 = add(times.data(), 12);
    const auto v_rot = add(rot.data(), rot.size() * 4);
    const auto v_t2 = add(times2.data(), 8);
    const auto v_spline = add(spline.data(), spline.size() * 4);
    const auto v_step = add(step_values.data(), step_values.size() * 4);
    const int a_t3 = accessor(v_t3, 5126, 3, "SCALAR", false, ",\"min\":[0],\"max\":[2]");
    const int a_rot = accessor(v_rot, 5126, 3, "VEC4", false);
    const int a_t2 = accessor(v_t2, 5126, 2, "SCALAR", false, ",\"min\":[0],\"max\":[1]");
    const int a_spline = accessor(v_spline, 5126, 6, "VEC3", false);
    const int a_step = accessor(v_step, 5126, 3, "VEC3", false);
    int a_weights_anim = -1;
    if (o.morph_channel) {
        const std::vector<float> w = {0.0f, 1.0f, 0.0f};
        a_weights_anim = accessor(add(w.data(), 12), 5126, 3, "SCALAR", false);
    }

    std::ostringstream j;
    const int loose = n + 2 + (o.attachment ? 1 : 0);
    j << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0," << (n + 1);
    if (o.node_animation) j << "," << loose;
    j << "]}],";
    j << "\"nodes\":[";
    j << "{\"name\":\"Armature\",\"translation\":[2,0,0],\"children\":[1]},";
    for (int k = 0; k < n; ++k) {
        j << "{\"name\":\"joint" << k << "\"" << (k == 0 ? "" : ",\"translation\":[0,1,0]");
        if (k + 1 < n || (o.attachment && k == 1)) {
            j << ",\"children\":[";
            if (k + 1 < n) j << (2 + k);
            if (o.attachment && k == 1) j << (k + 1 < n ? "," : "") << (n + 2);
            j << "]";
        }
        j << "},";
    }
    j << "{\"name\":\"strip\",\"mesh\":0,\"skin\":0}";
    if (o.attachment) j << ",{\"name\":\"hat\",\"mesh\":1,\"translation\":[0,0.5,0]}";
    if (o.node_animation) j << ",{\"name\":\"loose\",\"mesh\":1}";
    j << "],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" << a_pos << ",\"NORMAL\":" << a_nor << ",\"JOINTS_0\":" << a_joint
      << ",\"WEIGHTS_0\":" << a_weight << "},\"indices\":" << a_index
      << (o.morph_channel ? ",\"targets\":[{\"POSITION\":" + std::to_string(a_pos) + "}]" : std::string{}) << "}]},"
      << "{\"primitives\":[{\"attributes\":{\"POSITION\":" << a_pos << ",\"NORMAL\":" << a_nor << "},\"indices\":" << a_index << "}]}],";
    // The skin's joint list: nodes 1..n (chain order), or child first.
    j << "\"skins\":[{\"inverseBindMatrices\":" << a_ibm << ",\"joints\":[";
    for (int i = 0; i < n; ++i) {
        const int chain = o.reversed ? n - 1 - i : i;
        j << (i ? "," : "") << (1 + chain);
    }
    j << "]}],";
    j << "\"animations\":[{\"name\":\"bend\",\"samplers\":[{\"input\":" << a_t3 << ",\"output\":" << a_rot << ",\"interpolation\":\"LINEAR\"},"
      << "{\"input\":" << a_t2 << ",\"output\":" << a_spline << ",\"interpolation\":\"CUBICSPLINE\"},"
      << "{\"input\":" << a_t3 << ",\"output\":" << a_step << ",\"interpolation\":\"STEP\"}"
      << (o.morph_channel ? std::string{",{\"input\":"} + std::to_string(a_t3) + ",\"output\":" + std::to_string(a_weights_anim) + ",\"interpolation\":\"LINEAR\"}" : std::string{})
      << "],\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}},"
      << "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"translation\"}},"
      << "{\"sampler\":2,\"target\":{\"node\":2,\"path\":\"translation\"}}"
      << (o.morph_channel ? ",{\"sampler\":3,\"target\":{\"node\":" + std::to_string(n + 1) + ",\"path\":\"weights\"}}" : std::string{})
      << (o.node_animation ? ",{\"sampler\":2,\"target\":{\"node\":" + std::to_string(loose) + ",\"path\":\"translation\"}}" : std::string{}) << "]}],";
    j << "\"buffers\":[{\"byteLength\":" << buffer.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64(buffer) << "\"}],\"bufferViews\":[";
    for (std::size_t i = 0; i < views.size(); ++i) {
        j << (i ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << views[i].offset << ",\"byteLength\":" << views[i].size << "}";
    }
    j << "],\"accessors\":[" << accessors.str() << "]}";
    out.json = j.str();
    return out;
}


// --- a small biped for inverse kinematics --------------------------------------------------------------------------------

// Joint indices of make_biped(). A person of 1 m of leg and 0.95 m of arm, facing +Z, standing straight with the arms out
// (so every limb chain is straight at rest: the bend plane is the pole's to give).
namespace biped {
enum : int {
    root, pelvis, spine, chest, neck, head,
    hip_l, knee_l, ankle_l, hip_r, knee_r, ankle_r,
    shoulder_l, elbow_l, hand_l, shoulder_r, elbow_r, hand_r,
    count
};
} // namespace biped

inline kin::Skeleton make_biped() {
    kin::Skeleton sk;
    struct J {
        const char* name;
        int parent;
        Vec3f at;
    };
    const J joints[] = {
        {"root", -1, {0, 0, 0}},        {"pelvis", 0, {0, 1.0f, 0}},     {"spine", 1, {0, 0.2f, 0}},       {"chest", 2, {0, 0.25f, 0}},
        {"neck", 3, {0, 0.25f, 0}},     {"head", 4, {0, 0.1f, 0}},       {"hip_L", 1, {0.1f, 0, 0}},       {"knee_L", 6, {0, -0.5f, 0}},
        {"ankle_L", 7, {0, -0.45f, 0}}, {"hip_R", 1, {-0.1f, 0, 0}},     {"knee_R", 9, {0, -0.5f, 0}},     {"ankle_R", 10, {0, -0.45f, 0}},
        {"shoulder_L", 3, {0.2f, 0.2f, 0}}, {"elbow_L", 12, {0.5f, 0, 0}}, {"hand_L", 13, {0.45f, 0, 0}},
        {"shoulder_R", 3, {-0.2f, 0.2f, 0}}, {"elbow_R", 15, {-0.5f, 0, 0}}, {"hand_R", 16, {-0.45f, 0, 0}},
    };
    for (const J& j : joints) {
        sk.names.emplace_back(j.name);
        sk.parents.push_back(j.parent);
        kin::JointPose r;
        r.translation = j.at;
        sk.rest.push_back(r);
    }
    std::vector<Mat4f> world;
    kin::compute_world_matrices(sk, sk.rest_pose(), world);
    for (const Mat4f& m : world) {
        sk.inverse_bind.push_back(m.inverse());
    }
    return sk;
}


// A skinned body for the biped: a box along each bone (from the parent joint to the joint, the end at the parent bound to the
// parent and the end at the joint bound to the joint), `radius` thick, and, separately, a cube round one joint (a hand marker).
inline void append_box(kin::MeshData& m, Vec3f p0, Vec3f p1, float radius, int joint0, int joint1) {
    const Vec3f d = kin::normalize(p1 - p0);
    const Vec3f helper = std::fabs(d.y) < 0.9f ? Vec3f{0, 1, 0} : Vec3f{1, 0, 0};
    const Vec3f u = kin::normalize(kin::cross(helper, d)); // so that (d, u, v) is right-handed: cross(d, u) = v
    const Vec3f v = kin::cross(d, u);
    const float half = kin::length(p1 - p0) * 0.5f;
    const Vec3f mid = (p0 + p1) * 0.5f;
    struct Face {
        Vec3f normal, e1, e2;
    };
    const Face faces[6] = {
        {u, v * radius, d * half},  {u * -1.0f, d * half, v * radius}, {v, d * half, u * radius},
        {v * -1.0f, u * radius, d * half}, {d, u * radius, v * radius}, {d * -1.0f, v * radius, u * radius},
    };
    for (int f = 0; f < 6; ++f) {
        const Face& face = faces[f];
        const Vec3f centre = f < 4 ? mid + face.normal * radius : (f == 4 ? p1 : p0);
        const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
        const std::uint32_t first = static_cast<std::uint32_t>(m.vertices.size());
        for (int c = 0; c < 4; ++c) {
            const Vec3f pos = centre + face.e1 * sx[c] + face.e2 * sy[c];
            m.vertices.push_back({pos, face.normal, {sx[c] * 0.5f + 0.5f, sy[c] * 0.5f + 0.5f}});
            kin::MeshSkinVertex k;
            k.joints = {static_cast<std::uint8_t>(kin::dot(pos - p0, d) > half ? joint1 : joint0), 0, 0, 0};
            m.skin.push_back(k);
        }
        m.indices.insert(m.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }
}

// The boxes of every bone but those of the root and the pelvis (their bone is the spine's beginning).
inline kin::MeshData make_biped_body(const kin::Skeleton& sk, float radius = 0.045f) {
    kin::MeshData m;
    std::vector<Mat4f> world;
    kin::compute_world_matrices(sk, sk.rest_pose(), world);
    const auto at = [&](std::size_t j) { return Vec3f{world[j].m[12], world[j].m[13], world[j].m[14]}; };
    for (std::size_t j = 2; j < sk.joint_count(); ++j) {
        const std::size_t p = static_cast<std::size_t>(sk.parents[j]);
        if (p >= 1) {
            append_box(m, at(p), at(j), radius, static_cast<int>(p), static_cast<int>(j));
        }
    }
    return m;
}

inline kin::MeshData make_joint_cube(const kin::Skeleton& sk, int joint, float radius = 0.07f) {
    kin::MeshData m;
    std::vector<Mat4f> world;
    kin::compute_world_matrices(sk, sk.rest_pose(), world);
    const Mat4f& w = world[static_cast<std::size_t>(joint)];
    const Vec3f at{w.m[12], w.m[13], w.m[14]};
    append_box(m, at - Vec3f{0, 0, radius}, at + Vec3f{0, 0, radius}, radius, joint, joint);
    return m;
}

} // namespace skin_fixtures
