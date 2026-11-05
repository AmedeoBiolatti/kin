// Area lights (GlobalIllumination3D::area_lights; gpu_area_lights3d.hpp): the light an emitter gives a point, each
// emitter integrated along KIN_AREA_LINES lines across it, every line's shadowed stretches exact. For a fixed receiver o
// and an emitter point y = P0 + x1 P1 + x2 P2, Moller-Trumbore's numerators and determinant for the segment o -> y are
// affine in (x1, x2), so an occluder triangle blocks the points where five affine forms are all >= 0: on a line (x1
// fixed) one interval. Occluders come from a bounding volume hierarchy, walked with the shaft from o to the emitter.
// Two levels: the copies' hierarchy (world space) over each mesh's own (its space); a copy's triangles are tested
// against the shaft carried into its mesh's space (an affine map keeps every interval the same).
// With KIN_AREA_EXACT_FORM the result is the emitter's exact form factor times the share the lines find unshadowed
// (their quadrature's error cancels in it); else the lines' light itself.
// The including shader declares, before including this:
//   readonly buffers kin_area_triangles[] and kin_area_nodes[] (the meshes', vec4 x 4 a triangle, x 2 a node),
//   kin_area_scene[] (the copies' nodes, then the copies, vec4 x 7 each), kin_area_emitters[] (vec4 x 5), the
//   function float kin_area_strength(uint gi_emitter) (the GI emitter's strength this render), and KIN_AREA_LINES
//   (and KIN_AREA_EXACT_FORM, or not).

#ifndef KIN_AREA_LINES
#define KIN_AREA_LINES 4
#endif
const int KIN_AREA_SLOTS = 4; // blocked intervals kept a line (overlapping ones merged; more join their nearest)
const int KIN_AREA_STACK = 32;

// Gauss nodes and weights on [0, 1]: Legendre (a parallelogram's lines) and Jacobi with weight t (a triangle's, whose
// lines grow with t).
#if KIN_AREA_LINES == 2
const float kin_area_legendre_x[2] = float[2](0.211324871, 0.788675129);
const float kin_area_legendre_w[2] = float[2](0.5, 0.5);
const float kin_area_jacobi_x[2] = float[2](0.355051011, 0.844948947);
const float kin_area_jacobi_w[2] = float[2](0.181958616, 0.318041384);
#else
const float kin_area_legendre_x[4] = float[4](0.0694318414, 0.330009490, 0.669990540, 0.930568159);
const float kin_area_legendre_w[4] = float[4](0.173927426, 0.326072574, 0.326072574, 0.173927426);
const float kin_area_jacobi_x[4] = float[4](0.139759868, 0.416409582, 0.723156989, 0.942895830);
const float kin_area_jacobi_w[4] = float[4](0.0311809704, 0.129847541, 0.203464568, 0.135506913);
#endif

// Merge on insert, with constant indices only (the buffer stays in registers). Unused slots hold (2, 2).
void kin_area_insert(inout vec2 slots[KIN_AREA_SLOTS], inout int n, vec2 r) {
    int best = -1;
    float gap = 1e30;
    for (int k = 0; k < KIN_AREA_SLOTS; ++k) {
        const float g = max(slots[k].x - r.y, r.x - slots[k].y);
        if (k < n && g < gap) {
            gap = g;
            best = k;
        }
    }
    // Overlapping blockers share a slot; when full, the nearest takes it (over-blocks the gap, never drops a blocker).
    const bool merge = best >= 0 && (gap <= 1e-5 || n == KIN_AREA_SLOTS);
    for (int k = 0; k < KIN_AREA_SLOTS; ++k) {
        if (merge && k == best) {
            slots[k] = vec2(min(slots[k].x, r.x), max(slots[k].y, r.y));
        } else if (!merge && k == n) {
            slots[k] = r;
        }
    }
    if (!merge) {
        ++n;
    }
}

