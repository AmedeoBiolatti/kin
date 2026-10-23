#include "game.hpp"

#include "../sponza_walk/character.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <kin/render3d/skeleton3d.hpp>
#include <kin/runtime/state_hash.hpp>

namespace summit {

namespace {

constexpr u32 save_magic = 0x544D5553u; // "SUMT"
constexpr u32 save_version = 1;

// A tiny writer and reader for the state's bytes (little endian on every machine kin runs on; floats bit for bit).
struct Out {
    std::vector<u8>& bytes;
    template <class T> void put(const T& v) {
        const u8* b = reinterpret_cast<const u8*>(&v);
        bytes.insert(bytes.end(), b, b + sizeof(T));
    }
    void v3(Vec3f v) {
        put(v.x);
        put(v.y);
        put(v.z);
    }
    void blob(std::span<const u8> b) {
        put<u64>(b.size());
        bytes.insert(bytes.end(), b.begin(), b.end());
    }
};

struct In {
    std::span<const u8> bytes;
    std::size_t at = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (!ok || at + sizeof(T) > bytes.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    Vec3f v3() {
        Vec3f v;
        v.x = get<f32>();
        v.y = get<f32>();
        v.z = get<f32>();
        return v;
    }
    std::span<const u8> blob() {
        const u64 n = get<u64>();
        if (!ok || n > bytes.size() - at) {
            ok = false;
            return {};
        }
        const std::span<const u8> out = bytes.subspan(at, std::size_t(n));
        at += std::size_t(n);
        return out;
    }
};

f32 wrap_degrees(f32 d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f)
        d += 360.0f;
    return d - 180.0f;
}

bool finite(Vec2f v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

// The yaw (degrees) a person model faces when it looks along `d` (the model faces +Z at 0).
f32 facing_of(Vec3f d) {
    return std::atan2(d.x, d.z) * 180.0f / pi;
}

// The controller as the rules ask for it.
CharacterDesc3D make_desc(const PlayerRules& r, Vec3f feet, u64 user_id, f32 stamina_cap) {
    CharacterDesc3D d;
    d.position = feet;
    d.radius = 0.3f;
    d.height = 1.95f;
    d.mass = 70.0f;
    d.max_slope_degrees = 46.0f;
    d.max_step_height = 0.4f;
    d.walk_speed = r.walk_speed;
    d.sprint_speed = r.sprint_speed;
    d.jump_speed = r.jump_speed;
    d.coyote_seconds = 0.1f;
    d.jump_buffer_seconds = 0.1f;
    d.user_id = user_id;
    TraversalDesc3D& t = d.traversal;
    t.enabled = true;
    t.reach = r.reach;
    t.limbs[0].shoulder = {-0.2f, 1.65f, 0.0f};
    t.limbs[1].shoulder = {0.2f, 1.65f, 0.0f};
    t.climb_speed = r.climb_speed;
    t.shimmy_speed = r.shimmy_speed;
    t.resource.max = r.max_stamina * stamina_cap;
    t.resource.regen_rate = r.regen_rate;
    t.resource.regen_delay = r.regen_delay;
    t.drain_one_hand = r.drain_one_hand;
    t.drain_two_hands = r.drain_two_hands;
    t.drain_moving = r.drain_moving;
    t.drain_sprint = r.drain_sprint;
    t.grab_cost = r.grab_cost;
    t.pull_up_cost = r.pull_up_cost;
    t.pull_up_min = r.pull_up_min;
    return d;
}

} // namespace

Shape3D decor_shape(const SummitLevel& level, const DecorChunk& chunk) {
    std::vector<Vec3f> pts = decor_chunk_points(level, chunk);
    const Quatf inverse = chunk.rotation.conjugate();
    for (Vec3f& p : pts)
        p = inverse.rotate(p - chunk.centre);
    return Shape3D::convex_hull(pts);
}

Vec3f look_vector(f32 yaw, f32 pitch) {
    return Quatf::yaw_pitch(yaw, pitch).rotate({0.0f, 0.0f, -1.0f});
}

struct Game::Impl {
    Skeleton3D skeleton;
    RagdollDesc desc;
    std::vector<Mat4f> model_world; // the rest pose, joint by joint, in the model's space
    std::vector<Mat4f> bones;       // scratch
};

Game::Game(u64 seed, GameConfig config)
    : _level(config.expedition ? generate_expedition_level(seed, config.dense_scenery) : generate_level(seed)),
      _config(std::move(config)) {
    if (_config.players == 0)
        _config.players = 1;
    build();
}

Game::~Game() = default;

void Game::build() {
    World3DConfig cfg;
    cfg.single_threaded = _config.single_threaded;
    // Nothing here reads the world's contact events (the game's own come from the character, ragdoll and rope events),
    // so none are recorded: each costs a callback under a lock, and the pile of a step has hundreds.
    cfg.contact_events = false;
    cfg.persist_events = false;
    _world = std::make_unique<World3D>(cfg);

    // The mountain: one static body for each piece, in the list's order, user id = index + 1.
    _piece_bodies.reserve(_level.pieces.size());
    for (std::size_t i = 0; i < _level.pieces.size(); ++i) {
        const Piece& p = _level.pieces[i];
        Body3DDesc d;
        d.shape = Shape3D::box(p.half);
        d.motion = Body3DMotion::Static;
        d.position = p.centre;
        d.rotation = p.rotation;
        d.friction = p.friction;
        d.user_id = u64(i) + 1;
        const Body3D b = _world->create_body(d);
        _piece_bodies.push_back(b);
        SurfaceMaterial3D m;
        m.grippable = p.grippable;
        m.grip = p.grip;
        m.slip = p.slip;
        m.drain = p.drain;
        m.tag = p.tag;
        _world->set_surface_material(b, m);
        if (p.hold) {
            (void)_world->set_hold(b, {.quality = p.hold_quality});
        }
    }

    // The decor rock (buttresses, scree, boulders): a static convex hull of each chunk's mesh, after the pieces' ids. A
    // buttress has the Cliff's surface, a stone the ground's (neither is a hold).
    _decor = decor_chunks(_level);
    _decor_bodies.reserve(_decor.size());
    for (std::size_t i = 0; i < _decor.size(); ++i) {
        const DecorChunk& c = _decor[i];
        const Piece proto; // the defaults are what Cliff and Ground pieces use
        Body3DDesc d;
        d.shape = decor_shape(_level, c);
        d.motion = Body3DMotion::Static;
        d.position = c.centre;
        d.rotation = c.rotation;
        d.friction = proto.friction;
        d.user_id = u64(_level.pieces.size()) + 1 + u64(i);
        const Body3D b = _world->create_body(d);
        _decor_bodies.push_back(b);
        SurfaceMaterial3D m;
        m.grippable = proto.grippable;
        m.grip = proto.grip;
        m.slip = proto.slip;
        m.drain = proto.drain;
        m.tag = proto.tag;
        _world->set_surface_material(b, m);
    }
    _props = solid_props(_level);
    for (std::size_t i = 0; i < _props.size(); ++i) {
        const PropPlacement& p = _props[i];
        Body3DDesc d;
        d.shape = prop_shape(p);
        d.motion = Body3DMotion::Static;
        d.position = p.feet;
        d.rotation = Quatf::axis_angle({0, 1, 0}, p.yaw);
        d.user_id = 100000 + u64(i);
        d.friction = 0.85f;
        const Body3D body = _world->create_body(d);
        _prop_bodies.push_back(body);
        SurfaceMaterial3D surface;
        surface.grippable = false;
        _world->set_surface_material(body, surface);
    }
    _world->optimize_broad_phase();

    for (const RopeSpec& r : _level.ropes) {
        RopeDesc3D d;
        d.start = RopeAnchor3D::at(r.anchor);
        d.end = RopeAnchor3D::free();
        d.length = r.length;
        d.segment_length = 0.25f;
        d.radius = 0.04f;
        d.mass_per_metre = 1.0f;
        d.hang_direction = {0.0f, -1.0f, 0.0f};
        _ropes.push_back(_world->create_rope(d));
    }

    // The person's skeleton, for the ragdoll (CPU only: no model or mesh is made here).
    _impl = std::make_unique<Impl>();
    {
        const sponza_walk::PersonRig rig = sponza_walk::person_rig();
        _impl->skeleton = rig.skeleton;
        _impl->desc = auto_ragdoll_desc(_impl->skeleton, _impl->skeleton.rest_pose());
        _impl->desc.animated_bodies = false;
        compute_world_matrices(_impl->skeleton, _impl->skeleton.rest_pose(), _impl->model_world);
        _skeleton_joints = _impl->skeleton.joint_count();
        _skeleton_names = _impl->skeleton.names;
        for (const RagdollBoneDesc3D& b : _impl->desc.bones)
            _ragdoll_joints.push_back(b.joint);
    }

    // The players: a character and a ragdoll each, made in player order.
    const Camp& camp = _level.camps.front();
    f32 yaw0 = 0.0f;
    for (const Leg& l : _level.route) {
        const Vec3f d = l.to - camp.spawn;
        if (std::abs(d.x) + std::abs(d.z) > 4.0f) {
            yaw0 = std::atan2(-d.x, -d.z) * 180.0f / pi;
            break;
        }
    }
    const Vec3f right = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, yaw0).rotate({1.0f, 0.0f, 0.0f});
    for (u32 i = 0; i < _config.players; ++i) {
        Player p;
        p.yaw = yaw0;
        p.facing_yaw = facing_of(look_vector(yaw0, 0.0f));
        p.health = _config.rules.max_health;
        p.last_stamina_cap = 1.0f;
        const Vec3f at = camp.spawn + right * (_config.spawn_spacing * (f32(i) - 0.5f * f32(_config.players - 1)));
        p.character = _world->create_character(make_desc(_config.rules, at, 1000000u + i, 1.0f));
        p.best_altitude = at.y;
        _players.push_back(p);
    }
    for (u32 i = 0; i < _config.players; ++i) {
        Player& p = _players[i];
        RagdollDesc desc = _impl->desc;
        desc.user_id = 2000000u + i;
        const std::vector<Mat4f> bones = rest_bones(_world->character(p.character).position, p.facing_yaw);
        p.ragdoll = _world->create_ragdoll(desc, bones);
        // Parked (no collisions, no cost) until it is needed: made Animated its bodies are kinematic at the T pose,
        // where another player's capsule would meet an arm.
        _world->animate_ragdoll(p.ragdoll);
        const std::vector<Body3D> bodies = _world->ragdoll_bodies(p.ragdoll);
        if (!bodies.empty() && bodies[0].valid()) {
            // Its own bones are not in the capsule's way, nor in the way of its hands and feet.
            _world->set_group(_world->character(p.character).body, _world->group(bodies[0]));
        }
    }
    _rng = make_key(_level.seed ^ 0x5355'4D4D'4954ull);
    _wind.direction = 40.0f;
    _wind.target_direction = 40.0f;
}

// The rest pose's bones in the world, the person standing at `feet` facing `facing_yaw`.
std::vector<Mat4f> Game::rest_bones(Vec3f feet, f32 facing_yaw) const {
    const Mat4f place = Mat4f::translation(feet) * Mat4f::rotation(Quatf::axis_angle({0.0f, 1.0f, 0.0f}, facing_yaw));
    std::vector<Mat4f> out;
    out.reserve(_impl->desc.bones.size());
    for (const RagdollBoneDesc3D& b : _impl->desc.bones) {
        out.push_back(place * _impl->model_world[std::size_t(b.joint)]);
    }
    return out;
}

void Game::emit(i32 player, GameEventKind kind, Vec3f point, f32 value) {
    _events.push_back({_step, player, kind, point, value});
}

void Game::debug_teleport(i32 player, Vec3f feet) {
    Player& p = _players[std::size_t(player)];
    _world->teleport_character(p.character, feet);
    p.air_speed = 0.0f;
    p.air_velocity = {};
}

void Game::debug_set_health(i32 player, f32 health) {
    _players[std::size_t(player)].health = std::clamp(health, 0.0f, _config.rules.max_health);
}

// ---- reading
// ----------------------------------------------------------------------------------------------------------

CharacterState3D Game::character_state(i32 player) const {
    return _world->character(_players[std::size_t(player)].character);
}
TraversalState3D Game::traversal(i32 player) const {
    return _world->traversal(_players[std::size_t(player)].character);
}
f32 Game::stamina(i32 player) const {
    return traversal(player).resource.value;
}
f32 Game::stamina_cap(i32 player) const {
    return traversal(player).resource.max;
}

Vec3f Game::feet(i32 player) const {
    const Player& p = _players[std::size_t(player)];
    return _world->character(p.character).position;
}

Vec3f Game::head(i32 player) const {
    const Player& p = _players[std::size_t(player)];
    if (p.state == PlayerState::Stunned || p.state == PlayerState::Dead) {
        const RagdollPlace3D place = _world->ragdoll_get_up_place(p.ragdoll, {0.0f, 1.0f, 0.0f}, all_layers3d, 6.0f);
        // The pelvis is the root bone: 0.2 above the ground point of the place when it stands; a lying body is lower.
        const std::vector<Body3D> bodies = _world->ragdoll_bodies(p.ragdoll);
        for (const Body3D b : bodies) {
            if (b.valid())
                return _world->position(b) + Vec3f{0.0f, 0.4f, 0.0f};
        }
        return place.position + Vec3f{0.0f, 0.5f, 0.0f};
    }
    return _world->character(p.character).position + Vec3f{0.0f, 1.65f, 0.0f};
}

Vec3f Game::look_direction(i32 player) const {
    const Player& p = _players[std::size_t(player)];
    return look_vector(p.yaw, p.pitch);
}

f32 Game::altitude(i32 player) const {
    return feet(player).y - _level.start.y;
}

void Game::ragdoll_pose(i32 player, std::vector<RagdollBonePose3D>& out) const {
    _world->ragdoll_pose(_players[std::size_t(player)].ragdoll, out);
}

// ---- the step
// ---------------------------------------------------------------------------------------------------------

void Game::step(std::span<const PlayerCommand> commands) {
    _events.clear();
    for (std::size_t i = 0; i < _players.size(); ++i) {
        const PlayerCommand c = i < commands.size() ? commands[i] : PlayerCommand{};
        step_player(i32(i), c);
    }
    (void)_world->step(fixed_step);
    _world->drain_constraint_events();
    for (std::size_t i = 0; i < _players.size(); ++i) {
        after_player(i32(i));
        if (_config.expedition)
            step_expedition(i32(i));
    }
    _world->drain_character_events();
    _world->drain_ragdoll_events();
    _world->drain_rope_events();

    // The wind wanders: a new target every two seconds from the state's own key.
    ++_step;
    if (_step % 120 == 0) {
        const auto [next, sub] = split(_rng);
        _rng = next;
        const auto [k1, k2] = split(sub);
        _wind.target_speed = rng_f32(k1, 1.0f, 11.0f);
        _wind.target_direction = _wind.direction + rng_f32(k2, -50.0f, 50.0f);
    }
    _wind.speed += (_wind.target_speed - _wind.speed) * 0.02f;
    _wind.direction += (_wind.target_direction - _wind.direction) * 0.02f;
}

void Game::step_player(i32 index, const PlayerCommand& raw) {
    Player& p = _players[std::size_t(index)];
    const PlayerRules& r = _config.rules;
    PlayerCommand c = raw;
    if (!finite(c.move))
        c.move = {};
    if (!finite(c.look))
        c.look = {};
    const f32 m = std::sqrt(c.move.x * c.move.x + c.move.y * c.move.y);
    if (m > 1.0f)
        c.move = {c.move.x / m, c.move.y / m};
    p.command = c;
    if (_config.expedition &&
        (p.expedition.phase == ExpeditionPhase::Briefing || p.expedition.phase == ExpeditionPhase::Complete)) {
        c.move = {};
        c.jump = c.sprint = c.grab_left = c.grab_right = c.pull_up = false;
    }

    // Looking is always allowed (a camera that cannot turn while you lie there is worse than none).
    p.yaw = wrap_degrees(p.yaw - c.look.x);
    p.pitch = std::clamp(p.pitch + c.look.y, r.pitch_min, r.pitch_max);

    CharacterInput3D in;
    TraversalInput3D t;
    if (p.state == PlayerState::Alive) {
        const Vec3f flat = look_vector(p.yaw, 0.0f);
        in.move = c.move;
        in.forward = flat;
        in.jump = c.jump;
        in.sprint = c.sprint;
        t.grab = {c.grab_left && !c.release, c.grab_right && !c.release};
        t.reach = look_vector(p.yaw, p.pitch);
        t.jump_off = c.jump;
        t.drop = c.release;
        // Up and over by itself: both hands on a lip and forward pressed (the level alternates, so that each is an
        // edge).
        const TraversalState3D ts = _world->traversal(p.character);
        bool want = c.pull_up;
        if (!want && ts.mode == TraversalMode3D::Hanging && ts.held == 2 && c.move.y > 0.3f && ts.grips[0].ledge &&
            ts.grips[1].ledge) {
            want = true;
        }
        t.pull_up = want && !p.pull_up_sent;
        p.pull_up_sent = t.pull_up;
    } else {
        p.pull_up_sent = false;
    }
    _world->set_character_input(p.character, in);
    _world->set_traversal_input(p.character, t);

    // Injury lowers the cap on stamina; the descriptor's number is live.
    const f32 cap =
        r.injury_stamina_floor + (1.0f - r.injury_stamina_floor) * std::clamp(p.health / r.max_health, 0.0f, 1.0f);
    if (cap != p.last_stamina_cap) {
        CharacterDesc3D d = make_desc(r, {}, 0, cap);
        // Only the numbers that are live matter to set_character_desc, but it takes a whole descriptor: start from the
        // ones the controller has been given.
        _world->set_character_desc(p.character, d);
        p.last_stamina_cap = cap;
        const f32 value = _world->traversal(p.character).resource.value;
        if (value > d.traversal.resource.max)
            _world->set_traversal_resource(p.character, d.traversal.resource.max);
    }
}

void Game::begin_stun(i32 index, Vec3f velocity, bool dead) {
    Player& p = _players[std::size_t(index)];
    const CharacterState3D cs = _world->character(p.character);
    p.state = dead ? PlayerState::Dead : PlayerState::Stunned;
    p.state_time = 0.0f;
    if (!dead) {
        ++p.stuns;
        emit(index, GameEventKind::Stunned, cs.position, p.air_speed);
    }
    // The bodies are put where the person stands now (the rest pose), then take over.
    _world->set_ragdoll_targets(p.ragdoll, rest_bones(cs.position, p.facing_yaw));
    _world->set_character_enabled(p.character, false);
    RagdollStart3D s;
    s.blend_seconds = 0.05f;
    s.set_velocity = true;
    s.velocity = {velocity.x, std::max(velocity.y, 0.0f), velocity.z};
    // Knocked backwards: an impulse at the chest, against the way the body faces, as a hard landing does.
    const Vec3f face = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, p.facing_yaw).rotate({0.0f, 0.0f, 1.0f});
    const f32 hit = std::clamp(p.air_speed, 4.0f, 30.0f) * 14.0f;
    for (std::size_t b = 0; b < _impl->desc.bones.size(); ++b) {
        if (_skeleton_names[std::size_t(_impl->desc.bones[b].joint)] == "chest") {
            s.impulse_bone = i32(b);
            break;
        }
    }
    s.impulse = face * -hit;
    _world->start_ragdoll(p.ragdoll, s);
}

