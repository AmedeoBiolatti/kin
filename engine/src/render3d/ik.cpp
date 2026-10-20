#include <algorithm>
#include <array>
#include <cmath>
#include <kin/render3d/ik3d.hpp>
#include <span>
#include <vector>

namespace kin {

namespace {

constexpr f32 pi = 3.14159265358979323846f;
constexpr f32 to_radians = pi / 180.0f;
constexpr f32 to_degrees = 180.0f / pi;

Vec3f translation_of(const Mat4f& m) {
    return {m.m[12], m.m[13], m.m[14]};
}

// A unit vector perpendicular to `v`, the same one for the same input (the axis it is least along is crossed out).
Vec3f perpendicular(Vec3f v) {
    const f32 ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    const Vec3f axis = ax <= ay && ax <= az ? Vec3f{1, 0, 0} : (ay <= az ? Vec3f{0, 1, 0} : Vec3f{0, 0, 1});
    return normalize(cross(v, axis));
}

Quatf axis_angle_radians(Vec3f axis, f32 radians) {
    const f32 half = radians * 0.5f;
    const f32 s = std::sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

f32 angle_between(Vec3f a, Vec3f b) {
    return std::atan2(length(cross(a, b)), dot(a, b));
}

// `a` turned toward `b` (both unit) by `angle` radians along the short way; `b` itself when the angle covers it.
Vec3f rotate_toward(Vec3f a, Vec3f b, f32 angle) {
    if (angle <= 0.0f) {
        return a;
    }
    const f32 total = angle_between(a, b);
    if (angle >= total) {
        return b;
    }
    Vec3f axis = cross(a, b);
    const f32 n = length(axis);
    axis = n > 1e-6f ? axis / n : perpendicular(a);
    return axis_angle_radians(axis, angle).rotate(a);
}

// The short way, normalised: `a` at 0, `b` at 1.
Quatf blend_rotation(Quatf a, Quatf b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    return Quatf{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}
        .normalized();
}

bool is_below(const Skeleton3D& sk, i32 joint, i32 ancestor) {
    const i32 n = static_cast<i32>(sk.joint_count());
    for (i32 j = joint >= 0 && joint < n ? sk.parents[static_cast<std::size_t>(joint)] : -1; j >= 0;
         j = sk.parents[static_cast<std::size_t>(j)]) {
        if (j == ancestor) {
            return true;
        }
    }
    return false;
}

const Mat4f& parent_world(const Skeleton3D& sk, std::span<const Mat4f> world, i32 joint) {
    const i32 p = sk.parents[static_cast<std::size_t>(joint)];
    return p < 0 ? sk.root_transform : world[static_cast<std::size_t>(p)];
}

// Whether the arrays are the size the skeleton says and `joint` is one of its joints.
bool usable(const Skeleton3D& sk, const Pose3D& pose, std::span<const Mat4f> world) {
    const std::size_t n = sk.joint_count();
    return n > 0 && pose.joints.size() >= n && world.size() >= n;
}
bool is_joint(const Skeleton3D& sk, i32 j) {
    return j >= 0 && j < static_cast<i32>(sk.joint_count());
}

// Rotates `joint` by `r`, a rotation of the model's space, about the joint's own origin; in the joint's local rotation
// that is  Rp^-1 r Rp q  with Rp its parent's rotation. `world` follows.
void rotate_joint(const Skeleton3D& sk, Pose3D& pose, std::span<Mat4f> world, i32 joint, Quatf r) {
    const Quatf rp = rotation_of(parent_world(sk, world, joint));
    JointPose3D& jp = pose.joints[static_cast<std::size_t>(joint)];
    jp.rotation = (rp.conjugate() * r * rp * jp.rotation).normalized();
    update_world_below(sk, pose, world, joint);
}

f32 smoothstep01(f32 x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// The side a bend goes to, across the unit axis `t`: `want` (any length up to 1) taken across the axis, with a small
// pull toward `alt` that only matters when `want` is nearly along the axis (or nearly nothing), so that the side stays
// continuous as `want` goes through the axis instead of being undefined there. `zone` is how short a `want` across the
// axis must be for the pull to act, `strength` its size.
Vec3f side_across(Vec3f want, Vec3f alt, Vec3f t, f32 zone, f32 strength) {
    const Vec3f across = want - t * dot(want, t);
    const Vec3f alt_across = alt - t * dot(alt, t);
    const f32 pull = strength * std::pow(std::max(0.0f, 1.0f - length(across) / zone), 2.0f);
    const Vec3f v = across + alt_across * pull;
    if (length(v) > 1e-9f) {
        return normalize(v);
    }
    return length(alt_across) > 1e-9f ? normalize(alt_across) : perpendicular(t);
}

} // namespace

// --- matrices
// ------------------------------------------------------------------------------------------------------------

void update_world_below(const Skeleton3D& skeleton, const Pose3D& pose, std::span<Mat4f> world, i32 joint) {
    const std::size_t n = std::min<std::size_t>(skeleton.joint_count(), world.size());
    if (joint < 0 || static_cast<std::size_t>(joint) >= n) {
        return;
    }
    // Which joints are below `joint` (a parent comes first, so one pass finds them). On the stack for a skeleton a
    // skin can have; a bigger one (programmatic, not for skinning) takes the heap, so the array is never overrun.
    std::array<u8, max_skin_joints> small{};
    std::vector<u8> big;
    std::span<u8> dirty = small;
    if (n > small.size()) {
        big.assign(n, 0);
        dirty = big;
    }
    const auto recompute = [&](std::size_t i) {
        const Mat4f local = i < pose.joints.size() ? pose.joints[i].matrix() : skeleton.rest[i].matrix();
        const i32 p = skeleton.parents[i];
        world[i] = (p < 0 ? skeleton.root_transform : world[static_cast<std::size_t>(p)]) * local;
    };
    const std::size_t first = static_cast<std::size_t>(joint);
    dirty[first] = 1;
    recompute(first);
    for (std::size_t i = first + 1; i < n; ++i) {
        const i32 p = skeleton.parents[i];
        if (p >= joint && dirty[static_cast<std::size_t>(p)]) {
            dirty[i] = 1;
            recompute(i);
        }
    }
}

void world_to_joint_matrices(const Skeleton3D& skeleton, std::span<const Mat4f> world, std::span<Mat4f> out) {
    const std::size_t n = std::min({static_cast<std::size_t>(skeleton.joint_count()), world.size(), out.size()});
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = world[i] * skeleton.inverse_bind[i];
    }
}

Quatf rotation_of(const Mat4f& m) {
    Vec3f x{m.m[0], m.m[1], m.m[2]};
    Vec3f y{m.m[4], m.m[5], m.m[6]};
    x = normalize(x);
    if (length_squared(x) == 0.0f) {
        return {};
    }
    y = y - x * dot(x, y);
    y = normalize(y);
    if (length_squared(y) == 0.0f) {
        y = perpendicular(x);
    }
    const Vec3f z = cross(x, y);
    const f32 m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    const f32 trace = m00 + m11 + m22;
    Quatf q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return q.normalized();
}

Quatf rotation_between(Vec3f from, Vec3f to) {
    const Vec3f a = normalize(from), b = normalize(to);
    if (length_squared(a) == 0.0f || length_squared(b) == 0.0f) {
        return {};
    }
    const f32 d = dot(a, b);
    if (d < -0.999999f) {
        const Vec3f axis = perpendicular(a);
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3f c = cross(a, b);
    return Quatf{c.x, c.y, c.z, 1.0f + d}.normalized();
}

// --- two bones
// -----------------------------------------------------------------------------------------------------------

namespace {

// Whether the linear part of `m` is a rotation times one scale (to rounding): then a turn in the space of the frame `m`
// leads to is a turn of the joint's local rotation, exactly. A non-uniform scale (or shear) makes it not so.
bool is_similarity(const Mat4f& m) {
    const Vec3f c0{m.m[0], m.m[1], m.m[2]}, c1{m.m[4], m.m[5], m.m[6]}, c2{m.m[8], m.m[9], m.m[10]};
    const f32 l0 = length(c0), l1 = length(c1), l2 = length(c2);
    const f32 tol = 1e-4f * std::max(l0, 1e-9f);
    return std::fabs(l0 - l1) <= tol && std::fabs(l0 - l2) <= tol && std::fabs(dot(c0, c1)) <= tol * l0 &&
           std::fabs(dot(c0, c2)) <= tol * l0 && std::fabs(dot(c1, c2)) <= tol * l0;
}

f32 signed_angle(Vec3f from, Vec3f to, Vec3f axis) {
    return std::atan2(dot(cross(from, to), axis), dot(from, to));
}

// Where `joint` (below `root`) is at rest, in the space of the root joint's parent: the rest matrices composed down the
// chain.
Vec3f rest_point(const Skeleton3D& sk, i32 root, i32 joint) {
    std::array<i32, max_skin_joints> path{};
    std::size_t n = 0;
    for (i32 j = joint; j >= 0 && n < path.size(); j = sk.parents[static_cast<std::size_t>(j)]) {
        path[n++] = j;
        if (j == root) {
            break;
        }
    }
    Mat4f m;
    for (std::size_t i = n; i-- > 0;) {
        m = m * sk.rest[static_cast<std::size_t>(path[i])].matrix();
    }
    return translation_of(m);
}

// What one pass of the solver leaves for the passes after it (a scaled chain is solved by repeating, see
// solve_two_bone).
struct TwoBonePass {
    Vec3f goal{};     // where the end joint is meant to be, in the space of the root's parent
    Vec3f bend{};     // the side the elbow was put on
    Quatf end_goal{}; // the orientation the end joint is meant to have, in the same space
    bool has_end_goal = false;
    bool rigid = true; // the joints' parents' matrices are rotation times a uniform scale: one pass is exact
};

// One solve at weight `w` of the pose given. `forced_bend` and `forced_end` (later passes of a scaled chain) take the
// side of the elbow and the end orientation of an earlier pass instead of working them out again.
TwoBoneResult3D two_bone_pass(const Skeleton3D& sk, Pose3D& pose, std::span<Mat4f> world, const TwoBoneChain3D& chain,
                              Vec3f target, const TwoBoneOptions3D& o, f32 w, TwoBoneState3D* state, f32 dt,
                              const Vec3f* forced_bend, const Quatf* forced_end, TwoBonePass& pass) {
    TwoBoneResult3D res;
    // Everything in the space of the root joint's parent (F): there a rotation of the root is a rotation, whatever
    // scale or turn the model and its ancestors carry, and the bones are the lengths they are drawn at.
    const Mat4f F = parent_world(sk, world, chain.root);
    if (std::fabs(F.determinant()) < 1e-20f) {
        return res;
    }
    const Mat4f Fi = F.inverse();
    const auto root = static_cast<std::size_t>(chain.root), mid = static_cast<std::size_t>(chain.mid),
               end = static_cast<std::size_t>(chain.end);
    const Vec3f A = Fi.apply(translation_of(world[root]));
    const Vec3f B = Fi.apply(translation_of(world[mid]));
    const Vec3f C = Fi.apply(translation_of(world[end]));
    const Vec3f T = Fi.apply(target);
    const f32 l1 = length(B - A), l2 = length(C - B);
    if (l1 < 1e-7f || l2 < 1e-7f) {
        return res;
    }
    const f32 L = l1 + l2;

    // A soft stretch, measured against the skeleton's rest bone lengths (so that a pose that is stretched already, by a
    // clip or by this solver on a pass before, is not stretched again: solving twice is solving once). It tracks the
    // distance as the target passes the reach and tends to max_stretch, never beyond it.
    const Vec3f dvec = T - A;
    const f32 d = length(dvec);
    f32 held = 0.0f, rest_len = 0.0f;
    for (i32 j = chain.mid; j != chain.root; j = sk.parents[static_cast<std::size_t>(j)]) {
        held += length(pose.joints[static_cast<std::size_t>(j)].translation);
        rest_len += length(sk.rest[static_cast<std::size_t>(j)].translation);
    }
    for (i32 j = chain.end; j != chain.mid; j = sk.parents[static_cast<std::size_t>(j)]) {
        held += length(pose.joints[static_cast<std::size_t>(j)].translation);
        rest_len += length(sk.rest[static_cast<std::size_t>(j)].translation);
    }
    const f32 stretched_already = rest_len > 1e-9f ? std::max(held / rest_len, 1.0f) : 1.0f;
    f32 s = 1.0f;                    // the factor applied to the pose's bones now
    f32 s_total = stretched_already; // and relative to the rest lengths
    if (o.max_stretch > 1.0f && d > L) {
        const f32 span = o.max_stretch - 1.0f;
        const f32 target = 1.0f + span * (1.0f - std::exp(-(d / (L / stretched_already) - 1.0f) / span));
        if (target > stretched_already) {
            s = target / stretched_already;
            s_total = target;
        }
    }
    const f32 l1s = l1 * s, l2s = l2 * s;
    const f32 bend_floor = std::clamp(o.min_bend_degrees, 0.0f, 90.0f) * to_radians;
    const f32 dmax = bend_floor <= 0.0f
                         ? l1s + l2s
                         : std::sqrt(std::max(l1s * l1s + l2s * l2s + 2.0f * l1s * l2s * std::cos(bend_floor), 0.0f));
    f32 dmin = std::max(std::fabs(l1s - l2s), 1e-6f * L);
    if (o.max_bend_degrees < 180.0f) {
        // The chord at the most the mid joint may bend: nearer than that the chain stands as folded as it may.
        const f32 bend_ceiling = std::max(o.max_bend_degrees, 0.0f) * to_radians;
        dmin = std::max(dmin,
                        std::sqrt(std::max(l1s * l1s + l2s * l2s + 2.0f * l1s * l2s * std::cos(bend_ceiling), 0.0f)));
        dmin = std::min(dmin, dmax);
    }
    const f32 deff = std::clamp(d, std::min(dmin, dmax), dmax);

    // The direction to the target. A target nearer the root than the folded chain can come (unequal bones leave a hole
    // round the root) is aimed at through the point the chain's own axis adds to it, so that a target going through the
    // root's place turns the chain smoothly instead of flipping it end to end.
    const Vec3f axis_now = normalize(C - A);
    const Vec3f toward = length_squared(axis_now) > 0.0f ? axis_now : normalize(B - A);
    Vec3f aim = dvec;
    if (d < dmin) {
        aim = dvec + toward * (dmin - d);
    }
    const f32 aim_len = length(aim);
    const Vec3f t = aim_len > 1e-6f * L ? aim / aim_len : toward;

    const Vec3f u0 = normalize(B - A);
    const f32 a = (deff * deff + l1s * l1s - l2s * l2s) / (2.0f * deff);
    // The height of the triangle by Heron's factors: near full reach (l1 + l2 - d tends to 0) this keeps the elbow on
    // the line where l1^2 - a^2 would lose it to rounding.
    const f32 h =
        std::sqrt(std::max((l1s + l2s + deff) * (l1s + l2s - deff) * (deff + l1s - l2s) * (deff - l1s + l2s), 0.0f)) /
        (2.0f * deff);

    // The plane the pose bends in, to carry onto the new one: the hinge's, as the mid joint is built, or the pose's
    // own.
    Vec3f n0{};
    f32 carry = 0.0f;
    bool hinged = false;
    const Vec3f bend_now = cross(B - A, C - B);
    if (o.hinge) {
        const Vec3f axis = normalize(o.hinge_axis);
        if (length_squared(axis) > 0.0f) {
            const Vec3f hw = rotation_of(Fi * world[mid]).rotate(axis);
            const Vec3f across = hw - u0 * dot(hw, u0);
            if (length(across) > 1e-4f) {
                n0 = normalize(across);
                carry = 1.0f;
                hinged = true;
            }
        }
    }
    if (!hinged && length(bend_now) > 1e-9f) {
        const f32 angle_now = std::atan2(length(bend_now), dot(B - A, C - B));
        n0 = normalize(bend_now);
        carry = smoothstep01(angle_now / (5.0f * to_radians));
    }

    // Which way the elbow points. A pole gives it; with none (or on the target's axis, where a pole says nothing) it is
    // the side the pose is bent to (the hinge's, when the mid joint is one), and a chain with no bend to speak of goes
    // the way the rig was built to bend (the rest pose's bend, turned as the root is), else toward -Y of the root's
    // parent (an elbow down, a knee forward by a pole of their own). Each falls back on the next by a pull that only
    // acts near the degenerate case, never by a switch.
    Vec3f want_default{0.0f, -1.0f, 0.0f}, alt_default{0.0f, 0.0f, 1.0f};
    {
        const Vec3f Ar = translation_of(sk.rest[root].matrix());
        const Vec3f Br = rest_point(sk, chain.root, chain.mid), Cr = rest_point(sk, chain.root, chain.end);
        const Vec3f rest_axis = normalize(Cr - Ar);
        const Vec3f rest_arm = Br - Ar;
        const Vec3f rest_bend = rest_arm - rest_axis * dot(rest_arm, rest_axis);
        const f32 rest_l1 = length(rest_arm);
        if (length_squared(rest_axis) > 0.0f && rest_l1 > 1e-7f && length(rest_bend) > 0.02f * rest_l1) {
            // The rest bend turned as the root's own turn has changed since rest: the limb's own side, wherever it
            // points.
            const Quatf turned = (pose.joints[root].rotation * sk.rest[root].rotation.conjugate()).normalized();
            want_default = turned.rotate(normalize(rest_bend));
            alt_default = std::fabs(want_default.z) > 0.9f ? Vec3f{0.0f, -1.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f};
        }
    }
    const Vec3f default_side = side_across(want_default, alt_default, t, 0.3f, 0.1f);
    const Vec3f pose_side = hinged ? cross(t, n0) : (((B - A) - axis_now * dot(B - A, axis_now)) / l1);
    const Vec3f fallback = side_across(pose_side, default_side, t, 0.02f, 0.02f);
    Vec3f bend = fallback;
    if (o.pole_mode != PoleMode3D::None) {
        const Vec3f pole = o.pole_mode == PoleMode3D::Position ? Fi.apply(o.pole) - A : Fi.apply_vector(o.pole);
        const Vec3f pn = normalize(pole);
        if (length_squared(pn) > 0.0f) {
            bend = side_across(pn, fallback, t, 0.3f, 0.1f);
        }
    }
    if (forced_bend) {
        bend = side_across(*forced_bend, bend, t, 1e-3f, 0.0f);
    } else {
        // The side swings toward the one wanted at a bounded speed: a pole going through the target's axis (where the
        // side must go over to the other one) is then a turn over time, not over a few degrees of the target's travel.
        if (state && o.bend_swing_degrees_per_second > 0.0f && state->valid) {
            const Vec3f held_side = state->bend - t * dot(state->bend, t);
            if (length(held_side) > 1e-6f) {
                const Vec3f from = normalize(held_side);
                const f32 gap = angle_between(from, bend);
                const f32 step = std::max(o.bend_swing_degrees_per_second, 0.0f) * to_radians * std::max(dt, 0.0f);
                if (gap > step) {
                    // Exactly opposite sides have no shorter way round: the way the last swing went.
                    const f32 side = dot(cross(from, bend), t);
                    const f32 sense = gap > pi - 0.05f ? state->sense : (side >= 0.0f ? 1.0f : -1.0f);
                    state->sense = sense;
                    bend = normalize(axis_angle_radians(t, sense * step).rotate(from));
                }
            }
        }
        if (state) {
            state->bend = bend;
            state->valid = true;
        }
        // The root's twist about its bone, from what the clips did, held to a limit: the elbow's side is turned about
        // the target's axis (the end stays on the target) until the twist is the limit's; past it, as the pole goes
        // round behind, the limit itself eases to nothing at a half turn, so that the side never flips.
        const f32 limit = std::clamp(o.max_twist_degrees, 0.0f, 180.0f) * to_radians;
        if (limit < pi - 1e-4f && carry > 0.0f) {
            const auto twist_with = [&](Vec3f side) {
                const Vec3f u1 = normalize(t * a + side * h);
                const Vec3f n1 = normalize(cross(side, t));
                const Vec3f carried = rotation_between(u0, u1).rotate(n0);
                return signed_angle(carried, n1, u1);
            };
            const f32 phi = twist_with(bend);
            if (std::fabs(phi) > limit) {
                const f32 sigma = phi < 0.0f ? -1.0f : 1.0f;
                const f32 goal = sigma * limit * (1.0f - smoothstep01((std::fabs(phi) - limit) / (pi - limit)));
                const auto gap_at = [&](f32 psi) {
                    f32 g = twist_with(axis_angle_radians(t, psi).rotate(bend)) - goal;
                    g = std::fmod(g + pi, 2.0f * pi);
                    return (g < 0.0f ? g + 2.0f * pi : g) - pi;
                };
                const f32 g0 = gap_at(0.0f);
                f32 found = 0.0f;
                bool ok = false;
                constexpr f32 step = 5.0f * to_radians;
                for (int k = 1; k <= 38 && !ok; ++k) { // a little past a half turn: the root may be at it
                    for (const f32 sign : {1.0f, -1.0f}) {
                        const f32 lo = sign * step * static_cast<f32>(k - 1), hi = sign * step * static_cast<f32>(k);
                        const f32 glo = k == 1 ? g0 : gap_at(lo), ghi = gap_at(hi);
                        if (glo * ghi <= 0.0f && std::fabs(glo) < pi * 0.5f && std::fabs(ghi) < pi * 0.5f) {
                            f32 x = lo, y = hi, gx = glo;
                            for (int it = 0; it < 24; ++it) {
                                const f32 m = 0.5f * (x + y);
                                const f32 gm = gap_at(m);
                                if (gx * gm <= 0.0f) {
                                    y = m;
                                } else {
                                    x = m;
                                    gx = gm;
                                }
                            }
                            found = 0.5f * (x + y);
                            ok = true;
                            break;
                        }
                    }
                }
                if (ok) {
                    // How bent the pose is (carry) says how much of a plane there is to hold the twist against.
                    bend = normalize(axis_angle_radians(t, found * carry).rotate(bend));
                }
            }
        }
    }
    pass.bend = bend;
    const Vec3f M = A + t * a + bend * h;
    const Vec3f E = A + t * deff;
    pass.goal = E;

    // The turn of the upper bone. The shortest turn of the bone leaves the plane of the bend wherever it lands; the
    // arm's bend plane is information worth keeping (an elbow hinges one way), so as far as the pose is bent the turn
    // carries the whole triangle shoulder-elbow-hand with it, plane included, onto the new triangle: then the forearm
    // lies in the new plane and the elbow only hinges, with the twist taken up at the shoulder, continuously with the
    // pole. A pose that is straight has no plane to carry (the turn is the shortest one), and in between the two turns,
    // which differ by a twist about the bone, are mixed by how bent the pose is. The elbow's side is the pole's: the
    // normal is not chosen by which turn is shorter, so an elbow never bends backwards.
    const Vec3f Cs = A + (C - A) * s;
    const Vec3f u1 = normalize(M - A);
    Quatf R1 = rotation_between(u0, u1);
    const Vec3f n1 = normalize(cross(bend, t));
    if (!(length_squared(n1) > 0.0f)) {
        carry = 0.0f;
    }
    if (carry > 0.0f) {
        const auto frame = [](Vec3f x, Vec3f y) {
            Mat4f m;
            const Vec3f z = cross(x, y);
            m.m[0] = x.x, m.m[1] = x.y, m.m[2] = x.z;
            m.m[4] = y.x, m.m[5] = y.y, m.m[6] = y.z;
            m.m[8] = z.x, m.m[9] = z.y, m.m[10] = z.z;
            return rotation_of(m);
        };
        const Quatf carried = (frame(u1, n1) * frame(u0, n0).conjugate()).normalized();
        R1 = carry >= 1.0f ? carried : blend_rotation(R1, carried, carry);
    }
    const Vec3f C1 = A + R1.rotate(Cs - A);
    // The forearm's shortest turn onto its new direction; in the plane that is the hinge about the plane's normal (its
    // half turn, folded flat, about that normal too).
    Quatf R2 = rotation_between(C1 - M, E - M);
    if (carry > 0.0f && dot(normalize(C1 - M), normalize(E - M)) < -0.999999f) {
        R2 = axis_angle_radians(n1, pi);
    }

    // Back to local rotations: each joint's turn is in F's frame, so its parent's frame conjugates it.
    JointPose3D& jr = pose.joints[root];
    JointPose3D& jm = pose.joints[mid];
    JointPose3D& je = pose.joints[end];
    const Quatf q0 = jr.rotation, q1 = jm.rotation, q2 = je.rotation;
    const Quatf q0n = (R1 * q0).normalized();
    const Mat4f mid_parent = Fi * parent_world(sk, world, chain.mid);
    const Quatf rpm = R1 * rotation_of(mid_parent);
    const Quatf q1n = (rpm.conjugate() * R2 * rpm * q1).normalized();
    Quatf q2n = q2;
    pass.rigid = is_similarity(mid_parent);
    if (o.match_rotation) {
        const Mat4f end_parent = Fi * parent_world(sk, world, chain.end);
        pass.rigid = pass.rigid && is_similarity(end_parent);
        const Quatf pe = R2 * R1 * rotation_of(end_parent);
        if (forced_end) {
            // A later pass of a scaled chain: the end's orientation as it is now (from the matrices, which the shear of
            // a non-uniform scale makes not the product of its parts) is corrected toward the goal of the first pass.
            const Quatf actual = (R2 * R1 * rotation_of(Fi * world[end])).normalized();
            const Quatf err = (*forced_end * actual.conjugate()).normalized();
            q2n = (pe.conjugate() * err * pe * q2).normalized();
            pass.end_goal = *forced_end;
        } else {
            const Quatf want = rotation_of(F).conjugate() * o.end_rotation;
            const Quatf now = pe * q2;
            const Quatf goal = blend_rotation(now, want, std::clamp(o.rotation_weight, 0.0f, 1.0f));
            q2n = (pe.conjugate() * goal).normalized();
            pass.end_goal = goal;
        }
        pass.has_end_goal = true;
    }
    // Weight: the pose blended with the solution, bone by bone.
    const bool full = w >= 1.0f;
    jr.rotation = full ? q0n : blend_rotation(q0, q0n, w);
    jm.rotation = full ? q1n : blend_rotation(q1, q1n, w);
    if (o.match_rotation) {
        je.rotation = full ? q2n : blend_rotation(q2, q2n, w);
    }
    const f32 scale = 1.0f + (s - 1.0f) * w;
    if (s != 1.0f) {
        for (i32 j = chain.mid; j != chain.root; j = sk.parents[static_cast<std::size_t>(j)]) {
            pose.joints[static_cast<std::size_t>(j)].translation *= scale;
        }
        for (i32 j = chain.end; j != chain.mid; j = sk.parents[static_cast<std::size_t>(j)]) {
            pose.joints[static_cast<std::size_t>(j)].translation *= scale;
        }
    }
    update_world_below(sk, pose, world, chain.root);

    res.solved = true;
    res.stretch = s_total;
    res.reached = std::fabs(d - deff) <= 1e-5f * L; // not so in the hole round the root, nor past the reach
    res.bend_degrees = (std::atan2(h, a) + std::atan2(h, deff - a)) * to_degrees;
    return res;
}

} // namespace

TwoBoneResult3D solve_two_bone(const Skeleton3D& sk, Pose3D& pose, std::span<Mat4f> world, const TwoBoneChain3D& chain,
                               Vec3f target, const TwoBoneOptions3D& o, TwoBoneState3D* state, f32 dt) {
    if (!(o.weight > 0.0f) || !usable(sk, pose, world) || !is_joint(sk, chain.root) || !is_joint(sk, chain.mid) ||
        !is_joint(sk, chain.end) || !is_below(sk, chain.mid, chain.root) || !is_below(sk, chain.end, chain.mid)) {
        return {};
    }
    const f32 w = std::min(o.weight, 1.0f);
    TwoBonePass pass;
    const Mat4f F = parent_world(sk, world, chain.root);
    const Mat4f mid_parent = F.inverse() * parent_world(sk, world, chain.mid);
    const bool nonrigid = std::fabs(F.determinant()) >= 1e-20f &&
                          (!is_similarity(mid_parent) ||
                           (o.match_rotation && !is_similarity(F.inverse() * parent_world(sk, world, chain.end))));
    if (!nonrigid) {
        // The common case: one pass at the weight given.
        return two_bone_pass(sk, pose, world, chain, target, o, w, state, dt, nullptr, nullptr, pass);
    }

    // A scaled joint between the root and the mid joint (or the root itself): a turn of the mid joint changes the drawn
    // length of the forearm (the parent's matrix is a rotation times a non-uniform scale, which moves the forearm's end
    // along its own length as the joint turns), so the rigid triangle's lengths are not the chain's. Solved in three
    // steps, all at weight 1 from the pose as it was, the result blended with that pose at the end by the weight:
    //   1. turn the mid joint about its hinge (the plane the pose bends in, or the hinge's axis) until the end is at
    //   the distance
    //      from the root the target asks for, which is a scalar equation in the turn, solved by bracketing and
    //      bisection;
    //   2. the rigid solve then finds the lengths it sees are the chain's, and the triangle it makes is the chain's
    //   own;
    //   3. a few passes of the same from its own result, to the end orientation and to rounding.
    const std::size_t root = static_cast<std::size_t>(chain.root), mid = static_cast<std::size_t>(chain.mid),
                      end = static_cast<std::size_t>(chain.end);
    const JointPose3D before_root = pose.joints[root], before_mid = pose.joints[mid], before_end = pose.joints[end];
    // Translations along the chain (a stretch changes them): kept to blend with a weight below 1.
    std::array<std::pair<i32, Vec3f>, 16> kept{};
    std::size_t kept_count = 0;
    for (i32 j = chain.mid; j != chain.root && kept_count < kept.size(); j = sk.parents[static_cast<std::size_t>(j)]) {
        kept[kept_count++] = {j, pose.joints[static_cast<std::size_t>(j)].translation};
    }
    for (i32 j = chain.end; j != chain.mid && kept_count < kept.size(); j = sk.parents[static_cast<std::size_t>(j)]) {
        kept[kept_count++] = {j, pose.joints[static_cast<std::size_t>(j)].translation};
    }
    const Mat4f Fi = F.inverse();
    if (!is_similarity(mid_parent)) {
        const Vec3f A = Fi.apply(translation_of(world[root]));
        const Vec3f B = Fi.apply(translation_of(world[mid]));
        const Vec3f C = Fi.apply(translation_of(world[end]));
        const Vec3f T = Fi.apply(target);
        const Mat4f inverse_parent = mid_parent.inverse();
        const Vec3f u = B - A;
        const Vec3f y_forearm = inverse_parent.apply_vector(C - B); // the forearm in the mid joint's parent's own axes
        const Vec3f y_upper = inverse_parent.apply_vector(u);
        Vec3f axis{};
        if (o.hinge && length_squared(o.hinge_axis) > 0.0f) {
            axis = pose.joints[mid].rotation.rotate(normalize(o.hinge_axis));
        } else {
            axis = normalize(cross(y_upper, y_forearm));
            if (length(cross(y_upper, y_forearm)) < 1e-6f * length(y_upper) * length(y_forearm)) {
                axis = perpendicular(y_forearm);
            }
        }
        const auto chord = [&](f32 phi) {
            return length(u + mid_parent.apply_vector(axis_angle_radians(axis, phi).rotate(y_forearm)));
        };
        const f32 want = length(T - A);
        constexpr int samples = 72;
        f32 values[samples + 1];
        f32 lowest = 1e30f, highest = -1e30f, at_lowest = 0.0f, at_highest = 0.0f;
        for (int k = 0; k <= samples; ++k) {
            const f32 phi = 2.0f * pi * static_cast<f32>(k) / static_cast<f32>(samples);
            values[k] = chord(phi);
            if (values[k] < lowest) {
                lowest = values[k];
                at_lowest = phi;
            }
            if (values[k] > highest) {
                highest = values[k];
                at_highest = phi;
            }
        }
        f32 phi = 0.0f;
        if (want <= lowest) {
            phi = at_lowest;
        } else if (want >= highest) {
            phi = at_highest;
        } else {
            // The crossing nearest the pose's own turn (phi 0, or a turn the other way round).
            f32 best = 1e30f;
            for (int k = 0; k < samples; ++k) {
                if ((values[k] - want) * (values[k + 1] - want) > 0.0f) {
                    continue;
                }
                f32 x = 2.0f * pi * static_cast<f32>(k) / static_cast<f32>(samples),
                    y = 2.0f * pi * static_cast<f32>(k + 1) / static_cast<f32>(samples);
                f32 gx = values[k] - want;
                for (int it = 0; it < 30; ++it) {
                    const f32 m = 0.5f * (x + y);
                    const f32 gm = chord(m) - want;
                    if (gx * gm <= 0.0f) {
                        y = m;
                    } else {
                        x = m;
                        gx = gm;
                    }
                }
                f32 found = 0.5f * (x + y);
                if (found > pi) {
                    found -= 2.0f * pi;
                }
                if (std::fabs(found) < best) {
                    best = std::fabs(found);
                    phi = found;
                }
            }
        }
        JointPose3D& jm = pose.joints[mid];
        jm.rotation = (axis_angle_radians(axis, phi) * jm.rotation).normalized();
        update_world_below(sk, pose, world, chain.mid);
    }
    TwoBoneResult3D res = two_bone_pass(sk, pose, world, chain, target, o, 1.0f, state, dt, nullptr, nullptr, pass);
    if (!res.solved) {
        pose.joints[root] = before_root;
        pose.joints[mid] = before_mid;
        pose.joints[end] = before_end;
        update_world_below(sk, pose, world, chain.root);
        return res;
    }
    const Vec3f forced_bend = pass.bend;
    const Vec3f goal = pass.goal;
    const Quatf end_goal = pass.end_goal;
    const bool has_end_goal = pass.has_end_goal;
    const f32 reach =
        std::max(length(Fi.apply(translation_of(world[end])) - Fi.apply(translation_of(world[root]))), 1e-6f);
    f32 error = 0.0f;
    for (int it = 0; it < 8; ++it) {
        error = length(Fi.apply(translation_of(world[end])) - goal);
        bool orientation_off = false;
        if (has_end_goal) {
            const Quatf now = rotation_of(Fi * world[end]);
            orientation_off = angle_between(now.rotate({1, 0, 0}), end_goal.rotate({1, 0, 0})) > 5e-5f ||
                              angle_between(now.rotate({0, 1, 0}), end_goal.rotate({0, 1, 0})) > 5e-5f;
        }
        if (error <= 2e-6f * reach && !orientation_off) {
            break;
        }
        TwoBonePass again;
        const TwoBoneResult3D r = two_bone_pass(sk, pose, world, chain, target, o, 1.0f, nullptr, 0.0f, &forced_bend,
                                                has_end_goal ? &end_goal : nullptr, again);
        if (!r.solved) {
            break;
        }
    }
    if (w < 1.0f) {
        // Weight: the solve blended with the pose it began from, bone by bone, the stretch by the same share.
        JointPose3D& jr = pose.joints[root];
        JointPose3D& jm = pose.joints[mid];
        JointPose3D& je = pose.joints[end];
        jr.rotation = blend_rotation(before_root.rotation, jr.rotation, w);
        jm.rotation = blend_rotation(before_mid.rotation, jm.rotation, w);
        je.rotation = o.match_rotation ? blend_rotation(before_end.rotation, je.rotation, w) : before_end.rotation;
        for (std::size_t i = 0; i < kept_count; ++i) {
            JointPose3D& jp = pose.joints[static_cast<std::size_t>(kept[i].first)];
            jp.translation = kept[i].second + (jp.translation - kept[i].second) * w;
        }
        update_world_below(sk, pose, world, chain.root);
    }
    res.reached = res.reached && error <= 1e-5f * reach;
    return res;
}

// --- look at
// -------------------------------------------------------------------------------------------------------------

LookAtResult3D solve_look_at(const Skeleton3D& sk, Pose3D& pose, std::span<Mat4f> world,
                             std::span<const LookAtJoint3D> chain, Vec3f target, const LookAtOptions3D& o,
                             LookAtState3D* state, f32 dt) {
    LookAtResult3D res;
    if (chain.empty() || !usable(sk, pose, world)) {
        return res;
    }
    f32 total_weight = 0.0f;
    for (const LookAtJoint3D& j : chain) {
        if (!is_joint(sk, j.joint)) {
            return res;
        }
        total_weight += std::max(j.weight, 0.0f);
    }
    const auto head = static_cast<std::size_t>(chain.back().joint);
    const Mat4f& hw = world[head];
    const Vec3f aim = normalize(hw.apply_vector(o.aim_axis));
    Vec3f up = normalize(hw.apply_vector(o.up_axis));
    const Vec3f eye = hw.apply(o.eye_offset);
    const Vec3f to_target = target - eye;
    if (length_squared(aim) == 0.0f || length(to_target) < 1e-9f) {
        return res;
    }
    const Vec3f goal = normalize(to_target);

    // The direction the head follows: the goal itself, or a smoothed approach to it that the caller's state remembers.
    Vec3f follow = goal;
    if (state) {
        if (!state->valid || length_squared(state->direction) == 0.0f) {
            // Nothing followed yet: start from where the head looks now (no pop), then approach as below.
            state->direction = aim;
            state->valid = true;
        }
        f32 k = 1.0f;
        if (o.smooth_seconds > 0.0f) {
            k = dt > 0.0f ? 1.0f - std::exp(-dt / o.smooth_seconds) : 0.0f;
        }
        f32 step = angle_between(state->direction, goal) * k;
        if (o.max_speed_degrees > 0.0f) {
            step = std::min(step, o.max_speed_degrees * to_radians * dt);
        }
        state->direction = normalize(rotate_toward(normalize(state->direction), goal, step));
        follow = state->direction;
    }
    const f32 weight = std::clamp(o.weight, 0.0f, 1.0f);
    if (weight <= 0.0f || total_weight <= 0.0f) {
        return res;
    }

    const f32 turn = angle_between(aim, follow);
    res.solved = true;
    res.goal_degrees = turn * to_degrees;
    Vec3f ax = cross(aim, follow);
    if (turn < 1e-6f) {
        return res;
    }
    ax = length(ax) > 1e-6f ? normalize(ax) : perpendicular(aim);

    // Azimuth of the deflection seen from the head: 0 to its right, a quarter turn up. The limits are an ellipse in it.
    up = normalize(up - aim * dot(up, aim));
    const Vec3f right = cross(aim, up);
    const Vec3f deflection = cross(ax, aim);
    const f32 du = dot(deflection, up), dr = dot(deflection, right);
    const f32 phi = std::atan2(du, dr);
    const f32 cp = std::cos(phi), sp = std::sin(phi);

    f32 applied = 0.0f;
    for (const LookAtJoint3D& j : chain) {
        const f32 share = std::max(j.weight, 0.0f) / total_weight;
        f32 angle = turn * share;
        const f32 yaw = std::max(j.max_yaw_degrees, 1e-3f) * to_radians;
        const f32 pitch = std::max(sp >= 0.0f ? j.max_pitch_up_degrees : j.max_pitch_down_degrees, 1e-3f) * to_radians;
        const f32 limit = 1.0f / std::sqrt((cp / yaw) * (cp / yaw) + (sp / pitch) * (sp / pitch));
        if (angle > limit) {
            // Held at the limit, then eased back to forward before the target is straight behind (see release_degrees).
            const f32 release = std::min(o.release_degrees * to_radians, pi - limit);
            angle = release > 1e-6f ? limit * (1.0f - smoothstep01((angle - limit) / release)) : limit;
        }
        angle *= weight;
        if (angle > 0.0f) {
            rotate_joint(sk, pose, world, j.joint, axis_angle_radians(ax, angle));
            applied += angle;
        }
    }
    res.applied_degrees = applied * to_degrees;
    return res;
}

// --- FABRIK
// --------------------------------------------------------------------------------------------------------------

FabrikResult3D solve_chain_fabrik(const Skeleton3D& sk, Pose3D& pose, std::span<Mat4f> world,
                                  std::span<const i32> chain, Vec3f target, const FabrikOptions3D& o) {
    FabrikResult3D res;
    const std::size_t n = chain.size();
    constexpr std::size_t max_chain = 64;
    if (n < 2 || n > max_chain || !(o.weight > 0.0f) || !usable(sk, pose, world)) {
        return res;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_joint(sk, chain[i]) || (i > 0 && !is_below(sk, chain[i], chain[i - 1]))) {
            return res;
        }
    }
    std::array<Vec3f, max_chain> p{};
    std::array<f32, max_chain> len{};
    f32 total = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = translation_of(world[static_cast<std::size_t>(chain[i])]);
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        len[i] = length(p[i + 1] - p[i]);
        total += len[i];
    }
    if (total < 1e-7f) {
        return res;
    }
    const Vec3f root = p[0];
    const auto toward = [](Vec3f from, Vec3f to, f32 distance) {
        const Vec3f v = to - from;
        const f32 l = length(v);
        return from + (l > 1e-9f ? v / l : Vec3f{0, 1, 0}) * distance;
    };
    const f32 reach_dist = length(target - root);
    u32 iterations = 0;
    if (o.fix_root && reach_dist >= total) {
        // Out of reach: the chain straight at the target.
        for (std::size_t i = 0; i + 1 < n; ++i) {
            p[i + 1] = toward(p[i], target, len[i]);
        }
    } else {
        while (iterations < o.max_iterations && length(p[n - 1] - target) > o.tolerance) {
            ++iterations;
            p[n - 1] = target;
            for (std::size_t i = n - 1; i-- > 0;) {
                p[i] = toward(p[i + 1], p[i], len[i]);
            }
            if (o.fix_root) {
                p[0] = root;
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    p[i + 1] = toward(p[i], p[i + 1], len[i]);
                }
            } else {
                break; // a free root: the backward pass lands the tip exactly
            }
        }
    }
    if (o.pole_mode != PoleMode3D::None && n >= 3) {
        // Turn the inner joints about the line from the first joint to the tip, so that the bulge of the chain faces
        // the pole. Both ends are on the line and every joint turns by the same angle about it: lengths and reach are
        // untouched.
        Vec3f axis = p[n - 1] - p[0];
        const f32 axis_len = length(axis);
        if (axis_len > 1e-6f * total) {
            axis = axis / axis_len;
            const Vec3f pole = o.pole_mode == PoleMode3D::Position ? o.pole - p[0] : o.pole;
            const Vec3f pole_across = pole - axis * dot(pole, axis);
            Vec3f bulge{};
            for (std::size_t i = 1; i + 1 < n; ++i) {
                const Vec3f v = p[i] - p[0];
                bulge += v - axis * dot(v, axis);
            }
            bulge = bulge / static_cast<f32>(n - 2);
            const f32 bulge_len = length(bulge), pole_len = length(pole), across_len = length(pole_across);
            if (bulge_len > 1e-9f && across_len > 1e-9f && pole_len > 0.0f) {
                // Both fade out toward their degenerate cases (a straight chain, a pole on the line) rather than
                // switch.
                const f32 fade =
                    smoothstep01(bulge_len / (0.05f * total)) * smoothstep01(across_len / (0.2f * pole_len));
                const f32 angle = signed_angle(bulge / bulge_len, pole_across / across_len, axis) * fade;
                const Quatf turn = axis_angle_radians(axis, angle);
                for (std::size_t i = 1; i + 1 < n; ++i) {
                    p[i] = p[0] + turn.rotate(p[i] - p[0]);
                }
            }
        }
    }
    const f32 w = std::min(o.weight, 1.0f);
    if (!o.fix_root) {
        // The start of the chain follows: its local translation moves by the same step (in its parent's frame).
        const Mat4f Fi = parent_world(sk, world, chain[0]).inverse();
        JointPose3D& jp = pose.joints[static_cast<std::size_t>(chain[0])];
        jp.translation += Fi.apply_vector(p[0] - root) * w;
        update_world_below(sk, pose, world, chain[0]);
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Vec3f now = translation_of(world[static_cast<std::size_t>(chain[i + 1])]) -
                          translation_of(world[static_cast<std::size_t>(chain[i])]);
        Quatf r = rotation_between(now, p[i + 1] - p[i]);
        if (w < 1.0f) {
            r = blend_rotation(Quatf{}, r, w);
        }
        rotate_joint(sk, pose, world, chain[i], r);
    }
    res.solved = true;
    res.iterations = iterations;
    res.error = length(translation_of(world[static_cast<std::size_t>(chain[n - 1])]) - target);
    res.reached = res.error <= std::max(o.tolerance * 4.0f, 1e-4f * total);
    return res;
}

