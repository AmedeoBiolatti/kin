#include "props.hpp"

#include "audio.hpp"
#include "prop_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace summit {

const PropGeometry& prop_geometry_data(PropKind kind) {
    return prop_geometry::all[kind];
}

bool solid_prop(PropKind kind) {
    return kind == Tent || kind == Firepit || kind == Crate || kind == Barrel || kind == Wood || kind == Tree ||
           kind == TallTree || kind == RockA || kind == RockB;
}

std::vector<PropPlacement> camp_props(const Camp& camp, std::size_t index) {
    const f32 yaw = f32(index) * 40.0f + 15.0f;
    const Quatf q = Quatf::axis_angle({0, 1, 0}, yaw);
    const auto at = [&](Vec3f offset) { return camp.marker + q.rotate(offset); };
    return {{Tent, camp.marker, 2.3f, yaw},
            {Firepit, camp_fire_position(camp), 0.95f, yaw},
            {Bedroll, at({0, 0.02f, 0.1f}), 1.7f, yaw},
            {PackedBedroll, at({1.55f, 0, 0.5f}), 0.65f, yaw + 80},
            {Crate, at({1.7f, 0, -0.55f}), 0.7f, yaw},
            {Bottle, at({1.7f, 0.7f, -0.55f}), 0.25f, yaw},
            {Barrel, at({-1.65f, 0, -0.9f}), 0.9f, yaw},
            {Wood, at({-1.55f, 0, 0.85f}), 0.85f, yaw}};
}

namespace {
u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
f32 random(u32 key) {
    return f32(mix(key) & 0xffffu) / 65535.0f;
}

// A vertical ray against authored ground/ramp OBBs, independent of the GPU or a physics world. It places vegetation on
// the same surface that supports the player, including the sloped approach; cave floors and roofs are deliberately
// excluded.
std::optional<f32> surface_height(const SummitLevel& level, f32 right, f32 forward) {
    const Vec3f world = level.expedition.rotation.rotate({right, 0, -forward});
    const Vec3f from{world.x, 100, world.z}, direction{0, -1, 0};
    std::optional<f32> best;
    for (const Piece& p : level.pieces) {
        if ((p.kind != PieceKind::Ground && p.kind != PieceKind::Ramp) || p.tag == 20)
            continue;
        const Quatf inverse = p.rotation.conjugate();
        const Vec3f origin = inverse.rotate(from - p.centre), ray = inverse.rotate(direction);
        const f32 o[]{origin.x, origin.y, origin.z}, d[]{ray.x, ray.y, ray.z}, half[]{p.half.x, p.half.y, p.half.z};
        f32 enter = 0, exit = 200;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(d[axis]) < 1e-6f) {
                if (std::abs(o[axis]) > half[axis]) {
                    exit = -1;
                    break;
                }
            } else {
                f32 a = (-half[axis] - o[axis]) / d[axis], b = (half[axis] - o[axis]) / d[axis];
                if (a > b)
                    std::swap(a, b);
                enter = std::max(enter, a);
                exit = std::min(exit, b);
            }
        }
        if (enter <= exit) {
            const f32 height = 100 - enter;
            if (!best || height > *best)
                best = height;
        }
    }
    return best;
}

bool clear_trail(const SummitLevel& level, Vec3f point, f32 clearance) {
    for (const auto* route : {&level.route, &level.expedition.sheltered_route})
        for (const Leg& leg : *route) {
            const Vec3f a = leg.from, b = leg.to, delta = b - a;
            const f32 horizontal2 = delta.x * delta.x + delta.z * delta.z;
            const f32 t =
                horizontal2 > .001f
                    ? std::clamp(((point.x - a.x) * delta.x + (point.z - a.z) * delta.z) / horizontal2, 0.0f, 1.0f)
                    : 0;
            const Vec3f near = a + delta * t, d = point - near;
            if (std::abs(d.y) < 3 && d.x * d.x + d.z * d.z < clearance * clearance)
                return false;
        }
    for (const Camp& camp : level.camps) {
        const Vec3f d = point - camp.marker;
        if (std::abs(d.y) < 3 && d.x * d.x + d.z * d.z < 20)
            return false;
    }
    return true;
}

