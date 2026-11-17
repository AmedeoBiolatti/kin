// Lightmap UVs with xatlas (MIT): charts cut, flattened and packed into one square (kin/render3d/baked_lighting3d.hpp).

#include <algorithm>
#include <cmath>
#include <kin/render3d/baked_lighting3d.hpp>
#include <memory>
#include <xatlas.h>

namespace kin {

Status3D generate_lightmap_uv(MeshData3D& mesh, const LightmapUvOptions3D& options) {
    const auto fail = [](std::string why) { return status3d(false, {"generate_lightmap_uv: " + std::move(why)}); };
    if (Status3D valid = mesh.validate(); !valid) {
        return fail(valid.diagnostics.front());
    }
    if (mesh.skinned() || mesh.morphed()) {
        return fail("a skinned or morphed mesh cannot have lightmap UVs");
    }
    if (options.resolution < 16 || options.resolution > 8192) {
        return fail("the resolution is outside 16 to 8192");
    }
    const std::unique_ptr<xatlas::Atlas, void (*)(xatlas::Atlas*)> atlas{xatlas::Create(), xatlas::Destroy};
    xatlas::MeshDecl decl;
    decl.vertexPositionData = &mesh.vertices[0].position;
    decl.vertexPositionStride = sizeof(MeshVertex3D);
    decl.vertexNormalData = &mesh.vertices[0].normal;
    decl.vertexNormalStride = sizeof(MeshVertex3D);
    decl.vertexCount = static_cast<u32>(mesh.vertices.size());
    if (!mesh.indices.empty()) {
        decl.indexData = mesh.indices.data();
        decl.indexCount = static_cast<u32>(mesh.indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;
    } else {
        decl.faceCount = static_cast<u32>(mesh.vertices.size() / 3);
    }
    if (const xatlas::AddMeshError error = xatlas::AddMesh(atlas.get(), decl); error != xatlas::AddMeshError::Success) {
        return fail(std::string("xatlas refused the mesh: ") + xatlas::StringForEnum(error));
    }
    // Texels a unit so that the charts fill about the square at the resolution asked for (they are packed into one
    // atlas, whatever its size, then scaled into [0, 1] by its larger side).
    f64 area = 0.0;
    for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
        const auto at = [&](std::size_t k) {
            return mesh.vertices[mesh.indices.empty() ? 3 * t + k : mesh.indices[3 * t + k]].position;
        };
        area += 0.5 * static_cast<f64>(length(cross(at(1) - at(0), at(2) - at(0))));
    }
    if (!(area > 0.0)) {
        return fail("the mesh has no area");
    }
    xatlas::ChartOptions charts;
    xatlas::PackOptions pack;
    pack.padding = options.padding;
    pack.bilinear = true;
    pack.texelsPerUnit = static_cast<f32>(0.75 * static_cast<f64>(options.resolution) / std::sqrt(area));
    pack.resolution = 0;
    xatlas::Generate(atlas.get(), charts, pack);
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        return fail("xatlas made no atlas");
    }
    const xatlas::Mesh& out = atlas->meshes[0];
    if (out.indexCount != mesh.triangle_count() * 3) {
        return fail("xatlas changed the number of triangles");
    }
    const f32 side = static_cast<f32>(std::max(atlas->width, atlas->height));
    std::vector<MeshVertex3D> vertices(out.vertexCount);
    std::vector<Vec2f> uvs(out.vertexCount);
    for (u32 i = 0; i < out.vertexCount; ++i) {
        const xatlas::Vertex& v = out.vertexArray[i];
        vertices[i] = mesh.vertices[v.xref];
        uvs[i] = {std::clamp(v.uv[0] / side, 0.0f, 1.0f), std::clamp(v.uv[1] / side, 0.0f, 1.0f)};
    }
    mesh.vertices = std::move(vertices);
    mesh.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    mesh.lightmap_uv = std::move(uvs);
    return status3d(true);
}

} // namespace kin