// Sorts a line's slots by start (odd-even transposition: constant indices only).
void kin_area_sort(inout vec2 s[KIN_AREA_SLOTS]) {
    for (int pass = 0; pass < KIN_AREA_SLOTS; ++pass) {
        for (int k = 0; k + 1 < KIN_AREA_SLOTS; ++k) {
            if (((k ^ pass) & 1) == 0 && s[k].x > s[k + 1].x) {
                const vec2 t = s[k];
                s[k] = s[k + 1];
                s[k + 1] = t;
            }
        }
    }
}

// The light from a stretch [from, to] of the line A + u L, by 2 Gauss points.
vec3 kin_area_stretch(float from, float to, vec3 point, vec3 n, vec3 A, vec3 L, vec3 ne, bool two_sided, vec3 power) {
    vec3 light = vec3(0.0);
    const float span = to - from;
    for (int g = 0; g < 2; ++g) {
        const vec3 d = A + (from + span * (g == 0 ? 0.211324865 : 0.788675135)) * L - point;
        const float r2 = max(dot(d, d), 1e-20);
        const vec3 way = d * inversesqrt(r2);
        const float e = two_sided ? abs(dot(way, ne)) : max(0.0, -dot(way, ne));
        light += power * (span * 0.5 * e * max(0.0, dot(way, n)) / r2);
    }
    return light;
}

// Whether the box lo..hi lies wholly on the negative side of the plane through o with normal m (less `tolerance`).
bool kin_area_outside(vec3 m, float tolerance, vec3 o, vec3 lo, vec3 hi) {
    return dot(m, mix(lo, hi, greaterThan(m, vec3(0.0))) - o) < -tolerance;
}

// An emitter as the shaders use it: its frame, its corners round it (a triangle's third twice), its light.
struct KinAreaEmitter {
    vec3 P0, P1, P2, ne, radiance;
    vec3 c0, c1, c2, c3;
    float jacobian;
    bool triangle, two_sided;
};

KinAreaEmitter kin_area_load(uint e) {
    const vec4 E0 = kin_area_emitters[e * 5u], E1 = kin_area_emitters[e * 5u + 1u], E2 = kin_area_emitters[e * 5u + 2u];
    const vec4 EN = kin_area_emitters[e * 5u + 3u], ER = kin_area_emitters[e * 5u + 4u];
    KinAreaEmitter m;
    m.P0 = E0.xyz;
    m.P1 = E1.xyz;
    m.P2 = E2.xyz;
    m.ne = EN.xyz;
    m.triangle = E0.w > 0.5;
    m.two_sided = EN.w > 0.5;
    m.jacobian = E2.w;
    const uint gi = uint(E1.w + 0.5);
    m.radiance = ER.rgb * (gi > 0u ? kin_area_strength(gi) : 1.0);
    m.c0 = m.P0;
    m.c1 = m.P0 + m.P1;
    m.c2 = m.P0 + m.P1 + m.P2;
    m.c3 = m.triangle ? m.c2 : m.P0 + m.P2;
    return m;
}

// The emitter's form factor seen from `point` facing `n` (its light over pi, per unit radiance), exact: its polygon
// clipped to the horizon, Lambert's sum over the edges (each edge's angle times its plane's normal along n).
float kin_area_form_factor(KinAreaEmitter m, vec3 point, vec3 n) {
    const int count = m.triangle ? 3 : 4;
    const vec3 corners[4] = vec3[4](m.c0 - point, m.c1 - point, m.c2 - point, m.c3 - point);
    vec3 kept[5];
    int k = 0;
    for (int i = 0; i < 4; ++i) {
        if (i < count) {
            const vec3 a = corners[i], b = corners[(i + 1) % count];
            const float da = dot(a, n), db = dot(b, n);
            if (da > 0.0) {
                kept[k++] = a;
            }
            if ((da > 0.0) != (db > 0.0)) {
                kept[k++] = a + (b - a) * (da / (da - db)); // where the edge crosses the horizon
            }
        }
    }
    float f = 0.0;
    for (int i = 0; i < 5; ++i) {
        if (i < k) {
            const vec3 u = normalize(kept[i]), v = normalize(kept[(i + 1) % k]);
            const vec3 c = cross(u, v);
            const float s = length(c);
            f += s > 1e-12 ? dot(c, n) * atan(s, dot(u, v)) / s : 0.0;
        }
    }
    return abs(f) * (0.5 / 3.14159265);
}

