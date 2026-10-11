#include "traversal3d_impl.hpp"

#include <kin/physics3d/traversal3d.hpp>
#include <kin/physics3d/world3d.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

// --- the resource ----------------------------------------------------------------------------------------------------

bool Resource3D::step(const ResourceConfig3D& config, f32 dt, f32 drain_per_second) {
    max = config.max;
    bool ran_out = false;
    if (drain_per_second > 0.0f) {
        // Being spent restarts the wait before it comes back.
        delay = config.regen_delay;
        value -= drain_per_second * dt;
    } else if (delay > 0.0f) {
        delay = std::max(0.0f, delay - dt);
    } else if (value < max) {
        value = std::min(max, value + config.regen_rate * dt);
    }
    if (value > max) {
        value = max;
    }
    if (value <= 0.0f) {
        value = 0.0f;
        if (!exhausted) {
            exhausted = true;
            ran_out = true;
        }
    } else if (exhausted && value >= config.recover_fraction * max) {
        exhausted = false;
    }
    return ran_out;
}

bool Resource3D::take(const ResourceConfig3D& config, f32 amount) {
    max = config.max;
    if (!(amount > 0.0f)) {
        return false;
    }
    delay = config.regen_delay;
    value -= amount;
    if (value <= 0.0f) {
        value = 0.0f;
        if (!exhausted) {
            exhausted = true;
            return true;
        }
    }
    return false;
}

// --- frames and shapes -----------------------------------------------------------------------------------------------

SurfaceFrame3D surface_frame(Vec3f normal, Vec3f up, Vec3f forward) {
    SurfaceFrame3D f;
    // A character facing the surface faces -normal; its right is cross(facing, up), as the controller's own command is.
    const Vec3f r = cross(-normal, up);
    if (length(r) > 0.2f) {
        f.right = normalize(r);
        f.up = normalize(cross(f.right, -normal));
        return f;
    }
    // A ceiling or a floor has no "up it": the way the character looks, across up.
    Vec3f h = forward - up * dot(forward, up);
    if (length(h) < 1.0e-4f) {
        h = cross(up, std::fabs(up.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f});
    }
    h = normalize(h);
    f.right = normalize(cross(h, up));
    f.up = h;
    return f;
}

