// kin's GI scene (docs/3d_plan.md, G5a): one draw's triangles into the voxels they touch. Built twice from this file:
// gi_voxelize.comp gives each touched voxel its owner, the least item touching it, an emissive one before any other
// (an item is a copy's triangle, numbered in the order the scene draws them, so the same scene gives the same owners
// whatever order the GPU runs in; a lamp's panel just under a ceiling keeps its voxels, and its light), and
// gi_voxelize_attributes.comp (KIN_GI_ATTRIBUTES) has each owner write its voxel's colour, normal and emission. The
// first also marks, in a second word a voxel, which ways the faces in it look (a bit for each of the six axis
// directions, the one nearest each face's normal): a voxel with faces looking both ways along an axis (a wall thinner
// than a voxel) is taken as double-sided. A triangle touches a voxel when its plane passes through the voxel's box and
// the box's three axis-aligned projections overlap the triangle's (Schwarz and Seidel's conservative surface
// voxelization, 2010): every voxel it
// passes through, so no ray gets between them. A thread an item; a triangle over more than a few columns of voxels
// (along the axis its normal is nearest) is shared by its workgroup's threads, a column each. One level's window at a
// time, and only the voxels of its blocks of 8 x 8 x 8 marked to be made again (`Work`, a bit a stored block): the
// rest keep what they have (G5c). A workgroup a run of 64 of a copy's triangles, those listed (`Work` too: the runs
// that reach the blocks to make).
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler2D uBase;     // the material's base colour (white without)
layout(set = 0, binding = 1) uniform sampler2D uEmissive; // the emissive map (any texture without: p.flags.w)
layout(std430, set = 0, binding = 2) readonly buffer Vertices {
    uint vertices[]; // GpuMeshVertex: position, normal, uv (floats), colour (sRGB bytes): 9 words each
};
layout(std430, set = 0, binding = 3) readonly buffer Indices {
    uint indices[];
};
layout(std430, set = 0, binding = 4) readonly buffer Skin {
    uvec2 skin[]; // GpuSkinVertex: four joint indices, four weights (unorm), a byte each
};
layout(std430, set = 0, binding = 5) readonly buffer Joints {
    mat4 joints[];
};
struct Copy {
    mat4 model;
    vec4 normal0, normal1, normal2; // the columns of the normals' matrix (the cofactors, as GpuInstance3D's)
    vec4 colour;                    // the material's colour times the copy's tint, linear
    uvec4 skin;                     // x: its first joint matrix in Joints
};
layout(std430, set = 0, binding = 6) readonly buffer Copies {
    Copy copies[];
};
layout(std430, set = 0, binding = 7) readonly buffer Work {
    uint work[]; // each level's bits, a stored block each, then the runs: a copy (of the draw's) and a run, each
};
#ifdef KIN_GI_ATTRIBUTES
layout(std430, set = 0, binding = 8) readonly buffer Owners {
    uint owners[];
};
layout(std430, set = 0, binding = 9) readonly buffer Sides {
    uint sides[];
};
layout(set = 1, binding = 0, rgba8) uniform writeonly image3D uAlbedo;
layout(set = 1, binding = 1, rgba8) uniform writeonly image3D uNormal;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image3D uEmission;
#else
layout(std430, set = 1, binding = 0) buffer Owners {
    uint owners[]; // ~0: no surface
};
layout(std430, set = 1, binding = 1) buffer Sides {
    uint sides[];
};
#endif

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 origin;   // w: voxels a unit (1 / cell); xyz: 0 (the voxels are counted from the world's origin)
    uvec4 size;    // xyz: voxels a window; w: the draw's first item
    uvec4 mesh;    // x: its first index in Indices, y: its triangles, z: its base vertex, w: its copies
    uvec4 flags;   // x: skinned, y: a cut-out, z: the textures hold sRGB bytes (decode them), w: an emissive map
    vec4 material; // x: emissive strength, y: alpha cutoff, z: 1 double-sided, w: its emitter's number (0: none: the
                   // strength is in the voxel)
    uvec4 first;   // x: the draw's first copy in Copies; y: the level's first word in Work; z: the draw's first run's,
                   // w: its runs
    ivec4 window;  // xyz: the window's first voxel (in the level's voxels); w: the level's offset along z
    ivec4 low;     // xyz: the first voxel of the box round the blocks to make (the world's voxels)
    ivec4 high;    // xyz: its last
    vec4 emission; // xyz: linear factor, w: independent emission
} p;

