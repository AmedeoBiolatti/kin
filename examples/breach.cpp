#include "breach.hpp"

#include <kin/core/profile.hpp>

#include <algorithm>
#include <cmath>

namespace examples {
namespace {

constexpr float pi = 3.14159265f;
constexpr float degrees(float radians) { return radians * 180.0f / pi; }
constexpr std::array<float, 3> enemy_radius{0.32f, 0.4f, 0.3f};

// The horizontal view: forward (yaw 0 looks along -z) and right.
Vec3f forward_of(float yaw) {
    const float r = yaw * pi / 180.0f;
    return {-std::sin(r), 0.0f, -std::cos(r)};
}
Vec3f right_of(float yaw) {
    const float r = yaw * pi / 180.0f;
    return {std::cos(r), 0.0f, -std::sin(r)};
}
float wrap(float degrees_) {
    while (degrees_ > 180) degrees_ -= 360;
    while (degrees_ < -180) degrees_ += 360;
    return degrees_;
}
u64 bits(float v) { return static_cast<u64>(static_cast<i64>(std::lround(v * 16.0f))); }

} // namespace

BreachLevel::BreachLevel() : _cells(static_cast<std::size_t>(width * depth)) {
    const auto fill = [&](int x0, int z0, int x1, int z1, Cell cell) {
        for (int z = z0; z < z1; ++z) {
            for (int x = x0; x < x1; ++x) {
                _cells[static_cast<std::size_t>(z * width + x)] = cell;
            }
        }
    };
    const auto floor = [&](int x0, int z0, int x1, int z1, float h) { fill(x0, z0, x1, z1, {Kind::Floor, h, h, 0}); };
    const auto wall = [&](int x0, int z0, int x1, int z1) { fill(x0, z0, x1, z1, {Kind::Wall, wall_height, wall_height, 0}); };
    // A ramp from `lo` to `hi`, rising along `axis` (0 +x, 1 -x, 2 +z, 3 -z).
    const auto ramp = [&](int x0, int z0, int x1, int z1, float lo, float hi, u8 axis) {
        const int run = axis < 2 ? x1 - x0 : z1 - z0;
        for (int z = z0; z < z1; ++z) {
            for (int x = x0; x < x1; ++x) {
                const int i = axis == 0 ? x - x0 : axis == 1 ? x1 - 1 - x : axis == 2 ? z - z0 : z1 - 1 - z;
                _cells[static_cast<std::size_t>(z * width + x)] = {
                    Kind::Ramp, lo + (hi - lo) * float(i) / float(run), lo + (hi - lo) * float(i + 1) / float(run), axis};
            }
        }
    };
    // The rim.
    wall(0, 0, width, 1);
    wall(0, depth - 1, width, depth);
    wall(0, 0, 1, depth);
    wall(width - 1, 0, width, depth);
    // A raised middle, ramps up to it from four sides.
    floor(18, 12, 30, 20, 1.5f);
    ramp(22, 8, 26, 12, 0.0f, 1.5f, 2);
    ramp(22, 20, 26, 24, 0.0f, 1.5f, 3);
    ramp(14, 14, 18, 18, 0.0f, 1.5f, 0);
    ramp(30, 14, 34, 18, 0.0f, 1.5f, 1);
    // Four lower platforms in the corners, each with a ramp toward the middle.
    floor(3, 3, 9, 8, 1.0f);
    floor(39, 3, 45, 8, 1.0f);
    floor(3, 24, 9, 29, 1.0f);
    floor(39, 24, 45, 29, 1.0f);
    ramp(9, 4, 12, 7, 0.0f, 1.0f, 1);
    ramp(36, 4, 39, 7, 0.0f, 1.0f, 0);
    ramp(9, 25, 12, 28, 0.0f, 1.0f, 1);
    ramp(36, 25, 39, 28, 0.0f, 1.0f, 0);
    // Pillars to take cover behind.
    for (const auto [x, z] : std::array<std::pair<int, int>, 6>{{{13, 5}, {33, 5}, {13, 25}, {33, 25}, {7, 15}, {39, 15}}}) {
        wall(x, z, x + 2, z + 2);
    }
}

float BreachLevel::ground(float x, float z) const {
    const int cx = static_cast<int>(std::floor(x)), cz = static_cast<int>(std::floor(z));
    if (!inside(cx, cz)) {
        return wall_height;
    }
    const Cell& c = at(cx, cz);
    if (c.kind != Kind::Ramp) {
        return c.low;
    }
    const float fx = x - static_cast<float>(cx), fz = z - static_cast<float>(cz);
    const float t = c.axis == 0 ? fx : c.axis == 1 ? 1 - fx : c.axis == 2 ? fz : 1 - fz;
    return c.low + (c.high - c.low) * t;
}

bool BreachLevel::blocked(float x, float z, float radius, float feet) const {
    for (int cz = static_cast<int>(std::floor(z - radius)); cz <= static_cast<int>(std::floor(z + radius)); ++cz) {
        for (int cx = static_cast<int>(std::floor(x - radius)); cx <= static_cast<int>(std::floor(x + radius)); ++cx) {
            // The cell's point nearest the circle's centre.
            const float px = std::clamp(x, float(cx), float(cx + 1)), pz = std::clamp(z, float(cz), float(cz + 1));
            if ((px - x) * (px - x) + (pz - z) * (pz - z) >= radius * radius) {
                continue;
            }
            if (!inside(cx, cz) || at(cx, cz).kind == Kind::Wall) {
                return true;
            }
            const float top = at(cx, cz).kind == Kind::Ramp ? ground(px, pz) : at(cx, cz).low;
            if (top > feet + step) {
                return true;
            }
        }
    }
    return false;
}

std::optional<float> BreachLevel::raycast(Vec3f origin, Vec3f direction, float max) const {
    // Small steps: cheap enough for the few rays a step casts, and close to a
    // centimetre-scale hit.
    constexpr float stride = 0.05f;
    for (float t = 0.0f; t <= max; t += stride) {
        const Vec3f p = origin + direction * t;
        if (p.y < ground(p.x, p.z)) {
            return t;
        }
    }
    return std::nullopt;
}

bool BreachLevel::line_of_sight(Vec3f a, Vec3f b) const {
    const float d = length(b - a);
    return d < 1e-4f || !raycast(a, (b - a) / d, d);
}

Breach::Breach(int enemies, u64 seed)
    : _render(world), _movement(world.raw().query<Transform3D, BreachEnemy>()) {
    enemies = std::clamp(enemies, 1, max_enemies);
    world.raw().defer([&] {
        for (int i = 0; i < enemies; ++i) {
            _enemies.push_back(world.entity().set(Transform3D{}).set(BreachEnemy{.slot = i, .kind = i % 3}));
        }
    });
    _pos.resize(static_cast<std::size_t>(enemies));
    _enemy_at.resize(static_cast<std::size_t>(enemies));
    _hp.resize(static_cast<std::size_t>(enemies));
    shots.reserve(8192);
    pickups.reserve(512);
    reset(seed);
}

Breach::~Breach() = default;

float Breach::random(float lo, float hi) {
    const auto [next, value] = split(_rng);
    _rng = next;
    return rng_f32(value, lo, hi);
}

void Breach::respawn(kin::EcsEntity entity) {
    auto* enemy = entity.get_mut<BreachEnemy>();
    enemy->hp = enemy_max_hp(enemy->kind);
    _hp[static_cast<std::size_t>(enemy->slot)] = enemy->hp;
    enemy->cooldown = random(1, 4);
    enemy->spin = random(0, 360);
    // Somewhere over the deck, away from the player.
    Vec3f pos{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        pos = {random(2, BreachLevel::width - 2), 0.0f, random(2, BreachLevel::depth - 2)};
        if (length(Vec3f{pos.x - player.x, 0.0f, pos.z - player.z}) >= 12) break;
    }
    pos.y = _level.ground(pos.x, pos.z) + random(1.5f, 4.0f);
    entity.get_mut<Transform3D>()->position = pos;
    _pos[static_cast<std::size_t>(enemy->slot)] = pos;
    _enemy_at[static_cast<std::size_t>(enemy->slot)] = enemy;
}

void Breach::reset(u64 seed) {
    _rng = make_key(seed);
    player = {24.5f, 0.0f, 28.5f};
    velocity = {};
    yaw = 0;
    pitch = 0;
    grounded = true;
    health = 100;
    kills = collected = ticks = 0;
    elapsed = dash_cooldown = _fire = _hurt = _dash = 0;
    shots.clear();
    pickups.clear();
    events.clear();
    for (auto enemy : _enemies) respawn(enemy);
}

void Breach::use_enemy_meshes(const std::array<Mesh, 3>& meshes, const std::array<Material3D, 3>& materials) {
    world.raw().defer([&] {
        for (auto entity : _enemies) {
            const int kind = entity.get<BreachEnemy>()->kind;
            entity.set(MeshRenderer{.mesh = meshes[static_cast<std::size_t>(kind)], .material = materials[static_cast<std::size_t>(kind)]});
        }
    });
}

BreachInput Breach::autopilot() const {
    BreachInput in{.yaw = yaw, .pitch = pitch};
    const Vec3f eye_at = eye_position();
    // The nearest few enemies; the first in sight is the target.
    constexpr int candidates = 6;
    std::array<std::pair<float, int>, candidates> nearest;
    nearest.fill({1e30f, -1});
    for (int slot = 0; slot < static_cast<int>(_pos.size()); ++slot) {
        const float d = length_squared(_pos[static_cast<std::size_t>(slot)] - eye_at);
        if (d < nearest.back().first) {
            nearest.back() = {d, slot};
            std::sort(nearest.begin(), nearest.end());
        }
    }
    for (const auto& [d, slot] : nearest) {
        if (slot < 0) break;
        const Vec3f target = _pos[static_cast<std::size_t>(slot)];
        if (!_level.line_of_sight(eye_at, target)) continue;
        const Vec3f to = target - eye_at;
        const float want_yaw = degrees(std::atan2(-to.x, -to.z));
        const float want_pitch = degrees(std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z)));
        const float dyaw = wrap(want_yaw - yaw), dpitch = want_pitch - pitch;
        // Turned at most 3 degrees a step (360 a second at 120 steps).
        in.yaw = wrap(yaw + std::clamp(dyaw, -3.0f, 3.0f));
        in.pitch = std::clamp(pitch + std::clamp(dpitch, -2.0f, 2.0f), -85.0f, 85.0f);
        in.fire = std::abs(dyaw) < 4 && std::abs(dpitch) < 4;
        break;
    }
    // Round the deck, a waypoint every five seconds, jumping now and then.
    constexpr std::array<Vec2f, 6> waypoints{{{5, 12}, {24, 6}, {43, 12}, {43, 20}, {24, 26}, {5, 20}}};
    const Vec2f w = waypoints[static_cast<std::size_t>((ticks / 600) % static_cast<int>(waypoints.size()))];
    Vec3f to{w.x - player.x, 0.0f, w.y - player.z};
    if (length(to) > 0.5f) {
        to = normalize(to);
        in.move = {dot(to, right_of(in.yaw)), dot(to, forward_of(in.yaw))};
    }
    const float speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    in.jump = grounded && (ticks % 180 == 0 || (speed < 1.0f && ticks % 30 == 0));
    in.dash = _hurt > 0 && dash_cooldown <= 0;
    return in;
}

