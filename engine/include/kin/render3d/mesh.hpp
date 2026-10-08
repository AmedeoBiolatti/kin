#pragma once

// Meshes for kin's experimental 3D (docs/3d_plan.md): triangles as data
// (MeshData), and the same kept on the GPU (Mesh, from Renderer3D::create_mesh).

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <memory>
#include <string>
#include <vector>

namespace kin {

struct MeshVertex {
    Vec3f position{};
    Vec3f normal{0.0f, 1.0f, 0.0f}; // length 1, away from the front face
    Vec2f uv{};
    Color color = colors::white; // times the draw's colour
};

// An axis-aligned box.
struct Bounds3 {
    Vec3f min{};
    Vec3f max{};

    Vec3f center() const { return (min + max) * 0.5f; }
    Vec3f size() const { return max - min; }
    // The radius of the sphere about center() that holds it.
    f32 radius() const { return length(size()) * 0.5f; }
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    // Three a triangle, counter-clockwise seen from its front (where its
    // normals point). Empty: the vertices are the triangles, three by three.
    std::vector<u32> indices;

    std::size_t triangle_count() const { return (indices.empty() ? vertices.size() : indices.size()) / 3; }
    // Whether it can be drawn: whole triangles, indices in range, finite
    // positions. False, with why in `error`, when not.
    bool validate(std::string* error = nullptr) const;
    Bounds3 bounds() const;
    // Appends `other`, moved by `transform` (normals turned with it). The
    // result is indexed.
    void append(const MeshData& other, const Mat4f& transform = Mat4f::identity());
};

// The most levels of detail a mesh can have (level 0, the mesh itself, included).
constexpr u32 max_mesh_levels = 8;

// How Renderer3D::create_mesh builds a mesh's levels of detail (Scene3D::lod
// chooses among them): a chain of simpler versions, each about `target_ratio`
// of the triangles of the one before, made with meshoptimizer's simplifier.
// Level 0 is always the mesh as given, to the bit. The levels share the
// mesh's vertices (a level is only another list of triangles over them), so
// the cost is a few more indices. See kin/render3d/mesh_lod.hpp.
struct MeshOptions {
    // How many levels to try for, level 0 included (1: just the mesh, as
    // before; at most max_mesh_levels). Fewer are made where a level cannot get
    // meaningfully simpler, or would be tiny.
    u32 levels = 1;
    // A level's triangles as a share of the previous level's (0.1 to 0.9).
    f32 target_ratio = 0.5f;
    // A level must have this many triangles fewer than the previous one (as a share of it), else the chain stops.
    f32 min_reduction = 0.15f;
    // Nor does the chain go below this many triangles.
    u32 min_triangles = 16;
    // A level lies no further from the one before than this share of the mesh's size (its largest extent): a mesh
    // that cannot be simplified further without wrecking it ends the chain.
    f32 max_error = 0.1f;
    // How much the simplifier minds changes of the normals, the UVs and the vertex colours, against moving the surface.
    f32 normal_weight = 0.5f;
    f32 uv_weight = 0.5f;
    f32 color_weight = 0.25f;
    // Vertices on an open border of the mesh stay where they are, so that a
    // border (a cut-open model, a mesh that meets another) does not shrink or tear.
    bool lock_border = true;
};

// What Mesh tells of one of its levels.
struct MeshLevelInfo {
    u32 triangles = 0;
    // An upper bound on how far (in the mesh's own units) the level's surface lies from level 0's; 0 for level 0.
    f32 error = 0.0f;
};

// The backend's copy of a mesh (Renderer3D::create_mesh).
class IMeshBackend {
public:
    virtual ~IMeshBackend() = default;
};

// A mesh on the GPU, made from MeshData; cheap to copy (copies share it).
// Empty where 3D isn't available: drawing it then does nothing.
class Mesh {
public:
    Mesh() = default;
    Mesh(std::shared_ptr<IMeshBackend> backend, u32 vertex_count, u32 index_count, Bounds3 bounds,
         std::shared_ptr<const std::vector<MeshLevelInfo>> levels = {})
        : _backend(std::move(backend)), _vertex_count(vertex_count), _index_count(index_count), _bounds(bounds), _levels(std::move(levels)) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    u32 vertex_count() const { return _vertex_count; }
    u32 index_count() const { return _index_count; }
    u32 triangle_count() const { return _index_count / 3; }
    // Levels of detail (MeshOptions::levels): level 0 is the mesh, the others
    // simpler. A mesh made without them has one, and triangle_count() is its
    // triangles; with them, level_triangles(0) is the same.
    u32 level_count() const { return _levels ? static_cast<u32>(_levels->size()) : 1u; }
    u32 level_triangles(u32 level) const { return _levels && level < _levels->size() ? (*_levels)[level].triangles : level == 0 ? triangle_count() : 0u; }
    f32 level_error(u32 level) const { return _levels && level < _levels->size() ? (*_levels)[level].error : 0.0f; }
    const Bounds3& bounds() const { return _bounds; }
    const std::shared_ptr<IMeshBackend>& backend() const { return _backend; }

    friend bool operator==(const Mesh& a, const Mesh& b) { return a._backend == b._backend; }

private:
    std::shared_ptr<IMeshBackend> _backend;
    u32 _vertex_count = 0;
    u32 _index_count = 0;
    Bounds3 _bounds{};
    std::shared_ptr<const std::vector<MeshLevelInfo>> _levels; // null: one level
};

// Shapes made in code, centred on the origin (a plane and a box's base on
// it where said), with normals and UVs.
namespace meshes {

// `size` along X, Y and Z; flat faces.
MeshData box(Vec3f size);
// A flat square on the XZ plane, facing +Y, `cells` squares along each side.
MeshData plane(Vec2f size, i32 cells = 1);
// Smooth, `segments` round the Y axis, `rings` from pole to pole.
MeshData sphere(f32 radius, i32 segments = 24, i32 rings = 12);
// Along Y, `height` tall, its caps flat, its side smooth.
MeshData cylinder(f32 radius, f32 height, i32 segments = 24);
// A cone along Y, its point at the top.
MeshData cone(f32 radius, f32 height, i32 segments = 24);
// A ring round the Y axis: `radius` to the tube's centre, `tube` its radius.
MeshData torus(f32 radius, f32 tube, i32 segments = 32, i32 sides = 12);
// A star of `points` points in the XZ plane, `outer` and `inner` radii,
// `height` thick along Y; flat faces.
MeshData star_prism(i32 points, f32 outer, f32 inner, f32 height);
// Eight faces, `radius` to each corner.
MeshData octahedron(f32 radius);
// A ramp in the box of `size`: its slope rising from the +Z edge (floor) to
// the -Z edge (top); flat faces.
MeshData wedge(Vec3f size);

} // namespace meshes

} // namespace kin