#include "kin_srgb.inc.glsl"
#include "kin_gi_address.inc.glsl"

// A triangle in voxels (a unit a voxel, voxel (0, 0, 0) from the world's origin: what a voxel holds does not depend on
// where the window is, so a window that moves keeps what it made), with what the touch test needs: its normal
// (not unit), the plane's offsets at the box's two critical corners, the edges' normals and offsets in each of the
// three projections, the voxels its box covers, and the axis its normal is nearest (`w`) with the other two.
struct Tri {
    vec3 v0, v1, v2;
    vec3 n;
    float d1, d2;
    vec2 nxy[3], nyz[3], nzx[3];
    float dxy[3], dyz[3], dzx[3];
    ivec3 lo, hi;
    int w, u, v;
};
// What the attributes need of its corners (world space).
struct Corners {
    vec3 normal[3];
    vec2 uv[3];
    vec4 colour[3]; // linear
    uint copy;
};

void corner(uint copy, uint index, out vec3 position, out vec3 normal, out vec2 uv, out vec4 colour) {
    const uint vertex = p.mesh.z + index;
    const uint at = vertex * 9u;
    position = vec3(uintBitsToFloat(vertices[at]), uintBitsToFloat(vertices[at + 1u]), uintBitsToFloat(vertices[at + 2u]));
    normal =
        vec3(uintBitsToFloat(vertices[at + 3u]), uintBitsToFloat(vertices[at + 4u]), uintBitsToFloat(vertices[at + 5u]));
    uv = vec2(uintBitsToFloat(vertices[at + 6u]), uintBitsToFloat(vertices[at + 7u]));
    colour = unpackUnorm4x8(vertices[at + 8u]);
    colour.rgb = decode(colour.rgb);
    if (p.flags.x != 0u) {
        // Posed as kin_skin.inc.glsl poses it: the weighted sum of the vertex's joints' matrices.
        const uvec2 s = skin[vertex];
        const uint first = copies[copy].skin.x;
        const vec4 weight = unpackUnorm4x8(s.y);
        const mat4 m = joints[first + (s.x & 255u)] * weight.x + joints[first + ((s.x >> 8u) & 255u)] * weight.y +
                       joints[first + ((s.x >> 16u) & 255u)] * weight.z + joints[first + (s.x >> 24u)] * weight.w;
        position = (m * vec4(position, 1.0)).xyz;
        normal = mat3(m) * normal;
    }
    position = (copies[copy].model * vec4(position, 1.0)).xyz;
    normal = mat3(copies[copy].normal0.xyz, copies[copy].normal1.xyz, copies[copy].normal2.xyz) * normal;
}

