#pragma once
// Area lights for global illumination (experimental; GlobalIllumination3D::area_lights): emissive surfaces lit
// exactly instead of through the probes. On the CPU: each mesh's triangles in a bounding volume hierarchy of its own
// (its space, made once), the copies in one over them (made again when a copy moves, comes or goes), and the emissive
// triangles as emitters (pairs that make a parallelogram as one). The shaders (kin_area_light.inc.glsl) integrate each
// emitter along a few lines, each line's shadowed stretches exact (every occluder's blocked interval along it, from
// Moller-Trumbore's numerators, which are affine in the place along an emitter for a fixed receiver, and the same in a
// mesh's own space: a copy's triangles are tested there, against the shaft carried into it).
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <span>
#include <vector>

namespace kin::gpu {

struct AreaLightTriangle {
    Vec3f a, b, c;
    bool closed = false; // of a closed mesh (area_light_closed): the shaders skip it when the receiver is behind it
};

// An emissive triangle, world, wound so that cross(b - a, c - a) is the side it emits toward (both when two_sided).
struct AreaLightEmitterTriangle {
    Vec3f a, b, c;
    Vec3f radiance;     // linear; at strength one when gi_emitter is set (the strength comes from the GI's emitters)
    u32 gi_emitter = 0; // the GI scene's emitter number (from 1), 0 for none
    bool two_sided = false;
    u64 group = 0; // triangles of one copy of one draw: only those are paired into parallelograms
};

// The GPU layout (std430, all vec4s; kin_area_light.inc.glsl):
//   triangle (4 vec4): a.xyz, dot(a, N); e0 = b - a; e1 = c - a; N = cross(e0, e1) (not unit), 1 for a closed mesh's
//   node (2 vec4): lo.xyz, first (bits); hi.xyz, count (bits). count 0: inside, children first and first + 1; else a
//                  leaf of `count` triangles (a mesh's hierarchy) or copies (the copies') from `first`
//   copy (7 vec4): the world to its mesh's space (3 rows: xyz, w the offset); its mesh's to the world's (3 columns, w
//   0:
//                  a normal's turn into the mesh's space is their dots); its mesh's first node and first triangle
//                  (bits), 1 when that hierarchy is in world space already
//   emitter (5 vec4): P0.xyz, kind (0 parallelogram, 1 triangle); P1.xyz, GI emitter; P2.xyz, jacobian;
//                     unit normal, two-sided; radiance.rgb, 0. A point is P0 + x1 P1 + x2 P2: a parallelogram's
//                     x1, x2 in [0,1]^2 (P2 its longer side, along which its lines run; jacobian = area); a triangle's
//                     x1 = t, x2 = s t with s, t in [0,1] (lines along P2, its longest side; jacobian = 2 area).
// A mesh's nodes and triangles count from its own first (the copy's); the copies' hierarchy's from 0, then the copies.
constexpr u32 area_light_floats_triangle = 16, area_light_floats_node = 8, area_light_floats_copy = 28,
              area_light_floats_emitter = 20;
constexpr u32 area_light_max_depth = 30; // kin_area_light.inc.glsl's stacks (32) hold a walk this deep

// One mesh's hierarchy, its space (binned surface area heuristic, at most 4 triangles a leaf).
struct AreaLightMesh {
    std::vector<f32> triangles, nodes;
    u32 triangle_count = 0, node_count = 0, depth = 0;
};
AreaLightMesh build_area_light_mesh(std::span<const AreaLightTriangle> triangles);

// A mesh's triangles (corners, three a triangle) as a coarser occluder of about `target` triangles (meshoptimizer's
// simplifier on its welded positions, at most a tenth of its size off, its open edges kept): the same corners when
// they are no more than that, or cannot be simplified.
std::vector<Vec3f> simplify_area_light_occluder(std::span<const Vec3f> corners, std::size_t target);

// Whether a mesh's triangles (corners, three a triangle) close a volume: with their positions welded, each edge is one
// triangle's one way and one other's the other way (watertight, wound alike).
bool area_light_closed(std::span<const Vec3f> corners);

// A copy of a mesh: where it is, and where its mesh's hierarchy starts in the shared buffers.
struct AreaLightCopy {
    Mat4f to_world;
    Vec3f lo, hi; // its world box
    u32 first_node = 0, first_triangle = 0;
    bool world = false; // its mesh's hierarchy is in world space already (to_world the identity)
};

// The copies' hierarchy (at most 2 copies a leaf) and the copies in its order: the scene buffer (nodes, then copies).
struct AreaLightCopies {
    std::vector<f32> data;
    u32 node_count = 0, copy_count = 0, depth = 0;
};
AreaLightCopies build_area_light_copies(std::span<const AreaLightCopy> copies);

struct AreaLightEmitters {
    std::vector<f32> data;
    u32 count = 0;
    std::vector<u32> explicit_emitters; // GI emitter numbers lit here (all their triangles are emitters here)
};
// At most `max_emitters`; a GI emitter some of whose triangles do not fit is left to the probes entirely, and one
// without a GI number always is; so is one whose largest piece (a triangle or a parallelogram) is less than
// `min_size` across (the square root of its area): a candle's flame, an eye, a firefly, which the voxels hold about as
// well and whose shadows would be finer than them.
AreaLightEmitters build_area_light_emitters(std::span<const AreaLightEmitterTriangle> emitters, u32 max_emitters,
                                            f32 min_size = 0.0f);

// The pieces (parallelograms two triangles make, or triangles) a mesh's emissive triangles (corners, three a triangle)
// make as build_area_light_emitters pairs them: how many, and the area of the largest (an affine map keeps the pairs,
// and grows each area by at most the product of its two largest singular values).
struct AreaLightPieces {
    f32 largest = 0.0f;
    u32 count = 0;
};
AreaLightPieces area_light_pieces(std::span<const Vec3f> corners);

// Their reflections (kin_material.inc.glsl, kin_area_specular): linearly transformed cosines' two 64 x 64 RGBA tables
// for GGX (gpu_area_lights_ltc.cpp), as a storage buffer.
constexpr u32 area_light_ltc_floats = 2 * 64 * 64 * 4;
extern const float area_light_ltc[area_light_ltc_floats];

} // namespace kin::gpu