void Breach::move_player(float dt, const BreachInput& input) {
    Vec3f wish = right_of(yaw) * input.move.x + forward_of(yaw) * input.move.y;
    if (length(wish) > 1) wish = normalize(wish);
    const float speed = _dash > 0 ? 14.0f : 6.0f;
    const float accel = std::min(1.0f, (grounded ? 14.0f : 3.0f) * dt);
    velocity.x += (wish.x * speed - velocity.x) * accel;
    velocity.z += (wish.z * speed - velocity.z) * accel;
    if (input.jump && grounded) {
        velocity.y = 6.5f;
        grounded = false;
        event(BreachEvent::Kind::Jump, player);
    }
    velocity.y -= 20.0f * dt;
    // Along each axis in turn, stopped by walls and ledges too high to step onto.
    const float nx = player.x + velocity.x * dt;
    if (!_level.blocked(nx, player.z, radius, player.y)) player.x = nx; else velocity.x = 0;
    const float nz = player.z + velocity.z * dt;
    if (!_level.blocked(player.x, nz, radius, player.y)) player.z = nz; else velocity.z = 0;
    const float floor = _level.ground(player.x, player.z);
    const float ny = player.y + velocity.y * dt;
    if (ny <= floor || (grounded && velocity.y <= 0 && player.y - floor <= BreachLevel::step)) {
        // Landed, stepped up, or kept on the ground down a ramp or a step.
        player.y = floor;
        velocity.y = 0;
        grounded = true;
    } else {
        player.y = ny;
        grounded = false;
    }
}