// Item `id` (of this draw): its triangle in voxels and its corners. False when it touches no voxel to make (no area,
// or its box is outside the box round the blocks to make, in the window).
bool load(uint id, out Tri t, out Corners k) {
    const uint nth = id / p.mesh.y;
    const uint triangle = id - nth * p.mesh.y;
    const uint copy = p.first.x + nth;
    const uint first = p.mesh.x + triangle * 3u;
    vec3 position[3];
    for (uint i = 0u; i < 3u; ++i) {
        corner(copy, indices[first + i], position[i], k.normal[i], k.uv[i], k.colour[i]);
        position[i] *= p.origin.w;
    }
    k.copy = copy;
    t.v0 = position[0];
    t.v1 = position[1];
    t.v2 = position[2];
    const vec3 e[3] = vec3[3](t.v1 - t.v0, t.v2 - t.v1, t.v0 - t.v2);
    t.n = cross(e[0], e[1]);
    if (!(dot(t.n, t.n) > 0.0)) {
        return false; // no area (or not finite)
    }
    const vec3 lo = min(t.v0, min(t.v1, t.v2)), hi = max(t.v0, max(t.v1, t.v2));
    const ivec3 from = max(p.window.xyz, p.low.xyz), to = min(p.window.xyz + ivec3(p.size.xyz) - 1, p.high.xyz);
    if (any(lessThan(hi, vec3(from))) || any(greaterThanEqual(lo, vec3(to + 1)))) {
        return false;
    }
    t.lo = max(ivec3(floor(lo)), from);
    t.hi = min(ivec3(floor(hi)), to);
    const vec3 critical = vec3(greaterThan(t.n, vec3(0.0)));
    t.d1 = dot(t.n, critical - t.v0);
    t.d2 = dot(t.n, (vec3(1.0) - critical) - t.v0);
    const float sx = t.n.x >= 0.0 ? 1.0 : -1.0, sy = t.n.y >= 0.0 ? 1.0 : -1.0, sz = t.n.z >= 0.0 ? 1.0 : -1.0;
    for (int i = 0; i < 3; ++i) {
        const vec3 from = i == 0 ? t.v0 : i == 1 ? t.v1 : t.v2;
        t.nxy[i] = vec2(-e[i].y, e[i].x) * sz;
        t.dxy[i] = -dot(t.nxy[i], from.xy) + max(0.0, t.nxy[i].x) + max(0.0, t.nxy[i].y);
        t.nyz[i] = vec2(-e[i].z, e[i].y) * sx;
        t.dyz[i] = -dot(t.nyz[i], from.yz) + max(0.0, t.nyz[i].x) + max(0.0, t.nyz[i].y);
        t.nzx[i] = vec2(-e[i].x, e[i].z) * sy;
        t.dzx[i] = -dot(t.nzx[i], from.zx) + max(0.0, t.nzx[i].x) + max(0.0, t.nzx[i].y);
    }
    const vec3 a = abs(t.n);
    t.w = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
    t.u = (t.w + 1) % 3;
    t.v = (t.w + 2) % 3;
    return true;
}

bool touches(Tri t, ivec3 voxel) {
    const vec3 q = vec3(voxel);
    const float np = dot(t.n, q);
    if ((np + t.d1) * (np + t.d2) > 0.0) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (dot(t.nxy[i], q.xy) + t.dxy[i] < 0.0 || dot(t.nyz[i], q.yz) + t.dyz[i] < 0.0 ||
            dot(t.nzx[i], q.zx) + t.dzx[i] < 0.0) {
            return false;
        }
    }
    return true;
}

// The barycentric weights of the triangle's point nearest `q` (Ericson, Real-Time Collision Detection, 5.1.5).
vec3 nearest(vec3 q, vec3 a, vec3 b, vec3 c) {
    const vec3 ab = b - a, ac = c - a, aq = q - a;
    const float d1 = dot(ab, aq), d2 = dot(ac, aq);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return vec3(1.0, 0.0, 0.0);
    }
    const vec3 bq = q - b;
    const float d3 = dot(ab, bq), d4 = dot(ac, bq);
    if (d3 >= 0.0 && d4 <= d3) {
        return vec3(0.0, 1.0, 0.0);
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const float s = d1 / (d1 - d3);
        return vec3(1.0 - s, s, 0.0);
    }
    const vec3 cq = q - c;
    const float d5 = dot(ab, cq), d6 = dot(ac, cq);
    if (d6 >= 0.0 && d5 <= d6) {
        return vec3(0.0, 0.0, 1.0);
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const float s = d2 / (d2 - d6);
        return vec3(1.0 - s, 0.0, s);
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        const float s = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return vec3(0.0, 1.0 - s, s);
    }
    const float denom = 1.0 / (va + vb + vc);
    const float s = vb * denom, r = vc * denom;
    return vec3(1.0 - s - r, s, r);
}