namespace physics3d_detail {

Quatf capsule_rotation(Vec3f up) {
    // The half-way quaternion: axis = Y x up, w = 1 + Y.up, normalised (a half turn about X when upside down).
    if (up.y < -0.999999f) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    return Quatf{up.z, 0.0f, -up.x, 1.0f + up.y}.normalized();
}

namespace {

Vec3f flat(Vec3f v, Vec3f up) { return v - up * dot(v, up); }

// A shape per thread, remade only when its size changes (a probe is called every step for every hand).
Shape3D sphere_shape(f32 radius) {
    thread_local f32 r = -1.0f;
    thread_local Shape3D shape;
    if (r != radius || !shape.valid()) {
        shape = Shape3D::sphere(std::max(radius, 0.005f));
        r = radius;
    }
    return shape;
}

f32 clamp01(f32 v) { return std::clamp(v, 0.0f, 1.0f); }

// Where a body's point is now, and its local form.
Vec3f to_local(const World3D& w, Body3D b, Vec3f world) {
    return w.rotation(b).conjugate().rotate(world - w.position(b));
}

// The fan of casts: along `dir` and six round it. Each is a sphere swept `reach` long; the nearest hit of each, the
// surface refined by a ray at it (the sweep's contact point is the shape's, the ray's is the surface's own and its face
// normal exact). Sorted by distance, then body number, then place.
std::vector<RawGripHit3D> cast_fan(const World3D& world, const GripProbeDesc3D& p, Vec3f dir) {
    std::vector<RawGripHit3D> hits;
    static constexpr f32 ring[6][2] = {{1.0f, 0.0f},        {0.5f, 0.8660254f},  {-0.5f, 0.8660254f},
                                       {-1.0f, 0.0f},       {-0.5f, -0.8660254f}, {0.5f, -0.8660254f}};
    const Vec3f e1 = normalize(cross(dir, std::fabs(dir.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f}));
    const Vec3f e2 = cross(dir, e1);
    const Shape3D sphere = sphere_shape(p.radius);
    QueryFilter3D filter;
    filter.mask = p.mask;
    filter.ignore = p.ignore;
    const u32 count = p.spread > 0.0f ? 7u : 1u;
    for (u32 i = 0; i < count; ++i) {
        Vec3f d = dir;
        if (i > 0) {
            d = normalize(dir + (e1 * ring[i - 1][0] + e2 * ring[i - 1][1]) * p.spread);
        }
        const std::optional<ShapeHit3D> hit = world.shape_cast(sphere, p.origin, {}, d * p.reach, filter);
        if (!hit) {
            continue;
        }
        RawGripHit3D raw;
        raw.body = hit->body;
        raw.user_id = hit->user_id;
        raw.point = hit->point;
        raw.normal = hit->normal;
        const Vec3f to_hit = hit->point - p.origin;
        const f32 n = length(to_hit);
        if (hit->fraction > 0.0f && n > 1.0e-4f) {
            const std::optional<RayHit3D> ray =
                world.raycast(p.origin, hit->point + to_hit * (0.15f / n), filter);
            if (ray && ray->body == hit->body) {
                raw.point = ray->point;
                raw.normal = ray->normal;
            }
        }
        if (!(length(raw.normal) > 0.0f)) {
            continue;
        }
        raw.normal = normalize(raw.normal);
        const bool dup = std::any_of(hits.begin(), hits.end(), [&](const RawGripHit3D& o) {
            return o.body == raw.body && length_squared(o.point - raw.point) < 0.06f * 0.06f;
        });
        if (!dup) {
            hits.push_back(raw);
        }
    }
    std::ranges::sort(hits, [&](const RawGripHit3D& a, const RawGripHit3D& b) {
        const f32 da = length_squared(a.point - p.origin), db = length_squared(b.point - p.origin);
        if (da != db) return da < db;
        if (a.body.index != b.body.index) return a.body.index < b.body.index;
        if (a.point.x != b.point.x) return a.point.x < b.point.x;
        if (a.point.y != b.point.y) return a.point.y < b.point.y;
        return a.point.z < b.point.z;
    });
    return hits;
}

// Higher is better: a good grip, near, on a surface that faces the hand, and where the hand was aimed (the fan's other
// casts are for what the aim just misses).
f32 score_of(const GripCandidate3D& c, Vec3f origin, Vec3f dir, f32 reach) {
    const f32 closeness = 1.0f - 0.5f * clamp01(c.distance / std::max(reach, 1.0e-3f));
    const f32 facing = std::max(0.0f, dot(dir, -c.normal));
    const Vec3f to = c.point - origin;
    const f32 n = length(to);
    const f32 aim = n > 1.0e-4f ? std::max(0.0f, dot(to / n, dir)) : 1.0f;
    const f32 bonus = c.kind == GripKind3D::Hold ? 1.25f : c.kind == GripKind3D::Ledge ? 1.15f : 1.0f;
    return c.quality * closeness * (0.6f + 0.4f * facing) * (0.3f + 0.7f * aim * aim) * bonus;
}

GripAnchor3D anchor_of(const World3D& world, const GripCandidate3D& c) {
    GripAnchor3D a;
    a.body = c.body;
    a.kind = c.kind;
    a.local_point = to_local(world, c.body, c.point);
    a.local_normal = world.rotation(c.body).conjugate().rotate(c.normal);
    a.quality = c.quality;
    a.surface_tag = c.surface_tag;
    a.drain = c.drain;
    a.slip = c.slip;
    a.fixed = c.fixed;
    a.ledge = c.ledge;
    return a;
}

// Takes `point` of the body's surface along `normal` to the surface again after a move: a ray through the moved point
// from `normal * 0.25` out to 0.25 in. False when it hits another body or nothing.
bool reproject(const World3D& world, Body3D body, Vec3f point, Vec3f normal, Layer3DMask mask, Vec3f& out_point,
               Vec3f& out_normal) {
    QueryFilter3D filter;
    filter.mask = mask;
    const std::optional<RayHit3D> hit = world.raycast(point + normal * 0.25f, point - normal * 0.25f, filter);
    if (!hit || hit->body != body || !(length(hit->normal) > 0.0f)) {
        return false;
    }
    out_point = hit->point;
    out_normal = normalize(hit->normal);
    return true;
}

constexpr Layer3DMask climb_mask = all_layers3d & ~layer_bit(layers3d::Character) & ~layer_bit(layers3d::Sensor);

// --- providers ---------------------------------------------------------------------------------------------------------

class HoldProvider final : public GripProvider3D {
public:
    GripKind3D kind() const override { return GripKind3D::Hold; }

