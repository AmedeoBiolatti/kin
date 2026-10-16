#include "game.hpp"

#include <algorithm>
#include <cmath>

namespace summit {

SummitLevel generate_expedition_level(u64 seed, bool dense_scenery) {
    SummitLevel level;
    level.seed = seed;
    ExpeditionLayout& e = level.expedition;
    e.enabled = true;
    e.dense_scenery = dense_scenery;
    // An authored layout, rotated by the seed; its dimensions and the route choices stay learnable.
    e.rotation = Quatf::axis_angle({0, 1, 0}, f32(seed % 31) - 15.0f);
    const auto at = [&](f32 forward, f32 right, f32 up) { return e.rotation.rotate({right, up, -forward}); };
    const auto box = [&](PieceKind kind, f32 f0, f32 f1, f32 r0, f32 r1, f32 y0, f32 y1) {
        Piece p;
        p.kind = kind;
        p.centre = at((f0 + f1) * 0.5f, (r0 + r1) * 0.5f, (y0 + y1) * 0.5f);
        p.half = {(r1 - r0) * 0.5f, (y1 - y0) * 0.5f, (f1 - f0) * 0.5f};
        p.rotation = e.rotation;
        p.grippable = kind == PieceKind::Cliff || kind == PieceKind::Lip;
        level.pieces.push_back(p);
    };
    const auto ramp = [&](f32 f0, f32 f1, f32 y0, f32 y1, f32 r0, f32 r1, PieceKind kind = PieceKind::Ramp) {
        const f32 df = f1 - f0, dy = y1 - y0, thick = 1.0f;
        Piece p;
        p.kind = kind;
        p.rotation = e.rotation * Quatf::axis_angle({1, 0, 0}, std::atan2(dy, df) * 180.0f / pi);
        p.centre = at((f0 + f1) * 0.5f, (r0 + r1) * 0.5f, (y0 + y1) * 0.5f) - p.rotation.rotate({0, thick * 0.5f, 0});
        p.half = {(r1 - r0) * 0.5f, thick * 0.5f, std::sqrt(df * df + dy * dy) * 0.5f};
        p.grippable = false;
        p.friction = 0.9f;
        level.pieces.push_back(p);
    };
    const auto walk = [](Vec3f from, Vec3f to, const char* label, i32 camp = -1) {
        Leg l;
        l.from = from; l.to = to; l.label = label; l.camp = camp;
        return l;
    };
    const auto climb = [&](Vec3f from, Vec3f to, f32 height, const char* label) {
        Leg l = walk(from, to, label);
        l.kind = LegKind::Climb;
        l.normal = e.rotation.rotate({0, 0, 1});
        l.height = height;
        return l;
    };
    const auto rest = [&](Vec3f point, const char* label) {
        Leg l = walk(point, point, label);
        l.kind = LegKind::Rest; l.height = 1.0f;
        return l;
    };

    // Foothills: a narrow approach opens into the fork. The orange line climbs the face; the blue line enters the cave.
    box(PieceKind::Ground, -18, 12, -12, 12, -8, 0);
    ramp(10, 42, 0, 12, -5, 5);
    box(PieceKind::Ground, 41.8f, 54, -8, 22, 4, 12);
    box(PieceKind::Cliff, 54, 68, -7, 7, 4, 18);
    box(PieceKind::Ground, 68, 82, -8, 8, 10, 18);
    box(PieceKind::Ground, 70, 82, 8, 22, 10, 18);
    // The sheltered branch has actual walls, an inclined floor and an inclined ceiling, all in the same collider/mesh list.
    ramp(53.8f, 70.2f, 12, 18, 10, 18);
    level.pieces.back().tag = 20; // the cave floor is bare stone rather than the approach's grass
    ramp(53.8f, 70.2f, 15.8f, 21.8f, 9, 19, PieceKind::Roof);
    const Quatf cave_rotation = level.pieces.back().rotation;
    for (f32 side : {9.5f, 18.5f}) {
        Piece wall;
        wall.kind = PieceKind::Smooth;
        wall.centre = at(62, side, 16.4f);
        wall.half = {0.5f, 2.1f, std::sqrt(16.4f * 16.4f + 36.0f) * 0.5f};
        wall.rotation = cave_rotation;
        wall.grippable = false;
        level.pieces.push_back(wall);
    }
    // A second climb and the snow ridge provide a shared finish after the two branches join.
    ramp(80, 104, 18, 28, -5, 5);
    box(PieceKind::Ground, 103.8f, 114, -9, 9, 20, 28);
    box(PieceKind::Cliff, 114, 130, -8, 8, 20, 34);
    ramp(128, 155, 34, 44, -5, 5);
    box(PieceKind::Ground, 154.8f, 173, -10, 10, 36, 44);

    level.start = e.briefing = at(-6, 0, 0);
    e.fork = at(47, 0, 12);
    e.beacon = level.summit = at(163, 2, 44);
    e.extraction = at(165, -5, 44);
    e.shelter_centre = at(62, 14, 17);
    e.shelter_half = {3.7f, 5.0f, 7.5f};
    level.summit_radius = 2.5f;
    level.camps = {{0, level.start, 4, 0, at(-7, 6, 0)},
                   {1, at(76, 0, 18), 4, 18, at(76, -5, 18)},
                   {2, at(124, 0, 34), 4, 34, at(124, 5, 34)}};
    level.route = {walk(level.start, at(11, 0, 0), "leave base camp"),
                   walk(at(11, 0, 0), e.fork, "to the trail fork"),
                   walk(e.fork, at(53.05f, 0, 12), "follow the orange trail"),
                   climb(at(53.05f, 0, 12), at(55.2f, 0, 18), 6, "the exposed face"),
                   walk(at(55.2f, 0, 18), level.camps[1].spawn, "to the ridge camp", 1),
                   rest(level.camps[1].spawn, "rest at the ridge camp"),
                   walk(level.camps[1].spawn, at(105, 0, 28), "up the ridge"),
                   walk(at(105, 0, 28), at(113.05f, 0, 28), "to the final wall"),
                   climb(at(113.05f, 0, 28), at(115.2f, 0, 34), 6, "the final wall"),
                   walk(at(115.2f, 0, 34), level.camps[2].spawn, "to the snow camp", 2),
                   rest(level.camps[2].spawn, "rest at the snow camp"),
                   walk(level.camps[2].spawn, at(129, 0, 34), "to the snow ridge"),
                   walk(at(129, 0, 34), at(156, 0, 44), "along the snow ridge"),
                   walk(at(156, 0, 44), e.beacon, "to the beacon")};
    Leg finish = walk(e.beacon, e.beacon, "activate the beacon");
    finish.kind = LegKind::Summit;
    level.route.push_back(finish);
    e.sheltered_route.assign(level.route.begin(), level.route.begin() + 2);
    const std::vector<Leg> cave = {walk(e.fork, at(49, 14, 12), "follow the blue trail"),
                                 walk(at(49, 14, 12), at(55, 14, 12.45f), "enter the cave"),
                                 walk(at(55, 14, 12.45f), at(62, 14, 15), "inside the shelter"),
                                 rest(at(62, 14, 15), "recover in the shelter"),
                                 walk(at(62, 14, 15), at(72, 14, 18), "leave the cave"),
                                 walk(at(72, 14, 18), level.camps[1].spawn, "to the ridge camp", 1)};
    e.sheltered_route.insert(e.sheltered_route.end(), cave.begin(), cave.end());
    e.sheltered_route.insert(e.sheltered_route.end(), level.route.begin() + 5, level.route.end());
    e.direct_trail = {e.fork, at(52, 0, 12), at(55.5f, 0, 18), level.camps[1].spawn};
    e.sheltered_trail = {e.fork, at(49, 14, 12), at(55, 14, 12.45f), at(62, 14, 15), at(72, 14, 18), level.camps[1].spawn};
    const auto equipment = [&](u32 tag, f32 f0, f32 f1, f32 r0, f32 r1, f32 y0, f32 y1) {
        box(PieceKind::Equipment, f0, f1, r0, r1, y0, y1);
        level.pieces.back().tag = tag;
    };
    equipment(1, -6.2f, -5.8f, -2.1f, -1.9f, 0, 1.3f);
    equipment(1, -6.2f, -5.8f, -2.8f, -1.2f, 1.3f, 2.3f);
    equipment(2, 162.5f, 163.5f, 3.4f, 4.4f, 44, 45.4f);
    equipment(2, 162.8f, 163.2f, 3.7f, 4.1f, 45.4f, 49.5f);
    equipment(2, 162.4f, 163.6f, 2.8f, 5.0f, 49.0f, 49.2f);
    equipment(3, 162, 168, -8, -2, 44, 44.06f);
    // Sparse rock masses frame the route without hiding the entrances or crossing the paths.
    box(PieceKind::Boulder, 16, 26, -12, -7, -2, 5);
    box(PieceKind::Boulder, 30, 39, 7, 13, 5, 15);
    box(PieceKind::Boulder, 86, 96, -13, -8, 15, 31);
    box(PieceKind::Boulder, 141, 151, 7, 13, 32, 47);

    // A walkable forest apron surrounds the original trail rather than placing trees over the non-colliding distant terrain.
    if (dense_scenery) box(PieceKind::Ground, -28, 48, -48, 48, -4, -0.25f);
    Vec3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const Piece& p : level.pieces) {
        for (int x : {-1, 1}) for (int y : {-1, 1}) for (int z : {-1, 1}) {
            const Vec3f v = p.centre + p.rotation.rotate({p.half.x * x, p.half.y * y, p.half.z * z});
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
        }
    }
    level.bounds = {lo, hi};
    level.kill_y = -20;
    return level;
}

