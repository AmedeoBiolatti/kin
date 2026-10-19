#pragma once

// Meshes for kin's experimental 3D (docs/3d_plan.md): triangles as data
// (MeshData3D), and the same kept on the GPU (Mesh3D, from Renderer3D::create_mesh).

#include <array>
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <memory>
#include <string>
#include <vector>

namespace kin {

struct MeshVertex3D {
    Vec3f position{};
    Vec3f normal{0.0f, 1.0f, 0.0f}; // length 1, away from the front face
    Vec2f uv{};
    Color color = colors::white; // times the draw's colour
};

// An axis-aligned box.
struct Bounds3D {
    Vec3f min{};
    Vec3f max{};

    Vec3f center() const { return (min + max) * 0.5f; }
    Vec3f size() const { return max - min; }
    // The radius of the sphere about center() that holds it.
    f32 radius() const { return length(size()) * 0.5f; }
};

// The most joints a skin (a skeleton a mesh is bound to) can have: a joint index is one byte on the GPU.
constexpr u32 max_skin_joints = 256;

// What binds one vertex to a skeleton (docs/rendering_3d.md, Skinned meshes): up to four joints and how much each pulls
// the vertex, kept apart from MeshVertex3D so that a mesh without it is untouched. The weights should sum to 1 (the
// loader makes them; `validate` only asks for finite, non-negative weights with a sum above zero).
struct MeshSkinVertex3D {
    std::array<u8, 4> joints{};
    std::array<f32, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};
};

// A weight set as the GPU holds it: four unsigned normalised bytes that sum to exactly 255 (the largest takes the
// rounding), so a vertex bound to one joint is moved exactly as that joint moves. Weights that sum to zero give 255 to
// the first.
std::array<u8, 4> quantise_skin_weights(const std::array<f32, 4>& weights);

struct MeshData3D {
    std::vector<MeshVertex3D> vertices;
    // Three a triangle, counter-clockwise seen from its front (where its
    // normals point). Empty: the vertices are the triangles, three by three.
    std::vector<u32> indices;
    // Optional: one entry for each vertex, or none (a mesh that is not skinned). A skinned mesh is drawn with
    // Scene3D::draw_skinned and the joint matrices of its pose; it cannot have levels of detail or clusters.
    std::vector<MeshSkinVertex3D> skin;

    bool skinned() const { return !skin.empty(); }
    // The joints the skin uses: one more than the highest index with a weight above zero (0 for an unskinned mesh).
    u32 skin_joint_count() const;
    std::size_t triangle_count() const { return (indices.empty() ? vertices.size() : indices.size()) / 3; }
    // Whether it can be drawn: whole triangles, indices in range, finite
    // positions. False, with why in `error`, when not.
    bool validate(std::string* error = nullptr) const;
    Bounds3D bounds() const;
    // Appends `other`, moved by `transform` (normals turned with it). The
    // result is indexed. If either has a skin, the result has one: vertices of a mesh without it are bound to
    // joint 0 (the joints themselves are kept as they are, not renumbered).
    void append(const MeshData3D& other, const Mat4f& transform = Mat4f::identity());
};

// The most levels of detail a mesh can have (level 0, the mesh itself, included).
constexpr u32 max_mesh_levels = 8;

// How Renderer3D::create_mesh builds a mesh's levels of detail (Scene3D::lod
// chooses among them): a chain of simpler versions, each about `target_ratio`
// of the triangles of the one before, made with meshoptimizer's simplifier.
// Level 0 is always the mesh as given, to the bit. The levels share the
// mesh's vertices (a level is only another list of triangles over them), so
// the cost is a few more indices. See kin/render3d/mesh_lod3d.hpp.
struct MeshOptions3D {
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
    // A hierarchy of clusters of about 128 triangles instead of whole-mesh levels (kin/render3d/mesh_clusters3d.hpp):
    // Scene3D::lod then chooses, for each copy, a cut through the hierarchy, so that the part of one huge mesh near
    // the camera stays fine while the far part is coarse. `levels` is ignored for such a mesh. Built at load, over the
    // mesh welded (by position, UV and colour where it is flat-shaded: its normals are then made in the shader from
    // the screen-space derivatives of the position, so a normal map has no effect on it). A bound on the number of
    // triangles in a cluster (at most 128).
    bool clusters = false;
    u32 cluster_triangles = 128;
    bool operator==(const MeshOptions3D&) const = default;
};

// What Mesh3D tells of its cluster hierarchy.
struct MeshClusterInfo3D {
    u32 clusters = 0;        // all of them, every level
    u32 levels = 0;          // rounds of grouping and simplifying, leaves included (1: the leaves only)
    u32 roots = 0;           // clusters with no parent (1 when the hierarchy reached a single cluster)
    u32 leaf_triangles = 0;  // the mesh's own
    u64 total_triangles = 0; // all clusters' (the cost of keeping the hierarchy)
    bool flat = false;       // the normals are made in the shader
};