    void probe(const World3D& world, const GripProbeDesc3D& p, std::span<RawGripHit3D> hits,
               std::vector<GripCandidate3D>& out) const override {
        if (!p.holds) {
            return;
        }
        for (RawGripHit3D& h : hits) {
            if (h.claimed || !world.is_hold(h.body)) {
                continue;
            }
            // A hold is claimed even when it is too poor to take: the surface behind it is not offered in its place.
            h.claimed = true;
            const HoldDesc3D hd = world.hold(h.body);
            const SurfaceMaterial3D m = world.surface_material(h.body);
            GripCandidate3D c;
            c.body = h.body;
            c.user_id = h.user_id;
            c.point = h.point;
            c.normal = h.normal;
            if (hd.snap) {
                c.point = world.position(h.body) + world.rotation(h.body).rotate(hd.anchor);
                c.normal = normalize(world.rotation(h.body).rotate(hd.anchor_normal));
            } else {
                // A hold is taken from the front: the face a character looking along `forward` sees where the hand
                // went, not the side or the underside the sweep happened to graze (the hand is aimed past a small
                // thing). The place is the sweep's, moved 5 cm toward the hold's middle across the line of sight.
                const Vec3f middle = world.centre_of_mass_position(h.body);
                const Vec3f ahead = normalize(flat(p.forward, p.up));
                if (ahead != Vec3f{}) {
                    Vec3f across = middle - h.point;
                    across -= ahead * dot(across, ahead);
                    const f32 across_length = length(across);
                    const Vec3f aimed = h.point + (across_length > 1.0e-4f ? across * (std::min(0.05f, across_length) / across_length) : Vec3f{});
                    QueryFilter3D filter;
                    filter.mask = p.mask;
                    filter.ignore = p.ignore;
                    const std::optional<RayHit3D> ray =
                        world.raycast(aimed - ahead * (p.reach + 0.5f), aimed + ahead * 0.3f, filter);
                    if (ray && ray->body == h.body && length(ray->normal) > 0.0f) {
                        c.point = ray->point;
                        c.normal = normalize(ray->normal);
                    }
                }
            }
            c.kind = GripKind3D::Hold;
            c.quality = clamp01(hd.quality * m.grip);
            c.surface_tag = m.tag;
            c.drain = m.drain * hd.drain;
            c.slip = hd.slide ? m.slip : 0.0f;
            c.fixed = hd.snap || !hd.slide;
            if (c.quality < p.min_grip) {
                continue;
            }
            out.push_back(c);
        }
    }

    GripAnchor3D attach(const World3D& world, const GripCandidate3D& c) const override { return anchor_of(world, c); }

    bool move(const World3D& world, GripAnchor3D& a, Vec3f delta, Vec3f, Vec3f) const override {
        if (a.fixed) {
            return false;
        }
        const Vec3f p = world.position(a.body) + world.rotation(a.body).rotate(a.local_point);
        const Vec3f n = world.rotation(a.body).rotate(a.local_normal);
        Vec3f np, nn;
        if (!reproject(world, a.body, p + delta, n, climb_mask, np, nn)) {
            return false;
        }
        a.local_point = to_local(world, a.body, np);
        a.local_normal = world.rotation(a.body).conjugate().rotate(nn);
        return true;
    }
};

class LedgeProvider final : public GripProvider3D {
public:
    GripKind3D kind() const override { return GripKind3D::Ledge; }

    void probe(const World3D& world, const GripProbeDesc3D& p, std::span<RawGripHit3D> hits,
               std::vector<GripCandidate3D>& out) const override {
        if (!p.ledges) {
            return;
        }
        for (RawGripHit3D& h : hits) {
            if (h.claimed) {
                continue;
            }
            const SurfaceMaterial3D m = world.surface_material(h.body);
            if (!m.grippable || m.grip < p.min_grip || dot(h.normal, p.up) > 0.5f) {
                continue;
            }
            // A face that looks down (the underside of a ledge the hand came up under): the edge is on the side of the hand.
            Vec3f normal = h.normal;
            if (dot(normal, p.up) < -0.5f) {
                normal = normalize(flat(p.origin - h.point, p.up));
                if (normal == Vec3f{}) {
                    continue;
                }
            }
            LedgeInfo ledge = find_ledge(world, h.body, h.point, normal, p.up, p.ledge_search_height, 0.05f, p.mask, p.ignore);
            if (!ledge.found || length(ledge.lip - p.origin) > p.reach + p.radius) {
                continue;
            }
            ledge_room(world, ledge, p.up, p.clearance_radius, p.clearance_height, 0.05f, p.mask, p.ignore);
            h.claimed = true;
            GripCandidate3D c;
            c.body = h.body;
            c.user_id = h.user_id;
            c.point = ledge.lip;
            c.normal = normal;
            c.kind = GripKind3D::Ledge;
            c.quality = clamp01(m.grip);
            c.surface_tag = m.tag;
            c.drain = m.drain;
            c.slip = m.slip;
            c.fixed = false;
            c.ledge = true;
            c.ledge_top = ledge.stand;
            c.clear = ledge.clear;
            c.ledge_body = ledge.body;
            out.push_back(c);
        }
    }

