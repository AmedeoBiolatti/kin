#include "../../tests/result3d_helpers.hpp"
#include "../sponza_walk/character.hpp"
#include "props.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <kin/assets/content.hpp>
#include <kin/render3d/model3d.hpp>

namespace {
using namespace summit;
int failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            ++failures;                                                                                                \
            std::fprintf(stderr, "asset_tests:%d: %s\n", __LINE__, #c);                                                \
        }                                                                                                              \
    } while (0)

ModelData3D read(const std::filesystem::path& path) {
    ModelData3D data;
    const auto bytes = read_content_file(path);
    CHECK(bytes.has_value());
    if (bytes) {
        std::string error;
        CHECK(kin::test::read_value3d(kin::read_model_data(*bytes, path), data, error));
    }
    return data;
}

void character() {
    const auto data = read(std::filesystem::path(SUMMIT_ASSET_CONTENT) / "kenney-character/survivor.glb");
    CHECK(data.parts.size() == 1 && data.skeletons.size() == 1 && data.clips.size() == 6);
    if (data.skeletons.size() != 1)
        return;
    const auto canonical = sponza_walk::person_rig().skeleton;
    const Skeleton3D& sk = data.skeletons[0];
    CHECK(sk.names == canonical.names && sk.parents == canonical.parents);
    CHECK(sk.validate());
    std::vector<Mat4f> rest;
    compute_joint_matrices(sk, sk.rest_pose(), rest);
    for (u32 j = 0; j < sk.joint_count(); ++j) {
        CHECK(length(sk.rest[j].translation - canonical.rest[j].translation) < 0.00001f);
        const Mat4f identity;
        for (int k = 0; k < 16; ++k)
            CHECK(std::abs(rest[j].m[k] - identity.m[k]) < 0.00001f);
    }
    u32 triangles = 0;
    for (const ModelPartData3D& part : data.parts) {
        triangles += part.mesh.triangle_count();
        CHECK(part.skin == 0 && part.mesh.skin.size() == part.mesh.vertices.size());
        for (const MeshSkinVertex3D& skin : part.mesh.skin) {
            f32 total = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                CHECK(skin.joints[k] < sk.joint_count());
                total += skin.weights[k];
            }
            CHECK(std::abs(total - 1) < 0.00001f);
        }
        // Fitting the source's extra upper-chest/clavicle weights as the lower chest used to lift the shirt over the
        // head.
        for (std::size_t v = 0; v < part.mesh.vertices.size(); ++v) {
            const auto& skin = part.mesh.skin[v];
            f32 chest = 0;
            for (std::size_t k = 0; k < 4; ++k)
                if (skin.joints[k] == sk.find("spine") || skin.joints[k] == sk.find("chest"))
                    chest += skin.weights[k];
            if (chest > 0.8f)
                CHECK(part.mesh.vertices[v].position.y < 1.80f);
        }
    }
    CHECK(triangles == 1604);
    for (const char* name : {"Idle", "Walk", "Run", "Jump", "Fall", "Pickup"}) {
        const auto clip =
            std::find_if(data.clips.begin(), data.clips.end(), [&](const auto& c) { return c.name == name; });
        CHECK(clip != data.clips.end());
        if (clip == data.clips.end())
            continue;
        CHECK(clip->duration > 0);
        std::vector<Mat4f> first;
        bool changed = false;
        for (int frame = 0; frame <= 8; ++frame) {
            Pose3D pose;
            sample_clip(*clip, sk, clip->duration * f32(frame) / 8, false, pose);
            std::vector<Mat4f> matrices;
            compute_joint_matrices(sk, pose, matrices);
            if (!frame)
                first = matrices;
            for (std::size_t j = 0; j < matrices.size(); ++j)
                for (int k = 0; k < 16; ++k) {
                    CHECK(std::isfinite(matrices[j].m[k]));
                    changed = changed || std::abs(matrices[j].m[k] - first[j].m[k]) > 0.001f;
                }
        }
        if (std::string_view(name) != "Fall")
            CHECK(changed);
    }
}

void baked_geometry() {
    for (int kind = 0; kind < PropCount; ++kind) {
        const auto data = read(std::filesystem::path(SUMMIT_ASSET_CONTENT) / "kenney-survival" /
                               (std::string(prop_names[kind]) + ".glb"));
        Vec3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        std::vector<Vec3f> source;
        for (const auto& part : data.parts)
            for (const MeshVertex3D& vertex : part.mesh.vertices) {
                const Vec3f p = part.transform.apply(vertex.position);
                source.push_back(p);
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        const auto& baked = prop_geometry_data(static_cast<PropKind>(kind));
        CHECK(length(lo - baked.source_min) < 0.00001f && length(hi - baked.source_max) < 0.00001f);
        const Vec3f bottom{(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f};
        const f32 extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        for (Vec3f p : baked.points) {
            bool found = false;
            for (Vec3f s : source)
                found = found || length((s - bottom) / extent - p) < 0.00001f;
            CHECK(found);
        }
        if (solid_prop(static_cast<PropKind>(kind))) {
            const auto shape = prop_shape({static_cast<PropKind>(kind), {}, 1, 0});
            CHECK(shape.valid());
            if (kind == Tent || kind == Firepit)
                CHECK(shape.kind() == Shape3DKind::TriangleMesh);
        }
    }
}
} // namespace

int main() {
    character();
    baked_geometry();
    if (failures) {
        std::fprintf(stderr, "asset_tests: %d failures\n", failures);
        return 1;
    }
    std::puts("asset_tests: all passed");
}