// What the voxel sees of the surface: its point of the triangle nearest the voxel's centre (barycentric), the UV
// there, and the mip of a texture `texels` big whose texels are the voxel's size on the triangle.
void surface_at(Tri t, Corners k, ivec3 voxel, out vec3 b, out vec2 uv) {
    b = nearest(vec3(voxel) + 0.5, t.v0, t.v1, t.v2);
    uv = k.uv[0] * b.x + k.uv[1] * b.y + k.uv[2] * b.z;
}
float mip_for(Tri t, Corners k, vec2 texels) {
    const vec2 a = k.uv[1] - k.uv[0], c = k.uv[2] - k.uv[0];
    const float uv_area = abs(a.x * c.y - a.y * c.x) * texels.x * texels.y; // texels, twice the triangle's
    const float voxel_area = length(t.n);                                   // voxels, twice the triangle's
    return max(0.5 * log2(max(uv_area / voxel_area, 1e-20)), 0.0);
}

// The base colour (linear) and alpha at the voxel's point.
vec4 base_at(Tri t, Corners k, vec3 b, vec2 uv) {
    const vec4 vertex = k.colour[0] * b.x + k.colour[1] * b.y + k.colour[2] * b.z;
    vec4 tex = textureLod(uBase, uv, mip_for(t, k, vec2(textureSize(uBase, 0))));
    if (p.flags.z != 0u) {
        tex.rgb = decode(tex.rgb);
    }
    return copies[k.copy].colour * vertex * tex;
}

// Voxel `voxel` (the world's voxels, in the window) touched by item `item`.
void visit(Tri t, Corners k, ivec3 voxel, uint item) {
    const ivec3 size = ivec3(p.size.xyz);
    const ivec3 st = kin_gi_wrap(voxel, size);
    const ivec3 bricks = size >> 3, brick = st >> 3;
    const uint block = uint(brick.x + bricks.x * (brick.y + bricks.y * brick.z));
    if ((work[p.first.y + (block >> 5u)] & (1u << (block & 31u))) == 0u) {
        return; // kept as it is
    }
    const ivec3 stored = ivec3(st.xy, st.z + p.window.w);
    const uint at = uint(stored.x) + p.size.x * (uint(stored.y) + p.size.y * uint(stored.z));
    item |= p.material.x > 0.0 ? 0u : 0x80000000u; // the emissive first
#ifdef KIN_GI_ATTRIBUTES
    if (owners[at] != item) {
        return;
    }
    vec3 b;
    vec2 uv;
    surface_at(t, k, voxel, b, uv);
    const vec4 base = base_at(t, k, b, uv);
    // The face's normal (the voxel space's axes are the world's), on the side its vertices' normals are.
    vec3 n = normalize(t.n);
    const vec3 smooth_normal = k.normal[0] * b.x + k.normal[1] * b.y + k.normal[2] * b.z;
    if (dot(n, smooth_normal) < 0.0) {
        n = -n;
    }
    vec3 emission = vec3(0.0);
    if (p.material.x > 0.0) {
        vec3 own = p.emission.w > 0.5 ? vec3(1.0) : base.rgb;
        if (p.flags.w != 0u) {
            own = textureLod(uEmissive, uv, mip_for(t, k, vec2(textureSize(uEmissive, 0)))).rgb;
            if (p.flags.z != 0u) {
                own = decode(own);
            }
        }
        if (p.emission.w > 0.5) {
            own *= p.emission.rgb;
        }
        // With an emitter, the colour alone: the GI's light takes the strength from the emitter's (gi_light.comp).
        emission = p.material.w > 0.5 ? own : own * p.material.x;
    }
    // The base colour, metals' too: a metal reflects its colour's share of the light, here as if diffuse (the GI's
    // light is blurred), which keeps a furnace's metal as bright as its sky.
    imageStore(uAlbedo, stored, vec4(clamp(base.rgb, 0.0, 1.0), 1.0));
    const uint ways = sides[at];
    const bool both_ways = ((ways & 0x15u) & ((ways >> 1u) & 0x15u)) != 0u;
    imageStore(uNormal, stored, vec4(n * 0.5 + 0.5, p.material.z > 0.5 || both_ways ? 1.0 : 128.0 / 255.0));
    imageStore(uEmission, stored, vec4(min(emission, vec3(65000.0)), p.material.w));
#else
    if (p.flags.y != 0u) {
        vec3 b;
        vec2 uv;
        surface_at(t, k, voxel, b, uv);
        if (base_at(t, k, b, uv).a < p.material.y) {
            return; // a cut-out's hole
        }
    }
    atomicMin(owners[at], item);
    const vec3 facing = k.normal[0] + k.normal[1] + k.normal[2];
    const vec3 face = dot(t.n, facing) < 0.0 ? -t.n : t.n;
    const vec3 a = abs(face);
    const uint axis = a.x >= a.y && a.x >= a.z ? 0u : a.y >= a.z ? 1u : 2u;
    atomicOr(sides[at], 1u << (axis * 2u + (face[axis] < 0.0 ? 1u : 0u)));
#endif
}

