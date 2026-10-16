#pragma once
#include <kin/core/math3d.hpp>
#include <filesystem>
#include <string>
#include <vector>
namespace kin {
struct ModelGeometryLimits3D {
    u64 source_bytes=16u*1024u*1024u, buffer_bytes=16u*1024u*1024u;
    std::size_t vertices=32768, triangles=32768, parts=256;
};
struct ModelGeometry3D {
    std::vector<Vec3f> vertices;
    std::vector<u32> indices;
    std::vector<std::filesystem::path> source_files;
};
// CPU-only static geometry using the existing glTF parser and node transforms.
// Checks buffer/accessor budgets before allocating geometry. No textures,
// normals, animation, skinning or GPU copies; failure retains the old output.
bool read_model_geometry3d(const std::filesystem::path& path, ModelGeometry3D& out,
                          ModelGeometryLimits3D limits={}, std::string* error=nullptr);
}