// What Mesh3D tells of one of its levels.
struct MeshLevelInfo3D {
    u32 triangles = 0;
    // An upper bound on how far (in the mesh's own units) the level's surface lies from level 0's; 0 for level 0.
    f32 error = 0.0f;
};

// The backend's copy of a mesh (Renderer3D::create_mesh).
class IMeshBackend3D {
public:
    virtual ~IMeshBackend3D() = default;
};

// A mesh on the GPU, made from MeshData3D; cheap to copy (copies share it).
// Empty where 3D isn't available: drawing it then does nothing.
class Mesh3D {
public:
    Mesh3D() = default;
    Mesh3D(std::shared_ptr<IMeshBackend3D> backend, u32 vertex_count, u32 index_count, Bounds3D bounds,
           std::shared_ptr<const std::vector<MeshLevelInfo3D>> levels = {},
           std::shared_ptr<const MeshClusterInfo3D> clusters = {}, u32 skin_joints = 0)
        : _backend(std::move(backend)), _vertex_count(vertex_count), _index_count(index_count), _bounds(bounds),
          _levels(std::move(levels)), _clusters(std::move(clusters)), _skin_joints(skin_joints) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    u32 vertex_count() const { return _vertex_count; }
    u32 index_count() const { return _index_count; }
    u32 triangle_count() const { return _index_count / 3; }
    // Levels of detail (MeshOptions3D::levels): level 0 is the mesh, the others
    // simpler. A mesh made without them has one, and triangle_count() is its
    // triangles; with them, level_triangles(0) is the same.
    u32 level_count() const { return _levels ? static_cast<u32>(_levels->size()) : 1u; }
    u32 level_triangles(u32 level) const {
        return _levels && level < _levels->size() ? (*_levels)[level].triangles : level == 0 ? triangle_count() : 0u;
    }
    f32 level_error(u32 level) const { return _levels && level < _levels->size() ? (*_levels)[level].error : 0.0f; }
    // The cluster hierarchy (MeshOptions3D::clusters): none for a mesh made without. Read-only.
    bool has_clusters() const { return _clusters != nullptr; }
    u32 cluster_count() const { return _clusters ? _clusters->clusters : 0u; }
    u32 cluster_levels() const { return _clusters ? _clusters->levels : 0u; }
    u32 cluster_roots() const { return _clusters ? _clusters->roots : 0u; }
    u32 leaf_triangles() const { return _clusters ? _clusters->leaf_triangles : triangle_count(); }
    u64 cluster_triangles() const { return _clusters ? _clusters->total_triangles : 0u; }
    bool flat_shaded() const { return _clusters && _clusters->flat; }
    // A mesh made from MeshData3D with a skin: the joints it needs matrices for (Scene3D::draw_skinned wants at least
    // that many), and whether it has them. `bounds()` is the mesh at rest.
    bool skinned() const { return _skin_joints > 0; }
    u32 skin_joint_count() const { return _skin_joints; }
    const Bounds3D& bounds() const { return _bounds; }
    const std::shared_ptr<IMeshBackend3D>& backend() const { return _backend; }

    friend bool operator==(const Mesh3D& a, const Mesh3D& b) { return a._backend == b._backend; }

private:
    std::shared_ptr<IMeshBackend3D> _backend;
    u32 _vertex_count = 0;
    u32 _index_count = 0;
    Bounds3D _bounds{};
    std::shared_ptr<const std::vector<MeshLevelInfo3D>> _levels; // null: one level
    std::shared_ptr<const MeshClusterInfo3D> _clusters;          // null: no clusters
    u32 _skin_joints = 0;                                        // 0: not skinned
};

// Shapes made in code, centred on the origin (a plane and a box's base on
// it where said), with normals and UVs.
namespace meshes {

// `size` along X, Y and Z; flat faces.
MeshData3D box(Vec3f size);
// A flat square on the XZ plane, facing +Y, `cells` squares along each side.
MeshData3D plane(Vec2f size, i32 cells = 1);
// Smooth, `segments` round the Y axis, `rings` from pole to pole.
MeshData3D sphere(f32 radius, i32 segments = 24, i32 rings = 12);
// Along Y, `height` tall, its caps flat, its side smooth.
MeshData3D cylinder(f32 radius, f32 height, i32 segments = 24);
// A cone along Y, its point at the top.
MeshData3D cone(f32 radius, f32 height, i32 segments = 24);
// A ring round the Y axis: `radius` to the tube's centre, `tube` its radius.
MeshData3D torus(f32 radius, f32 tube, i32 segments = 32, i32 sides = 12);
// A star of `points` points in the XZ plane, `outer` and `inner` radii,
// `height` thick along Y; flat faces.
MeshData3D star_prism(i32 points, f32 outer, f32 inner, f32 height);
// Eight faces, `radius` to each corner.
MeshData3D octahedron(f32 radius);
// A ramp in the box of `size`: its slope rising from the +Z edge (floor) to
// the -Z edge (top); flat faces.
MeshData3D wedge(Vec3f size);

} // namespace meshes

} // namespace kin