bool Game::try_get_up(i32 index) {
    Player& p = _players[std::size_t(index)];
    const RagdollPlace3D place = _world->ragdoll_get_up_place(p.ragdoll, {0.0f, 1.0f, 0.0f},
                                                              all_layers3d & ~layer_bit(layers3d::Character), 3.0f);
    if (!place.found)
        return false;
    // The standing capsule must fit there, against everything that is not the ragdoll's own or the capsule's.
    const std::vector<Body3D> own = _world->ragdoll_bodies(p.ragdoll);
    QueryFilter3D filter;
    filter.mask = all_layers3d & ~layer_bit(layers3d::Character) & ~layer_bit(layers3d::Sensor);
    filter.ignore = _world->character(p.character).body;
    const Shape3D capsule = Shape3D::capsule(0.5f * (1.95f - 0.6f), 0.3f);
    const std::vector<OverlapHit3D> hits =
        _world->overlap(capsule, place.position + Vec3f{0.0f, 0.5f * 1.95f + 0.02f, 0.0f}, Quatf{}, filter);
    for (const OverlapHit3D& h : hits) {
        if (std::find(own.begin(), own.end(), h.body) != own.end())
            continue;
        if (h.penetration > 0.01f)
            return false;
    }
    // Where it lies, facing by the pelvis; the clip (standing) blends in.
    const Vec3f fwd = place.rotation.rotate({0.0f, 0.0f, 1.0f});
    p.facing_yaw = facing_of(fwd);
    _world->teleport_character(p.character, place.position);
    _world->set_character_enabled(p.character, true);
    _world->set_ragdoll_targets(p.ragdoll, rest_bones(place.position, p.facing_yaw));
    _world->ragdoll_get_up(p.ragdoll, _config.rules.get_up_seconds);
    p.state = PlayerState::GettingUp;
    p.state_time = 0.0f;
    return true;
}