bool in_expedition_shelter(const SummitLevel& level, Vec3f feet) {
    const ExpeditionLayout& e = level.expedition;
    if (!e.enabled) return false;
    const Vec3f v = e.rotation.conjugate().rotate(feet - e.shelter_centre);
    return std::abs(v.x) < e.shelter_half.x && std::abs(v.y) < e.shelter_half.y && std::abs(v.z) < e.shelter_half.z;
}

ExpeditionInteraction Game::interaction(i32 index) const {
    if (!_config.expedition) return ExpeditionInteraction::None;
    const Player& p = player(index);
    if (p.state != PlayerState::Alive || character_state(index).ground != CharacterGround3D::OnGround || traversal(index).hanging())
        return ExpeditionInteraction::None;
    const ExpeditionLayout& e = _level.expedition;
    const auto near = [&](Vec3f target) {
        const Vec3f d = feet(index) - target;
        return d.x * d.x + d.z * d.z < 2.5f * 2.5f && std::abs(d.y) < 1.0f;
    };
    if (p.expedition.phase == ExpeditionPhase::Briefing && near(e.briefing)) return ExpeditionInteraction::Begin;
    if (p.expedition.phase == ExpeditionPhase::Climbing && near(e.beacon)) return ExpeditionInteraction::Beacon;
    if (p.expedition.phase == ExpeditionPhase::Extraction && near(e.extraction)) return ExpeditionInteraction::Extract;
    if (p.expedition.phase != ExpeditionPhase::Complete && in_expedition_shelter(_level, feet(index))) return ExpeditionInteraction::Recover;
    return ExpeditionInteraction::None;
}