// Whether `point` facing `n` can have any of its light: in front of a one-sided one, some of it above the horizon.
bool kin_area_wants(KinAreaEmitter m, vec3 point, vec3 n) {
    if (dot(m.radiance, vec3(1.0)) <= 0.0 || (!m.two_sided && dot(point - m.P0, m.ne) <= 0.0)) {
        return false;
    }
    return max(max(dot(m.c0 - point, n), dot(m.c1 - point, n)), max(dot(m.c2 - point, n), dot(m.c3 - point, n))) > 0.0;
}

// The shaft from o to an emitter: its box, the planes through o and each edge (inward; a triangle's fourth is 0, which
// never culls), each with its tolerance, the side of the emitter's plane o is on, and the receiver's normal.
struct KinAreaShaft {
    vec3 o, n, lo, hi, beyond;
    vec3 m0, m1, m2, m3;
    vec4 tolerance;
};

KinAreaShaft kin_area_shaft(KinAreaEmitter m, vec3 o, vec3 n) {
    KinAreaShaft s;
    s.o = o;
    s.n = n;
    s.lo = min(o, min(min(m.c0, m.c1), min(m.c2, m.c3)));
    s.hi = max(o, max(max(m.c0, m.c1), max(m.c2, m.c3)));
    const vec3 centre = (m.c0 + m.c1 + m.c2 + m.c3) * 0.25;
    const float reach = max(max(s.hi.x - s.lo.x, s.hi.y - s.lo.y), s.hi.z - s.lo.z);
    s.m0 = cross(m.c0 - o, m.c1 - o);
    s.m1 = cross(m.c1 - o, m.c2 - o);
    s.m2 = cross(m.c2 - o, (m.triangle ? m.c0 : m.c3) - o);
    s.m3 = m.triangle ? vec3(0.0) : cross(m.c3 - o, m.c0 - o);
    s.m0 = dot(s.m0, centre - o) < 0.0 ? -s.m0 : s.m0;
    s.m1 = dot(s.m1, centre - o) < 0.0 ? -s.m1 : s.m1;
    s.m2 = dot(s.m2, centre - o) < 0.0 ? -s.m2 : s.m2;
    s.m3 = dot(s.m3, centre - o) < 0.0 ? -s.m3 : s.m3;
    s.tolerance = 1e-5 * reach * vec4(length(s.m0), length(s.m1), length(s.m2), length(s.m3));
    s.beyond = dot(o - m.P0, m.ne) >= 0.0 ? m.ne : -m.ne; // toward o's side of the emitter
    return s;
}

// Whether a box can hold a blocker of the shaft: inside its box, not wholly past an edge plane, behind the receiver
// (nothing there shades what it can see), past the emitter's plane or behind o's plane along it (nothing there is
// between: every shadow segment runs from o's plane to the emitter's, so nothing wholly outside that slab blocks).
bool kin_area_box_in_shaft(KinAreaShaft s, vec3 P0, vec3 lo, vec3 hi) {
    return !(any(lessThan(s.hi, lo)) || any(greaterThan(s.lo, hi)) || kin_area_outside(s.m0, s.tolerance.x, s.o, lo, hi) ||
             kin_area_outside(s.m1, s.tolerance.y, s.o, lo, hi) || kin_area_outside(s.m2, s.tolerance.z, s.o, lo, hi) ||
             kin_area_outside(s.m3, s.tolerance.w, s.o, lo, hi) || kin_area_outside(s.n, 0.0, s.o, lo, hi) ||
             kin_area_outside(s.beyond, 0.0, P0, lo, hi) || kin_area_outside(-s.beyond, 0.0, s.o, lo, hi));
}

// Whether the triangle a, b, c lies wholly on the negative side of the plane through `base` with normal m (less
// `tolerance`): then no shadow segment of a shaft bounded by that plane meets it.
bool kin_area_triangle_outside(vec3 m, float tolerance, vec3 base, vec3 a, vec3 b, vec3 c) {
    return max(dot(m, a - base), max(dot(m, b - base), dot(m, c - base))) < -tolerance;
}