// --- foot placement
// ------------------------------------------------------------------------------------------------------

FootPlan3D plan_foot_placement(const Skeleton3D& sk, const Pose3D& pose, std::span<const Mat4f> world,
                               std::span<const FootLeg3D> legs, const GroundQuery3D& ground,
                               const FootPlacementOptions3D& o, FootPlacementState3D* state, f32 dt,
                               const Mat4f* model_to_world) {
    FootPlan3D plan;
    plan.feet.resize(legs.size());
    if (legs.empty() || !ground || !usable(sk, pose, world)) {
        return plan;
    }
    const Vec3f up = normalize(o.up);
    const f32 k = o.smooth_seconds > 0.0f && state && state->valid
                      ? (dt > 0.0f ? 1.0f - std::exp(-dt / o.smooth_seconds) : 0.0f)
                      : 1.0f;
    if (state && state->foot_offset.size() != legs.size()) {
        state->foot_offset.assign(legs.size(), 0.0f);
        state->valid = false;
    }
    const bool locking = o.lock_feet && state;
    Mat4f to_world, to_model;
    if (locking) {
        if (state->locks.size() != legs.size() || !state->valid) {
            state->locks.assign(legs.size(), {});
        }
        to_world = model_to_world ? *model_to_world : Mat4f{};
        to_model = to_world.inverse();
    }
    const Quatf world_turn = locking ? rotation_of(to_world) : Quatf{};
    const f32 max_tilt = std::clamp(o.max_tilt_degrees, 0.0f, 89.0f) * to_radians;
    const auto clamp_normal = [&](Vec3f n) {
        n = normalize(n);
        if (length_squared(n) == 0.0f || dot(n, up) <= 0.0f) {
            n = up;
        }
        if (angle_between(up, n) > max_tilt) {
            const Vec3f across = normalize(n - up * dot(n, up));
            n = up * std::cos(max_tilt) + across * std::sin(max_tilt);
        }
        return n;
    };
    const auto tilt_amount = [&](f32 sole_above) {
        return 1.0f - smoothstep01(o.tilt_blend_height > 0.0f ? sole_above / o.tilt_blend_height
                                                              : (sole_above > 0.0f ? 1.0f : 0.0f));
    };

    f32 lowest = 0.0f;
    bool any = false;
    for (std::size_t i = 0; i < legs.size(); ++i) {
        FootTarget3D& out = plan.feet[i];
        const i32 end = legs[i].leg.end;
        if (!is_joint(sk, end)) {
            continue;
        }
        const Mat4f& ew = world[static_cast<std::size_t>(end)];
        const Vec3f ankle = translation_of(ew);
        out.target = ankle;
        out.rotation = rotation_of(ew);
        const f32 foot_height = legs[i].foot_height;
        const f32 sole_above = std::max(dot(ankle, up) - o.ground_level - foot_height, 0.0f);
        const Vec3f from = ankle + up * o.ray_up;
        const Vec3f to = ankle - up * (sole_above + foot_height + o.ray_down);
        const std::optional<GroundHit3D> hit = ground(from, to);
        f32 rise = 0.0f; // how far the ground there is above the floor the clips assume
        if (hit) {
            rise = dot(hit->position, up) - o.ground_level;
        }
        if (state) {
            f32& held = state->foot_offset[i];
            const f32 goal = hit ? rise : 0.0f;
            // Up at once (a foot never sinks into a step), down by easing.
            held = (!state->valid || goal >= held) ? goal : held + (goal - held) * k;
            if (hit) {
                rise = held;
            }
        }
        if (!hit) {
            if (locking) {
                state->locks[i] = {};
            }
            continue;
        }
        const Vec3f n = clamp_normal(hit->normal);
        const f32 tilt = tilt_amount(sole_above);
        const Vec3f follow = normalize(up + (n - up) * tilt);
        // The sole stands on the ground point (moved to the eased height), the ankle a foot height along the foot's up.
        const Vec3f ground_point = hit->position + up * (rise - (dot(hit->position, up) - o.ground_level));
        out.grounded = true;
        out.normal = follow;
        out.target = ground_point + follow * foot_height + up * sole_above;
        out.rotation = (rotation_between(up, follow) * out.rotation).normalized();
        out.rotation_weight = 1.0f;
        f32 foot_rise = rise;
        if (locking) {
            // Planted (the animated sole is on the ground): the target is held where it was put, in the world, while
            // the body moves; it lets go when the animated foot lifts or has moved too far from the held place, and
            // eases back.
            FootPlacementState3D::Lock& lock = state->locks[i];
            const bool on_ground = sole_above <= o.lock_height;
            if (lock.planted) {
                const f32 drift = length(to_model.apply(lock.point) - out.target);
                if (sole_above > o.lock_release_height || drift > o.lock_release_distance) {
                    lock.planted = false; // lock.blend is still 1: the target is where it was, then eases
                }
            }
            if (!lock.planted && lock.blend <= 0.0f && on_ground) {
                lock.planted = true;
                lock.blend = 1.0f;
                lock.point = to_world.apply(out.target);
                lock.rotation = (world_turn * out.rotation).normalized();
                lock.normal = world_turn.rotate(follow);
                lock.rise = rise;
            } else if (!lock.planted && lock.blend > 0.0f) {
                lock.blend = o.lock_replant_seconds > 0.0f && dt > 0.0f
                                 ? std::max(lock.blend - dt / o.lock_replant_seconds, 0.0f)
                                 : 0.0f;
            }
            if (lock.blend > 0.0f) {
                const f32 share = lock.planted ? 1.0f : smoothstep01(lock.blend);
                out.target = out.target + (to_model.apply(lock.point) - out.target) * share;
                out.rotation =
                    blend_rotation(out.rotation, (world_turn.conjugate() * lock.rotation).normalized(), share);
                out.normal = normalize(out.normal + (world_turn.conjugate().rotate(lock.normal) - out.normal) * share);
                foot_rise = rise + (lock.rise - rise) * share;
                out.locked = true;
            }
        }
        // The toe: the ground under it (a second query), its normal clamped like the foot's, followed only as far as
        // the foot is on the ground.
        const i32 toe = legs[i].toe;
        if (is_joint(sk, toe) && is_below(sk, toe, end)) {
            const Mat4f& tw = world[static_cast<std::size_t>(toe)];
            const Vec3f ball = translation_of(tw);
            const f32 toe_above = std::max(dot(ball, up) - o.ground_level - legs[i].toe_height, 0.0f);
            const std::optional<GroundHit3D> toe_hit =
                ground(ball + up * o.ray_up, ball - up * (toe_above + legs[i].toe_height + o.ray_down));
            if (toe_hit) {
                const Vec3f toe_n = clamp_normal(toe_hit->normal);
                const Vec3f toe_follow = normalize(up + (toe_n - up) * tilt);
                out.toe_grounded = true;
                out.toe_rotation = (rotation_between(up, toe_follow) * rotation_of(tw)).normalized();
                out.toe_weight = 1.0f;
                out.toe_max_degrees = o.toe_max_tilt_degrees;
            }
        }
        lowest = any ? std::min(lowest, foot_rise) : foot_rise;
        any = true;
    }
    f32 raw = any ? std::clamp(lowest, -std::max(o.max_pelvis_drop, 0.0f), std::max(o.max_pelvis_raise, 0.0f)) : 0.0f;
    if (state) {
        state->pelvis_offset = state->valid ? state->pelvis_offset + (raw - state->pelvis_offset) * k : raw;
        raw = state->pelvis_offset;
        state->valid = true;
    }
    plan.pelvis_offset = raw;
    return plan;
}