void Game::respawn(i32 index) {
    Player& p = _players[std::size_t(index)];
    const PlayerRules& r = _config.rules;
    _world->animate_ragdoll(p.ragdoll);
    const Camp& camp = _level.camps[std::size_t(std::clamp(p.camp, 0, i32(_level.camps.size()) - 1))];
    const Vec3f right = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, p.yaw).rotate({1.0f, 0.0f, 0.0f});
    const Vec3f at = camp.spawn + right * (_config.spawn_spacing * (f32(index) - 0.5f * f32(_players.size() - 1)));
    _world->teleport_character(p.character, at);
    _world->set_character_enabled(p.character, true);
    p.health = r.max_health;
    p.last_stamina_cap = 1.0f;
    _world->set_character_desc(p.character, make_desc(r, at, 1000000u + u32(index), 1.0f));
    _world->set_traversal_resource(p.character, r.max_stamina);
    p.state = PlayerState::Alive;
    p.state_time = 0.0f;
    p.air_speed = 0.0f;
    p.air_velocity = {};
    p.pull_up_sent = false;
    emit(index, GameEventKind::Respawned, at, f32(p.camp));
}

void Game::after_player(i32 index) {
    Player& p = _players[std::size_t(index)];
    const PlayerRules& r = _config.rules;
    const f32 dt = fixed_step;
    const u64 user = 1000000u + u64(index);
    p.state_time += dt;

    if (p.state == PlayerState::Alive) {
        const CharacterState3D cs = _world->character(p.character);
        const TraversalState3D ts = _world->traversal(p.character);
        bool landed = false;
        f32 land_speed = 0.0f;
        for (const CharacterEvent3D& e : _world->character_events()) {
            if (e.user_id != user)
                continue;
            switch (e.kind) {
            case CharacterEventKind3D::Landed:
                landed = true;
                land_speed = std::abs(e.speed);
                emit(index, GameEventKind::Landed, cs.position, land_speed);
                break;
            case CharacterEventKind3D::LeftGround:
                if (e.jumped)
                    emit(index, GameEventKind::Jumped, cs.position, 0.0f);
                break;
            case CharacterEventKind3D::Gripped:
                emit(index, GameEventKind::Gripped, e.point, f32(e.hand));
                break;
            case CharacterEventKind3D::GripReleased:
                emit(index, GameEventKind::Released, e.point, f32(e.reason));
                break;
            case CharacterEventKind3D::Exhausted:
                emit(index, GameEventKind::Exhausted, cs.position, 0.0f);
                break;
            case CharacterEventKind3D::MantleBlocked:
                emit(index, GameEventKind::PullUpBlocked, e.point, f32(e.reason));
                break;
            case CharacterEventKind3D::MantleFinished:
                emit(index, GameEventKind::Mantled, cs.position, 0.0f);
                break;
            case CharacterEventKind3D::Fell:
                ++p.falls;
                emit(index, GameEventKind::Fell, cs.position, 0.0f);
                break;
            default:
                break;
            }
        }
        // A jump costs stamina (it also restarts the regeneration delay).
        if (cs.jumped) {
            const f32 v = std::max(0.0f, ts.resource.value - r.jump_cost);
            _world->set_traversal_resource(p.character, v);
        }
        // How fast it fell, for the ragdoll's start.
        if (cs.ground == CharacterGround3D::InAir && !ts.hanging()) {
            p.air_velocity = cs.velocity;
            p.air_speed = std::max(p.air_speed, -cs.vertical_speed);
        }
        if (landed) {
            const f32 speed = std::max(land_speed, p.air_speed);
            if (speed > r.safe_landing) {
                const f32 damage = (speed - r.safe_landing) * r.damage_per_speed;
                p.health = std::max(0.0f, p.health - damage);
                emit(index, GameEventKind::Hurt, cs.position, damage);
            }
            p.air_speed = speed;
            if (p.health <= 0.0f) {
                ++p.deaths;
                emit(index, GameEventKind::Died, cs.position, 0.0f);
                begin_stun(index, {p.air_velocity.x, 0.0f, p.air_velocity.z}, true);
            } else if (speed >= r.stun_speed) {
                begin_stun(index, {p.air_velocity.x, 0.0f, p.air_velocity.z}, false);
            }
            if (p.state == PlayerState::Alive)
                p.air_speed = 0.0f;
        } else if (cs.ground == CharacterGround3D::OnGround || ts.hanging()) {
            if (cs.ground == CharacterGround3D::OnGround)
                p.air_speed = 0.0f;
        }
        if (p.state == PlayerState::Alive) {
            // Health comes back slowly standing still, and fast in a camp.
            const bool still = cs.ground == CharacterGround3D::OnGround && cs.ground_speed < 0.3f;
            bool in_camp = false;
            for (const Camp& camp : _level.camps) {
                const Vec3f d = cs.position - camp.spawn;
                if (std::abs(d.x) < camp.radius && std::abs(d.z) < camp.radius && std::abs(d.y) < 2.5f) {
                    in_camp = true;
                    if (camp.index > p.camp) {
                        p.camp = camp.index;
                        emit(index, GameEventKind::Camp, camp.spawn, f32(camp.index));
                    }
                }
            }
            if (in_camp && cs.ground == CharacterGround3D::OnGround) {
                p.health =
                    std::min(r.max_health, p.health + r.camp_health_regen * dt * (p.command.interact ? 2.0f : 1.0f));
            } else if (still) {
                p.health = std::min(r.max_health, p.health + r.health_regen * dt);
            }
            p.best_altitude = std::max(p.best_altitude, cs.position.y);
            // The top.
            if (!p.summit && !_config.expedition) {
                const Vec3f d = cs.position - _level.summit;
                if (std::abs(d.x) < _level.summit_radius && std::abs(d.z) < _level.summit_radius &&
                    std::abs(d.y) < 4.0f) {
                    p.summit = true;
                    p.summit_step = i32(_step);
                    emit(index, GameEventKind::Summit, cs.position, 0.0f);
                }
            }
            // Lost: below the kill plane it is back at the camp.
            if (cs.position.y < _level.kill_y) {
                ++p.deaths;
                emit(index, GameEventKind::Died, cs.position, 1.0f);
                respawn(index);
            }
        }
        // Facing: into the rock while it hangs, the way it goes while it walks.
        if (p.state == PlayerState::Alive) {
            if (ts.hanging()) {
                Vec3f n = ts.surface_normal;
                n.y = 0.0f;
                if (length(n) > 0.1f)
                    p.facing_yaw = facing_of(-n);
            } else {
                const Vec3f fwd = look_vector(p.yaw, 0.0f);
                const Vec3f right{-fwd.z, 0.0f, fwd.x};
                const Vec3f d = fwd * p.command.move.y + right * p.command.move.x;
                if (length(d) > 0.1f) {
                    const f32 target = facing_of(d);
                    const f32 delta = wrap_degrees(target - p.facing_yaw);
                    const f32 maxturn = r.turn_rate * dt;
                    p.facing_yaw = wrap_degrees(p.facing_yaw + std::clamp(delta, -maxturn, maxturn));
                }
            }
        }
    } else if (p.state == PlayerState::Stunned || p.state == PlayerState::Dead) {
        // The capsule follows the pelvis's ground point (the camera and the feet read it).
        const RagdollPlace3D place = _world->ragdoll_get_up_place(p.ragdoll, {0.0f, 1.0f, 0.0f},
                                                                  all_layers3d & ~layer_bit(layers3d::Character), 6.0f);
        if (place.found)
            _world->teleport_character(p.character, place.position);
        const Vec3f where = place.found ? place.position : _world->character(p.character).position;
        if (where.y < _level.kill_y) {
            if (p.state == PlayerState::Stunned) {
                ++p.deaths;
                emit(index, GameEventKind::Died, where, 1.0f);
            }
            respawn(index);
        } else if (p.state == PlayerState::Dead) {
            if (p.state_time >= r.respawn_seconds)
                respawn(index);
        } else {
            const RagdollState3D rs = _world->ragdoll_state(p.ragdoll);
            const bool calm = rs.settled || rs.max_speed < 0.4f;
            if (p.state_time >= r.stun_seconds && calm) {
                if (try_get_up(index)) {
                    emit(index, GameEventKind::GotUp, _world->character(p.character).position, 0.0f);
                }
            }
            if (p.state == PlayerState::Stunned && p.state_time >= r.stun_timeout) {
                // No room or no ground: back to the camp, hurt.
                respawn(index);
            }
        }
    } else if (p.state == PlayerState::GettingUp) {
        if (p.state_time >= r.get_up_seconds) {
            p.state = PlayerState::Alive;
            p.state_time = 0.0f;
            p.air_speed = 0.0f;
        }
    }
}

