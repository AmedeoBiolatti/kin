#pragma once

// The light baker's rays (baked_lighting3d.cpp): a bounding volume hierarchy over a world-space triangle soup, built
// with binned SAH, walked for the nearest hit or for any hit. Private to the engine; tests include it directly.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <span>
#include <vector>

namespace kin::bake {

struct RayHit {
    f32 t = 0.0f;
    u32 triangle = 0; // the index given to build()
    f32 u = 0.0f;     // barycentrics: the point is a + u (b - a) + v (c - a)
    f32 v = 0.0f;
    bool back = false; // the ray met the triangle's back (its winding clockwise as the ray sees it)
};

class TriangleBvh {
public:
    // Three corners a triangle (counter-clockwise seen from its front). Degenerate ones are kept (never hit).
    void build(std::span<const Vec3f> corners);

    // The nearest hit with t in (0, t_max), if any.
    bool nearest(Vec3f origin, Vec3f direction, f32 t_max, RayHit& hit) const;
    // Whether anything is hit with t in (0, t_max).
    bool any(Vec3f origin, Vec3f direction, f32 t_max) const;

    u32 triangle_count() const { return static_cast<u32>(_order.size()); }
    u32 node_count() const { return static_cast<u32>(_nodes.size()); }

private:
    struct Node {
        Vec3f lo;
        u32 first = 0; // a leaf's first triangle in _tris, or an inner node's first child (its second is next)
        Vec3f hi;
        u32 count = 0; // triangles in a leaf; 0: an inner node
    };
    struct Tri {
        Vec3f a, e1, e2;
    };
    std::vector<Node> _nodes;
    std::vector<Tri> _tris;  // in leaf order
    std::vector<u32> _order; // _tris[i] is triangle _order[i]
};

} // namespace kin::bake