void Breach::fire(const BreachInput&) {
    _fire = 0.1f;
    const Vec3f from = eye_position();
    const Vec3f dir = view_direction();
    const Quatf view = Quatf::yaw_pitch(yaw, pitch);
    const Vec3f muzzle = from + view.rotate({0.25f, -0.2f, -0.5f});
    float best = _level.raycast(from, dir, 80.0f).value_or(80.0f);
    int hit = -1;
    for (int slot = 0; slot < static_cast<int>(_pos.size()); ++slot) {
        const Vec3f oc = _pos[static_cast<std::size_t>(slot)] - from;
        const float along = dot(oc, dir);
        if (along < 0 || along - 1.0f > best) continue;
        const float r = enemy_radius[static_cast<std::size_t>(_enemy_at[static_cast<std::size_t>(slot)]->kind)];
        const float miss = length_squared(oc) - along * along;
        if (miss > r * r) continue;
        const float t = along - std::sqrt(r * r - miss);
        if (t < best) {
            best = t;
            hit = slot;
        }
    }
    const Vec3f at = from + dir * best;
    event(BreachEvent::Kind::Fire, muzzle, 0, at);
    if (hit < 0) return;
    BreachEnemy* enemy = _enemy_at[static_cast<std::size_t>(hit)];
    enemy->hp -= 1;
    _hp[static_cast<std::size_t>(hit)] = enemy->hp;
    event(enemy->hp <= 0 ? BreachEvent::Kind::Kill : BreachEvent::Kind::Hit, at, enemy->kind);
    if (enemy->hp <= 0) {
        ++kills;
        _killed.push_back(hit);
        if (pickups.size() < 512) pickups.push_back({_pos[static_cast<std::size_t>(hit)]});
    }
}