    GripAnchor3D attach(const World3D& world, const GripCandidate3D& c) const override { return anchor_of(world, c); }

    bool move(const World3D& world, GripAnchor3D& a, Vec3f delta, Vec3f up, Vec3f) const override {
        // Along the edge only: the lip is at the top. The ray that finds the face goes a little below it (a ray along the
        // top plane itself grazes it), and the lip is found again from there.
        const Vec3f p = world.position(a.body) + world.rotation(a.body).rotate(a.local_point);
        const Vec3f n = world.rotation(a.body).rotate(a.local_normal);
        Vec3f np, nn;
        if (!reproject(world, a.body, p + flat(delta, up) - up * 0.05f, n, climb_mask, np, nn)) {
            return false;
        }
        const LedgeInfo ledge = find_ledge(world, a.body, np, nn, up, 0.3f, 0.1f, climb_mask, {});
        if (!ledge.found) {
            return false;
        }
        a.local_point = to_local(world, a.body, ledge.lip);
        a.local_normal = world.rotation(a.body).conjugate().rotate(nn);
        return true;
    }
};

class SurfaceProvider final : public GripProvider3D {
public:
    GripKind3D kind() const override { return GripKind3D::Surface; }

    void probe(const World3D& world, const GripProbeDesc3D& p, std::span<RawGripHit3D> hits,
               std::vector<GripCandidate3D>& out) const override {
        if (!p.surfaces) {
            return;
        }
        for (RawGripHit3D& h : hits) {
            if (h.claimed) {
                continue;
            }
            const SurfaceMaterial3D m = world.surface_material(h.body);
            // The ground is to be stood on, not held.
            if (!m.grippable || m.grip < p.min_grip || dot(h.normal, p.up) > 0.7f) {
                continue;
            }
            h.claimed = true;
            GripCandidate3D c;
            c.body = h.body;
            c.user_id = h.user_id;
            c.point = h.point;
            c.normal = h.normal;
            c.kind = GripKind3D::Surface;
            c.quality = clamp01(m.grip);
            c.surface_tag = m.tag;
            c.drain = m.drain;
            c.slip = m.slip;
            out.push_back(c);
        }
    }

    GripAnchor3D attach(const World3D& world, const GripCandidate3D& c) const override { return anchor_of(world, c); }