// ---- state, hash, save
// --------------------------------------------------------------------------------------------------

void Game::serialise(std::vector<u8>& bytes) const {
    Out o{bytes};
    o.put<u64>(_step);
    o.put(_rng.lo);
    o.put(_rng.hi);
    o.put(_wind.speed);
    o.put(_wind.direction);
    o.put(_wind.target_speed);
    o.put(_wind.target_direction);
    o.put<u32>(u32(_players.size()));
    for (const Player& p : _players) {
        o.put<u32>(u32(p.state));
        o.put(p.state_time);
        o.put(p.yaw);
        o.put(p.pitch);
        o.put(p.facing_yaw);
        o.put(p.health);
        o.put(p.camp);
        o.put(p.falls);
        o.put(p.stuns);
        o.put(p.deaths);
        o.put<u8>(p.summit ? 1 : 0);
        o.put(p.summit_step);
        o.put(p.air_speed);
        o.v3(p.air_velocity);
        o.put(p.best_altitude);
        o.put<u8>(p.pull_up_sent ? 1 : 0);
        o.put(p.last_stamina_cap);
        o.put(p.command.move.x);
        o.put(p.command.move.y);
        o.put(p.command.look.x);
        o.put(p.command.look.y);
        o.put<u8>(u8((p.command.jump ? 1 : 0) | (p.command.sprint ? 2 : 0) | (p.command.grab_left ? 4 : 0) |
                     (p.command.grab_right ? 8 : 0) | (p.command.release ? 16 : 0) | (p.command.pull_up ? 32 : 0) |
                     (p.command.interact ? 64 : 0)));
        if (_config.expedition) {
            o.put<u8>(u8(p.expedition.phase));
            o.put(p.expedition.start_step);
            o.put(p.expedition.finish_step);
            o.put(p.expedition.beacon_steps);
            o.put(p.expedition.extraction_steps);
            o.put<u8>(p.expedition.shelter_visited ? 1 : 0);
        }
    }
}