void Breach::step(float dt, BreachInput input, bool invincible) {
    KIN_PROFILE_SCOPE("example.breach.step");
    if (health <= 0 || (won() && !invincible)) return;
    ++ticks;
    elapsed += dt;
    _fire -= dt;
    _hurt -= dt;
    _dash -= dt;
    dash_cooldown -= dt;
    yaw = wrap(input.yaw);
    pitch = std::clamp(input.pitch, -85.0f, 85.0f);
    if (input.dash && dash_cooldown <= 0) {
        _dash = 0.18f;
        dash_cooldown = 1.5f;
    }
    move_player(dt, input);
    const Vec3f target = chest();
    {
        KIN_PROFILE_SCOPE("example.breach.move");
        _movement.run([&](flecs::iter& it) {
            while (it.next()) {
                const auto t = it.field<Transform3D>(0);
                const auto e = it.field<BreachEnemy>(1);
                const auto rows = static_cast<i32>(it.count());
                _distance.resize(static_cast<std::size_t>(rows));
                _aim.resize(static_cast<std::size_t>(rows));
                // Each enemy moves on its own, so big fights split across threads;
                // the arithmetic is the same either way.
                const auto advance = [&](i32 begin, i32 end) {
                    for (i32 i = begin; i < end; ++i) {
                        Transform3D& transform = t[i];
                        BreachEnemy& enemy = e[i];
                        Vec3f pos = transform.position;
                        const Vec3f to = target - pos;
                        const float distance = std::max(1e-3f, length(to));
                        const Vec3f dir = to / distance;
                        Vec3f v{};
                        const float pace = 1 + elapsed / 180;
                        if (enemy.kind == 0) {
                            v = dir * (4.5f * pace);
                        } else if (enemy.kind == 1) {
                            v = distance > 9 ? dir * (2.4f * pace) : distance < 7 ? dir * -1.5f : Vec3f{};
                        } else {
                            const Vec3f around = normalize(Vec3f{-dir.z, 0.0f, dir.x});
                            v = distance < 10 ? around * (3.6f * pace) : dir * (3.6f * pace);
                        }
                        pos += v * dt;
                        if (enemy.kind != 0) {
                            // Plates and rings hover at their own height over the player's.
                            const float want = target.y + (enemy.kind == 1 ? 1.5f : 0.8f) +
                                               0.3f * std::sin(elapsed * 1.3f + static_cast<float>(enemy.slot));
                            pos.y += (want - pos.y) * std::min(1.0f, 2.0f * dt);
                        }
                        pos.x = std::clamp(pos.x, 1.0f, BreachLevel::width - 1.0f);
                        pos.z = std::clamp(pos.z, 1.0f, BreachLevel::depth - 1.0f);
                        pos.y = std::max(pos.y, _level.ground(pos.x, pos.z) + 0.5f);
                        enemy.cooldown -= dt;
                        enemy.spin += dt * (enemy.kind == 2 ? 120.0f : 70.0f);
                        transform.position = pos;
                        transform.rotation = Quatf::axis_angle({0, 1, 0}, enemy.spin);
                        if (enemy.kind == 2) transform.rotation = transform.rotation * Quatf::axis_angle({1, 0, 0}, 60.0f);
                        _distance[static_cast<std::size_t>(i)] = distance;
                        _aim[static_cast<std::size_t>(i)] = dir;
                        _pos[static_cast<std::size_t>(enemy.slot)] = pos;
                        _enemy_at[static_cast<std::size_t>(enemy.slot)] = &enemy;
                    }
                };
                constexpr i32 parallel_min = 16384, chunk = 4096;
                if (rows >= parallel_min) {
                    if (!_jobs) _jobs = std::make_unique<JobSystem>();
                    _jobs->parallel_for((rows + chunk - 1) / chunk, [&](i32 c) { advance(c * chunk, std::min(rows, (c + 1) * chunk)); });
                } else {
                    advance(0, rows);
                }
                // Firing and damage depend on the order enemies are visited: serial,
                // in storage order.
                for (i32 i = 0; i < rows; ++i) {
                    const Vec3f pos = t[i].position;
                    BreachEnemy& enemy = e[i];
                    const float distance = _distance[static_cast<std::size_t>(i)];
                    if (enemy.kind != 0 && enemy.cooldown <= 0 && distance < 26 && shots.size() < 8192) {
                        if (_level.line_of_sight(pos, target)) {
                            shots.push_back({pos, _aim[static_cast<std::size_t>(i)] * (enemy.kind == 1 ? 6.5f : 9.0f), 5});
                            event(BreachEvent::Kind::EnemyFire, pos, enemy.kind);
                            enemy.cooldown = enemy.kind == 1 ? 3.0f : 2.5f;
                        } else {
                            enemy.cooldown = 0.5f; // look again soon
                        }
                    }
                    if (distance < 0.9f) {
                        if (_hurt <= 0 && _dash <= 0 && !invincible) {
                            health -= enemy.kind == 0 ? 10 : 5;
                            _hurt = 0.35f;
                            event(BreachEvent::Kind::Hurt, target);
                        }
                        if (enemy.kind == 0) _killed.push_back(enemy.slot); // a star spends itself
                    }
                }
            }
        });
    }
    if (input.fire && _fire <= 0) fire(input);
    KIN_PROFILE_SCOPE("example.breach.shots");
    for (BreachShot& shot : shots) {
        shot.pos += shot.velocity * dt;
        shot.life -= dt;
        if (shot.pos.y < _level.ground(shot.pos.x, shot.pos.z)) {
            shot.life = 0;
            continue;
        }
        // Against the player's body: the segment from the feet up.
        const float along = std::clamp(shot.pos.y - player.y, 0.0f, height);
        const Vec3f nearest{player.x, player.y + along, player.z};
        if (length(shot.pos - nearest) < radius + 0.15f && _dash <= 0) {
            if (_hurt <= 0 && !invincible) {
                health -= 5;
                _hurt = 0.2f;
                event(BreachEvent::Kind::Hurt, shot.pos);
            }
            shot.life = 0;
        }
    }
    // In slot order, as respawns draw from the random stream.
    std::sort(_killed.begin(), _killed.end());
    _killed.erase(std::unique(_killed.begin(), _killed.end()), _killed.end());
    for (const int slot : _killed) respawn(_enemies[static_cast<std::size_t>(slot)]);
    _killed.clear();
    for (BreachPickup& pickup : pickups) {
        pickup.life -= dt;
        const float floor = _level.ground(pickup.pos.x, pickup.pos.z) + 0.4f;
        if (pickup.pos.y > floor) pickup.pos.y = std::max(floor, pickup.pos.y - 4.0f * dt);
        const Vec3f delta = target - pickup.pos;
        const float distance = length(delta);
        if (distance < 4) pickup.pos += delta * (std::min(distance, 8.0f * dt) / std::max(distance, 1e-3f));
        if (distance < 1.0f) {
            ++collected;
            health = std::min(100, health + 2);
            pickup.life = 0;
            event(BreachEvent::Kind::Pickup, pickup.pos);
        }
    }
    std::erase_if(shots, [](const BreachShot& s) { return s.life <= 0; });
    std::erase_if(pickups, [](const BreachPickup& p) { return p.life <= 0; });
}

u64 Breach::checksum() const {
    u64 value = u64(ticks) * 7919 + u64(kills) * 97 + u64(collected) * 31 + u64(std::max(health, 0));
    value = value * 33 + bits(player.x);
    value = value * 33 + bits(player.y);
    value = value * 33 + bits(player.z);
    for (const Vec3f& p : _pos) value = (value * 33 + bits(p.x)) * 33 + bits(p.y) + bits(p.z) * 7;
    return value;
}

} // namespace examples