    bool move(const World3D& world, GripAnchor3D& a, Vec3f delta, Vec3f up, Vec3f) const override {
        const Vec3f p = world.position(a.body) + world.rotation(a.body).rotate(a.local_point);
        const Vec3f n = world.rotation(a.body).rotate(a.local_normal);
        Vec3f np, nn;
        if (!reproject(world, a.body, p + delta, n, climb_mask, np, nn)) {
            return false;
        }
        a.local_point = to_local(world, a.body, np);
        a.local_normal = world.rotation(a.body).conjugate().rotate(nn);
        // Going up a face, the hand finds the lip when it is near it and takes it: from there it is a ledge's grip.
        if (dot(delta, up) > 0.0f) {
            const LedgeInfo ledge = find_ledge(world, a.body, np, nn, up, 0.12f, 0.0f, climb_mask, {});
            if (ledge.found) {
                a.kind = GripKind3D::Ledge;
                a.ledge = true;
                a.local_point = to_local(world, a.body, ledge.lip);
            }
        }
        return true;
    }
};

} // namespace

Shape3D capsule_shape(f32 radius, f32 height) {
    thread_local f32 r = -1.0f, h = -1.0f;
    thread_local Shape3D shape;
    if (r != radius || h != height || !shape.valid()) {
        shape = Shape3D::capsule(std::max(0.5f * (height - 2.0f * radius), 0.01f), radius);
        r = radius;
        h = height;
    }
    return shape;
}

LedgeInfo find_ledge(const World3D& world, Body3D body, Vec3f point, Vec3f normal, Vec3f up, f32 search_up,
                     f32 search_down, Layer3DMask mask, Body3D ignore) {
    LedgeInfo ledge;
    Vec3f across = flat(normal, up);
    if (length(across) < 0.3f) {
        return ledge;
    }
    across = normalize(across);
    // A ray down a little behind the surface's plane: it meets the top of the body if the top is in range, and starts inside
    // the solid (a hit at once, which does not count) when it is not.
    constexpr f32 inset = 0.1f;
    QueryFilter3D filter;
    filter.mask = mask;
    filter.ignore = ignore;
    const Vec3f from = point + up * search_up - across * inset;
    const Vec3f to = point - up * search_down - across * inset;
    if (!(length(from - to) > 0.0f)) {
        return ledge;
    }
    const std::optional<RayHit3D> hit = world.raycast(from, to, filter);
    if (!hit || hit->body != body || hit->fraction < 1.0e-3f || dot(hit->normal, up) < 0.7f) {
        return ledge;
    }
    ledge.found = true;
    ledge.top = hit->point;
    ledge.lip = point + up * dot(hit->point - point, up);
    ledge.across = across;
    ledge.body = hit->body;
    return ledge;
}

void ledge_room(const World3D& world, LedgeInfo& ledge, Vec3f up, f32 radius, f32 height, f32 inset, Layer3DMask mask,
                Body3D ignore) {
    // The feet go a capsule's radius and `inset` beyond the lip, 5 mm above the top: that is where it is checked to fit (a
    // capsule touching the floor overlaps it by rounding). The pull-up's last move is down onto the top, where the controller
    // rests on a floor, so that it has no step of falling and no landing when the move ends.
    ledge.stand = ledge.lip - ledge.across * (radius + inset) + up * 0.005f;
    ledge.clear = false;
    QueryFilter3D filter;
    filter.mask = mask;
    filter.ignore = ignore;
    // A floor under it: the top must reach that far in.
    const std::optional<RayHit3D> floor = world.raycast(ledge.stand + up * 0.25f, ledge.stand - up * 0.25f, filter);
    if (!floor || dot(floor->normal, up) < 0.5f) {
        return;
    }
    if (!(height > 0.0f)) {
        ledge.clear = true;
        return;
    }
    const std::vector<OverlapHit3D> overlaps =
        world.overlap(capsule_shape(radius, height), ledge.stand + up * (0.5f * height), capsule_rotation(up), filter);
    ledge.clear = std::none_of(overlaps.begin(), overlaps.end(), [](const OverlapHit3D& o) { return o.penetration > 0.005f; });
}

} // namespace physics3d_detail

const GripProvider3D& grip_provider(GripKind3D kind) {
    static const physics3d_detail::HoldProvider hold;
    static const physics3d_detail::LedgeProvider ledge;
    static const physics3d_detail::SurfaceProvider surface;
    switch (kind) {
    case GripKind3D::Hold: return hold;
    case GripKind3D::Ledge: return ledge;
    case GripKind3D::Surface: break;
    }
    return surface;
}

std::vector<GripCandidate3D> World3D::probe_grips(const GripProbeDesc3D& p) const {
    std::vector<GripCandidate3D> out;
    const auto finite = [](Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(p.origin) || !finite(p.direction) || !finite(p.up) || !finite(p.forward) || !(p.reach > 0.0f) ||
        !std::isfinite(p.reach) || !(p.radius >= 0.0f)) {
        return out;
    }
    const Vec3f dir = normalize(p.direction);
    const Vec3f up = normalize(p.up);
    if (dir == Vec3f{} || up == Vec3f{}) {
        return out;
    }
    GripProbeDesc3D probe = p;
    probe.up = up;
    std::vector<RawGripHit3D> hits = physics3d_detail::cast_fan(*this, probe, dir);
    // Providers in priority order: a hit becomes one candidate.
    for (const GripKind3D kind : {GripKind3D::Hold, GripKind3D::Ledge, GripKind3D::Surface}) {
        grip_provider(kind).probe(*this, probe, hits, out);
    }
    for (GripCandidate3D& c : out) {
        c.distance = length(c.point - p.origin);
        const SurfaceFrame3D f = surface_frame(c.normal, up, p.forward);
        c.right = f.right;
        c.surface_up = f.up;
        c.score = physics3d_detail::score_of(c, p.origin, dir, p.reach);
    }
    std::ranges::sort(out, [](const GripCandidate3D& a, const GripCandidate3D& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.body.index != b.body.index) return a.body.index < b.body.index;
        if (a.point.x != b.point.x) return a.point.x < b.point.x;
        if (a.point.y != b.point.y) return a.point.y < b.point.y;
        return a.point.z < b.point.z;
    });
    if (out.size() > p.max_candidates) {
        out.resize(p.max_candidates);
    }
    return out;
}

} // namespace kin