void Game::step_expedition(i32 index) {
    Player& p = _players[std::size_t(index)];
    ExpeditionState& s = p.expedition;
    if (s.phase == ExpeditionPhase::Complete) return;
    const ExpeditionInteraction target = interaction(index);
    if (target == ExpeditionInteraction::Begin && p.command.interact) {
        s.phase = ExpeditionPhase::Climbing;
        s.start_step = i32(_step);
        emit(index, GameEventKind::ExpeditionStarted, feet(index));
        return;
    }
    const bool sheltered = in_expedition_shelter(_level, feet(index));
    if (sheltered && p.state == PlayerState::Alive && !s.shelter_visited) {
        s.shelter_visited = true;
        emit(index, GameEventKind::ShelterReached, feet(index));
    }
    if (target == ExpeditionInteraction::Recover && character_state(index).ground_speed < 0.3f) {
        p.health = std::min(_config.rules.max_health, p.health + _config.rules.camp_health_regen * fixed_step);
        _world->set_traversal_resource(p.character, std::min(stamina_cap(index), stamina(index) + 20.0f * fixed_step));
    }
    // Repairing or extracting requires a continuous hold on solid ground, in range. Leaving, releasing or falling resets it.
    s.beacon_steps = target == ExpeditionInteraction::Beacon && p.command.interact ? s.beacon_steps + 1 : 0;
    s.extraction_steps = target == ExpeditionInteraction::Extract && p.command.interact ? s.extraction_steps + 1 : 0;
    if (s.beacon_steps >= beacon_hold_steps) {
        s.phase = ExpeditionPhase::Extraction;
        s.beacon_steps = 0;
        emit(index, GameEventKind::BeaconActivated, _level.expedition.beacon);
    }
    if (s.extraction_steps >= extraction_hold_steps) {
        s.phase = ExpeditionPhase::Complete;
        s.extraction_steps = 0;
        s.finish_step = i32(_step);
        p.summit = true;
        p.summit_step = i32(_step);
        emit(index, GameEventKind::Summit, feet(index));
    }
}

} // namespace summit