float kin_area_x1(KinAreaEmitter m, int k) {
    return m.triangle ? kin_area_jacobi_x[k] : kin_area_legendre_x[k];
}

// The emitter in a copy's mesh's space (`r0`..`r2`: the world to it, rows).
KinAreaEmitter kin_area_into(KinAreaEmitter m, vec4 r0, vec4 r1, vec4 r2) {
    const mat3 l = transpose(mat3(r0.xyz, r1.xyz, r2.xyz));
    const vec3 offset = vec3(r0.w, r1.w, r2.w);
    KinAreaEmitter c = m;
    c.P0 = l * m.P0 + offset;
    c.P1 = l * m.P1;
    c.P2 = l * m.P2;
    c.ne = cross(c.P1, c.P2); // either side: kin_area_shaft takes o's
    c.c0 = c.P0;
    c.c1 = c.P0 + c.P1;
    c.c2 = c.P0 + c.P1 + c.P2;
    c.c3 = c.triangle ? c.c2 : c.P0 + c.P2;
    return c;
}

// Irradiance over pi at `point` facing `n` from emitter `e`, shadow rays leaving from `o` (point, a little off it);
// the copies start at kin_area_scene[copies]. One walk: the copies' hierarchy hands over one copy at a time (its
// emitter frame and shaft carried into its mesh's space, or kept for the settled copies' world-space hierarchy), whose
// own hierarchy is walked next; every occluder triangle goes through the one place below.
vec3 kin_area_emitter(uint e, uvec4 scene, vec3 point, vec3 n, vec3 o) {
    const uint copies = scene.x;
    const KinAreaEmitter m = kin_area_load(e);
    if (!kin_area_wants(m, point, n)) {
        return vec3(0.0);
    }
    // (Only the shaft's box is kept for the copies' hierarchy: each copy's shaft is made when it is walked, so one
    // shaft is live at a time.)
    const vec3 shaft_lo = min(o, min(min(m.c0, m.c1), min(m.c2, m.c3)));
    const vec3 shaft_hi = max(o, max(max(m.c0, m.c1), max(m.c2, m.c3)));
    vec2 slots[KIN_AREA_LINES][KIN_AREA_SLOTS];
    int filled[KIN_AREA_LINES];
    float x1[KIN_AREA_LINES], inv_scale[KIN_AREA_LINES];
    for (int k = 0; k < KIN_AREA_LINES; ++k) {
        filled[k] = 0;
        x1[k] = kin_area_x1(m, k);
        inv_scale[k] = m.triangle ? 1.0 / x1[k] : 1.0;
        for (int i = 0; i < KIN_AREA_SLOTS; ++i) {
            slots[k][i] = vec2(2.0);
        }
    }
    // The copy being walked.
    vec3 P0 = m.P0, P1 = m.P1, P2 = m.P2;
    KinAreaShaft cs;
    uint node_base = 0u, triangle_base = 0u, pending = 0u, pending_count = 0u;
    uint outer[KIN_AREA_STACK], inner[KIN_AREA_STACK];
    int outer_top = 0, inner_top = 0;
    outer[outer_top++] = 0u;
    for (;;) {
        if (inner_top == 0) {
            // The next copy in the shaft.
            while (pending_count == 0u && outer_top > 0) {
                const uint node = outer[--outer_top];
                const vec4 nlo = kin_area_scene[node * 2u], nhi = kin_area_scene[node * 2u + 1u];
                if (any(lessThan(shaft_hi, nlo.xyz)) || any(greaterThan(shaft_lo, nhi.xyz))) {
                    continue;
                }
                const uint first = floatBitsToUint(nlo.w), count = floatBitsToUint(nhi.w);
                if (count == 0u) {
                    if (outer_top + 2 <= KIN_AREA_STACK) {
                        outer[outer_top++] = first + 1u;
                        outer[outer_top++] = first;
                    }
                } else {
                    pending = first;
                    pending_count = count;
                }
            }
            if (pending_count == 0u) {
                break;
            }
            const uint at = copies + pending * 7u;
            ++pending;
            --pending_count;
            const vec4 bases = kin_area_scene[at + 6u];
            node_base = floatBitsToUint(bases.x);
            triangle_base = floatBitsToUint(bases.y);
            if (bases.z > 0.5) {
                P0 = m.P0;
                P1 = m.P1;
                P2 = m.P2;
                cs = kin_area_shaft(m, o, n);
            } else {
                const vec4 r0 = kin_area_scene[at], r1 = kin_area_scene[at + 1u], r2 = kin_area_scene[at + 2u];
                const vec3 c0 = kin_area_scene[at + 3u].xyz, c1 = kin_area_scene[at + 4u].xyz,
                           c2 = kin_area_scene[at + 5u].xyz;
                const KinAreaEmitter mm = kin_area_into(m, r0, r1, r2);
                P0 = mm.P0;
                P1 = mm.P1;
                P2 = mm.P2;
                cs = kin_area_shaft(mm, vec3(dot(r0.xyz, o) + r0.w, dot(r1.xyz, o) + r1.w, dot(r2.xyz, o) + r2.w),
                                    vec3(dot(c0, n), dot(c1, n), dot(c2, n)));
            }
            inner[inner_top++] = 0u;
        }
        const uint node = node_base + inner[--inner_top];
        const vec4 nlo = kin_area_nodes[node * 2u], nhi = kin_area_nodes[node * 2u + 1u];
        if (!kin_area_box_in_shaft(cs, P0, nlo.xyz, nhi.xyz)) {
            continue;
        }
        const uint first = floatBitsToUint(nlo.w), count = floatBitsToUint(nhi.w);
        if (count == 0u) {
            if (inner_top + 2 <= KIN_AREA_STACK) {
                inner[inner_top++] = first + 1u;
                inner[inner_top++] = first;
            }
            continue;
        }
        for (uint t = triangle_base + first; t < triangle_base + first + count; ++t) {
            const vec4 A4 = kin_area_triangles[t * 4u];
            const vec4 N4 = kin_area_triangles[t * 4u + 3u];
            const float nt = dot(cs.o, N4.xyz) - A4.w; // t's numerator: the same for every y; < 0: o behind it
            // A closed mesh's face with o behind it: a way out of the volume, whose ways in block the same.
            if (nt == 0.0 || (nt < 0.0 && N4.w > 0.5)) {
                continue;
            }
            const vec3 e0 = kin_area_triangles[t * 4u + 1u].xyz, e1 = kin_area_triangles[t * 4u + 2u].xyz;
            // The triangle itself against the shaft's edge planes and its slab (o's plane to the emitter's): what lies
            // wholly outside one of them has no blocked interval on any line (its node's box passed the rest).
            const vec3 b = A4.xyz + e0, c = A4.xyz + e1;
            if (kin_area_triangle_outside(cs.m0, cs.tolerance.x, cs.o, A4.xyz, b, c) ||
                kin_area_triangle_outside(cs.m1, cs.tolerance.y, cs.o, A4.xyz, b, c) ||
                kin_area_triangle_outside(cs.m2, cs.tolerance.z, cs.o, A4.xyz, b, c) ||
                kin_area_triangle_outside(cs.m3, cs.tolerance.w, cs.o, A4.xyz, b, c) ||
                kin_area_triangle_outside(cs.beyond, 0.0, P0, A4.xyz, b, c) ||
                kin_area_triangle_outside(-cs.beyond, 0.0, cs.o, A4.xyz, b, c)) {
                continue;
            }
            const vec3 N = N4.xyz;
            const float sg = nt > 0.0 ? 1.0 : -1.0;
            const vec3 sv = cs.o - A4.xyz, q = cross(sv, e0), r = cross(e1, sv), W = P0 - cs.o;
            // det = -(y - o).N, Nu = (y - o).r, Nv = (y - o).q, each A + B x1 + G x2.
            const float dd = -dot(W, N), dX = -dot(P1, N), dL = -dot(P2, N);
            const float u0 = dot(W, r), uX = dot(P1, r), uL = dot(P2, r);
            const float v0 = dot(W, q), vX = dot(P1, q), vL = dot(P2, q);
            vec3 C[5];
            C[0] = sg * vec3(u0, uX, uL);                                                  // u >= 0
            C[1] = sg * vec3(v0, vX, vL);                                                  // v >= 0
            C[2] = sg * vec3(dd - u0 - v0, dX - uX - vX, dL - uL - vL);                    // u + v <= 1
            C[3] = sg * vec3(nt - 1e-6 * dd, -1e-6 * dX, -1e-6 * dL);                      // t > eps
            C[4] = sg * vec3((1.0 - 1e-6) * dd - nt, (1.0 - 1e-6) * dX, (1.0 - 1e-6) * dL); // t < 1 - eps
            vec2 PQ[5];
            bool lower[5];
            for (int j = 0; j < 5; ++j) {
                const float g = abs(C[j].z) < 1e-30 ? 1e-30 : C[j].z; // no x2 term: all of the line or none
                PQ[j] = -C[j].xy / g;
                lower[j] = g > 0.0;
            }
            for (int k = 0; k < KIN_AREA_LINES; ++k) {
                float a = 0.0, bb = 1.0;
                for (int j = 0; j < 5; ++j) {
                    const float bound = (PQ[j].x + PQ[j].y * x1[k]) * inv_scale[k];
                    a = lower[j] ? max(a, bound) : a;
                    bb = lower[j] ? bb : min(bb, bound);
                }
                if (a < bb) {
                    kin_area_insert(slots[k], filled[k], vec2(a, bb));
                }
            }
        }
    }
    // The lines' light with its shadows and without (the same quadrature: its error cancels in their ratio), and the
    // weighted share of the lines' length not blocked (the visibility where the lines see none of the light).
    vec3 light = vec3(0.0), open = vec3(0.0);
    float clear = 0.0, weights = 0.0;
    for (int k = 0; k < KIN_AREA_LINES; ++k) {
        const float w = m.triangle ? kin_area_jacobi_w[k] : kin_area_legendre_w[k];
        const vec3 A = m.P0 + x1[k] * m.P1, L = (m.triangle ? x1[k] : 1.0) * m.P2;
        const vec3 power = m.radiance * (m.jacobian * w / 3.14159265);
        const vec3 whole = kin_area_stretch(0.0, 1.0, point, n, A, L, m.ne, m.two_sided, power);
        open += whole;
        weights += w;
        if (filled[k] == 0) {
            light += whole; // unshadowed: one stretch
            clear += w;
            continue;
        }
        kin_area_sort(slots[k]);
        float cursor = 0.0;
        for (int i = 0; i <= KIN_AREA_SLOTS; ++i) {
            const float stop = i < KIN_AREA_SLOTS ? min(slots[k][i].x, 1.0) : 1.0;
            if (stop > cursor) {
                light += kin_area_stretch(cursor, stop, point, n, A, L, m.ne, m.two_sided, power);
                clear += w * (stop - cursor);
            }
            if (i < KIN_AREA_SLOTS) {
                cursor = max(cursor, slots[k][i].y);
            }
        }
    }
    // The emitter's exact light unshadowed, times the share the lines find unshadowed: a large emitter close by (whose
    // light 4 lines of 2 points each sample poorly) is right where nothing blocks it, its shadows are the lines'.
#ifdef KIN_AREA_EXACT_FORM
    // (The voxels' 2 lines take their own light: against Cycles the bounces came out closer so.)
    const float lines = dot(open, vec3(1.0));
    const float seen = lines > 1e-20 ? clamp(dot(light, vec3(1.0)) / lines, 0.0, 1.0) : clear / max(weights, 1e-20);
    return m.radiance * (kin_area_form_factor(m, point, n) * seen);
#else
    return light;
#endif
}

// Every emitter's.
vec3 kin_area_light(uint emitters, uvec4 scene, vec3 point, vec3 n, vec3 o) {
    vec3 light = vec3(0.0);
    for (uint e = 0u; e < emitters; ++e) {
        light += kin_area_emitter(e, scene, point, n, o);
    }
    return light;
}