u64 Game::state_hash() const {
    std::vector<u8> fields;
    serialise(fields);
    const u64 a = hash_bytes(fields.data(), fields.size());
    const u64 w = _world->state_hash();
    return hash_bytes(&w, sizeof w, a);
}

std::vector<u8> Game::save() const {
    std::vector<u8> bytes;
    Out o{bytes};
    o.put(save_magic);
    o.put<u32>(_config.expedition ? (_level.expedition.dense_scenery ? 4 : 3) : save_version);
    o.put<u64>(_level.seed);
    serialise(bytes);
    o.blob(_world->save_state());
    return bytes;
}

bool Game::restore(std::span<const u8> bytes) {
    In in{bytes};
    if (in.get<u32>() != save_magic ||
        in.get<u32>() != (_config.expedition ? (_level.expedition.dense_scenery ? 4 : 3) : save_version) ||
        in.get<u64>() != _level.seed || !in.ok)
        return false;
    const u64 step = in.get<u64>();
    RngKey rng;
    rng.lo = in.get<u32>();
    rng.hi = in.get<u32>();
    Wind wind;
    wind.speed = in.get<f32>();
    wind.direction = in.get<f32>();
    wind.target_speed = in.get<f32>();
    wind.target_direction = in.get<f32>();
    const u32 count = in.get<u32>();
    if (!in.ok || count != _players.size())
        return false;
    std::vector<Player> players = _players;
    for (Player& p : players) {
        p.state = static_cast<PlayerState>(in.get<u32>());
        p.state_time = in.get<f32>();
        p.yaw = in.get<f32>();
        p.pitch = in.get<f32>();
        p.facing_yaw = in.get<f32>();
        p.health = in.get<f32>();
        p.camp = in.get<i32>();
        p.falls = in.get<i32>();
        p.stuns = in.get<i32>();
        p.deaths = in.get<i32>();
        p.summit = in.get<u8>() != 0;
        p.summit_step = in.get<i32>();
        p.air_speed = in.get<f32>();
        p.air_velocity = in.v3();
        p.best_altitude = in.get<f32>();
        p.pull_up_sent = in.get<u8>() != 0;
        p.last_stamina_cap = in.get<f32>();
        p.command.move.x = in.get<f32>();
        p.command.move.y = in.get<f32>();
        p.command.look.x = in.get<f32>();
        p.command.look.y = in.get<f32>();
        const u8 flags = in.get<u8>();
        p.command.jump = flags & 1;
        p.command.sprint = flags & 2;
        p.command.grab_left = flags & 4;
        p.command.grab_right = flags & 8;
        p.command.release = flags & 16;
        p.command.pull_up = flags & 32;
        p.command.interact = flags & 64;
        if (_config.expedition) {
            p.expedition.phase = ExpeditionPhase(in.get<u8>());
            p.expedition.start_step = in.get<i32>();
            p.expedition.finish_step = in.get<i32>();
            p.expedition.beacon_steps = in.get<i32>();
            p.expedition.extraction_steps = in.get<i32>();
            p.expedition.shelter_visited = in.get<u8>() != 0;
            if (u8(p.expedition.phase) > u8(ExpeditionPhase::Complete) || p.expedition.beacon_steps < 0 ||
                p.expedition.beacon_steps >= beacon_hold_steps || p.expedition.extraction_steps < 0 ||
                p.expedition.extraction_steps >= extraction_hold_steps)
                return false;
        }
        if (u32(p.state) > u32(PlayerState::Dead))
            return false;
    }
    const std::span<const u8> world = in.blob();
    if (!in.ok || in.at != bytes.size())
        return false;
    if (!_world->restore_state(world))
        return false;
    _step = step;
    _rng = rng;
    _wind = wind;
    _players = std::move(players);
    _events.clear();
    return true;
}

} // namespace summit