void add_dense_scenery(const SummitLevel& level, std::vector<PropPlacement>& out) {
    const auto scatter = [&](f32 spacing, PropKind first, PropKind second, u32 salt, f32 min_span, f32 max_span,
                             f32 clearance, bool trees) {
        const int rows = int(194 / spacing), columns = int(88 / spacing);
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < columns; ++column) {
                if (out.size() >= 12000)
                    return;
                const u32 key = mix(u32(row) * 73856093u ^ u32(column) * 19349663u ^ u32(level.seed) ^ salt);
                const f32 forward = -25 + (f32(row) + .15f + .7f * random(key)) * spacing;
                const f32 right = -44 + (f32(column) + .15f + .7f * random(key + 1)) * spacing;
                const auto height = surface_height(level, right, forward);
                if (!height || *height > (trees ? 24 : 29) || random(key + 2) < .16f)
                    continue;
                const f32 span = min_span + (max_span - min_span) * random(key + 3);
                // A broad trailhead clearing keeps the camp visible and gives the third-person camera room behind the
                // player.
                if (trees && forward > -19 && forward < 5 && std::abs(right) < 8)
                    continue;
                const Vec3f feet = level.expedition.rotation.rotate({right, *height, -forward});
                if (!clear_trail(level, feet, clearance + (trees ? span * .12f : 0)))
                    continue;
                // Keep the blue cave approach readable and free of dressing where the route splits.
                if (forward > 50 && forward < 73 && right > 7 && right < 21)
                    continue;
                out.push_back({random(key + 4) < .5f ? first : second, feet, span, random(key + 5) * 360});
            }
    };
    scatter(4.0f, Tree, TallTree, 371u, 4.0f, 7.5f, 3.0f, true);
    scatter(3.1f, RockA, RockB, 911u, .5f, 1.7f, 1.4f, false);
    scatter(.9f, Grass, LargeGrass, 1399u, .45f, 1.1f, 1.4f, false);
}
} // namespace

std::vector<PropPlacement> scenery_props(const SummitLevel& level) {
    std::vector<PropPlacement> out;
    if (!level.expedition.enabled)
        return out;
    const Quatf rotation = level.expedition.rotation;
    const auto add = [&](PropKind kind, f32 forward, f32 right, f32 up, f32 span, f32 yaw) {
        out.push_back({kind, rotation.rotate({right, up, -forward}), span, yaw});
    };
    for (int side : {-1, 1}) {
        for (int i = 0; i < 4; ++i) {
            const f32 f = -14.0f + f32(i) * 6.5f;
            add(i % 2 ? Tree : TallTree, f, f32(side) * 10.0f, 0, 4.8f + f32(i % 3), f32(i * 71 + side * 35));
            add(i % 2 ? RockA : RockB, f + 2, f32(side) * 8.3f, 0, 0.8f + f32(i % 2) * 0.25f, f32(i * 31));
        }
        for (int i = 0; i < 12; ++i)
            add(i % 2 ? Grass : LargeGrass, -15.0f + f32(i) * 2.0f, f32(side) * (7.2f + f32(i % 3) * 0.6f), 0, 0.7f,
                f32(i * 53));
        for (int i = 0; i < 8; ++i) {
            const f32 f = 14.0f + f32(i) * 3.3f;
            add(LargeGrass, f, f32(side) * 3.8f, (f - 10.0f) * (12.0f / 32.0f), 0.7f, f32(i * 61));
        }
    }
    if (level.expedition.dense_scenery)
        add_dense_scenery(level, out);
    return out;
}

std::vector<PropPlacement> solid_props(const SummitLevel& level) {
    std::vector<PropPlacement> out;
    if (!level.expedition.enabled)
        return out;
    for (const PropPlacement& p : scenery_props(level))
        if (solid_prop(p.kind))
            out.push_back(p);
    for (std::size_t i = 0; i < level.camps.size(); ++i)
        for (const PropPlacement& p : camp_props(level.camps[i], i))
            if (solid_prop(p.kind))
                out.push_back(p);
    return out;
}

Shape3D prop_shape(const PropPlacement& prop) {
    if (!solid_prop(prop.kind))
        return {};
    if (prop.kind == Tree || prop.kind == TallTree) {
        // A small trunk proxy leaves foliage permeable. Centre the shape above its foot, just as the mesh is placed.
        const ShapeChild3D trunk{.shape = Shape3D::cylinder(prop.span * 0.12f, prop.span * 0.035f),
                                 .position = {0, prop.span * 0.12f, 0}};
        return Shape3D::compound({&trunk, 1});
    }
    const PropGeometry& data = prop_geometry_data(prop.kind);
    std::vector<Vec3f> points;
    points.reserve(data.points.size());
    for (Vec3f p : data.points)
        points.push_back(p * prop.span);
    // Cloth roofs and fire rings stay hollow. A convex hull would block the tent entrance and fill its interior.
    return data.indices.empty() ? Shape3D::convex_hull(points) : Shape3D::triangle_mesh(points, data.indices);
}

} // namespace summit