void apply_foot_placement(const Skeleton3D& sk, Pose3D& pose, std::span<Mat4f> world, i32 pelvis_joint,
                          std::span<const FootLeg3D> legs, const FootPlan3D& plan, const TwoBoneOptions3D& leg_options,
                          Vec3f up) {
    if (!usable(sk, pose, world)) {
        return;
    }
    if (plan.pelvis_offset != 0.0f && is_joint(sk, pelvis_joint)) {
        const Mat4f Fi = parent_world(sk, world, pelvis_joint).inverse();
        pose.joints[static_cast<std::size_t>(pelvis_joint)].translation +=
            Fi.apply_vector(normalize(up) * plan.pelvis_offset);
        update_world_below(sk, pose, world, pelvis_joint);
    }
    for (std::size_t i = 0; i < legs.size() && i < plan.feet.size(); ++i) {
        const FootTarget3D& foot = plan.feet[i];
        if (!foot.grounded) {
            continue;
        }
        TwoBoneOptions3D options = leg_options;
        options.match_rotation = foot.rotation_weight > 0.0f;
        options.end_rotation = foot.rotation;
        options.rotation_weight = foot.rotation_weight;
        solve_two_bone(sk, pose, world, legs[i].leg, foot.target, options);
        const i32 toe = legs[i].toe;
        if (foot.toe_grounded && is_joint(sk, toe) && is_below(sk, toe, legs[i].leg.end)) {
            // Seen after the leg has moved: the toe's world turn that lays it on the ground, as a change of its local
            // turn, held to a number of degrees from the animated one and blended by the leg's weight.
            const Quatf parent_turn = rotation_of(parent_world(sk, world, toe));
            JointPose3D& jp = pose.joints[static_cast<std::size_t>(toe)];
            const Quatf animated = jp.rotation;
            const Quatf wanted = (parent_turn.conjugate() * foot.toe_rotation).normalized();
            Quatf delta = (wanted * animated.conjugate()).normalized();
            if (delta.w < 0.0f) {
                delta = {-delta.x, -delta.y, -delta.z, -delta.w};
            }
            const f32 angle = 2.0f * std::acos(std::clamp(delta.w, -1.0f, 1.0f));
            const f32 most = std::max(foot.toe_max_degrees, 0.0f) * to_radians;
            if (angle > most && angle > 1e-6f) {
                const Vec3f axis = normalize(Vec3f{delta.x, delta.y, delta.z});
                delta = axis_angle_radians(axis, most);
            }
            const f32 share = std::clamp(leg_options.weight, 0.0f, 1.0f) * foot.toe_weight;
            if (share < 1.0f) {
                delta = blend_rotation(Quatf{}, delta, share);
            }
            jp.rotation = (delta * animated).normalized();
            update_world_below(sk, pose, world, toe);
        }
    }
}

} // namespace kin