// The voxels of column `c` (the triangle's u and v) its plane passes through.
void column(Tri t, Corners k, ivec2 c, uint item) {
    const float nu = t.n[t.u], nv = t.n[t.v], nw = t.n[t.w];
    const float plane = dot(t.n, t.v0);
    const float u0 = float(c.x), u1 = float(c.x + 1), v0 = float(c.y), v1 = float(c.y + 1);
    const float a = (plane - nu * u0 - nv * v0) / nw, b = (plane - nu * u1 - nv * v0) / nw;
    const float d = (plane - nu * u0 - nv * v1) / nw, e = (plane - nu * u1 - nv * v1) / nw;
    const int from = max(int(floor(min(min(a, b), min(d, e)) - 1e-3)), t.lo[t.w]);
    const int to = min(int(floor(max(max(a, b), max(d, e)) + 1e-3)), t.hi[t.w]);
    for (int w = from; w <= to; ++w) {
        ivec3 voxel;
        voxel[t.u] = c.x;
        voxel[t.v] = c.y;
        voxel[t.w] = w;
        if (touches(t, voxel)) {
            visit(t, k, voxel, item);
        }
    }
}

// The columns the triangle's box spans along its two minor axes.
uint columns(Tri t) {
    const ivec3 extent = t.hi - t.lo + 1;
    return uint(extent[t.u]) * uint(extent[t.v]);
}
ivec2 column_at(Tri t, uint i) {
    const uint across = uint(t.hi[t.u] - t.lo[t.u] + 1);
    return ivec2(t.lo[t.u] + int(i % across), t.lo[t.v] + int(i / across));
}

shared uint shared_items[64];
shared uint shared_count;

void main() {
    const uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if (group >= p.first.w) {
        return; // the whole workgroup
    }
    const uint triangle = work[p.first.z + group * 2u + 1u] * 64u + gl_LocalInvocationIndex;
    const uint id = work[p.first.z + group * 2u] * p.mesh.y + triangle;
    if (gl_LocalInvocationIndex == 0u) {
        shared_count = 0u;
    }
    memoryBarrierShared();
    barrier();
    if (triangle < p.mesh.y) {
        Tri t;
        Corners k;
        if (load(id, t, k)) {
            const uint n = columns(t);
            if (n <= 8u) {
                for (uint i = 0u; i < n; ++i) {
                    column(t, k, column_at(t, i), p.size.w + id);
                }
            } else {
                shared_items[atomicAdd(shared_count, 1u)] = id;
            }
        }
    }
    memoryBarrierShared();
    barrier();
    const uint wide = shared_count;
    for (uint s = 0u; s < wide; ++s) {
        const uint shared_id = shared_items[s];
        Tri t;
        Corners k;
        load(shared_id, t, k);
        const uint n = columns(t);
        for (uint i = gl_LocalInvocationIndex; i < n; i += 64u) {
            column(t, k, column_at(t, i), p.size.w + shared_id);
        }
    }
}
