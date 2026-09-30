#include "workloads.hpp"
#include <kin/core/profile.hpp>
#include <algorithm>
#include <cmath>

namespace examples {
namespace {
Vec2f add(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
Vec2f sub(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
Vec2f mul(Vec2f a, float b) { return {a.x * b, a.y * b}; }
float length(Vec2f a) { return std::sqrt(a.x * a.x + a.y * a.y); }
Vec2f unit(Vec2f a) { const float n = length(a); return n > 0.001f ? mul(a, 1 / n) : Vec2f{1, 0}; }
int cell(Vec2f p) { return std::clamp(int(p.y / 64), 0, 31) * 48 + std::clamp(int(p.x / 64), 0, 47); }
constexpr std::array<Color, 3> colors{Color::rgb(241, 99, 103), Color::rgb(241, 182, 92), Color::rgb(161, 127, 237)};
}

Arena::Arena(int enemies, u64 seed) : _render(world), _movement(world.raw().query<Transform2D, Enemy>()) {
    enemies = std::clamp(enemies, 1, 5000);
    for (int i = 0; i < enemies; ++i) {
        const float size = i % 3 == 1 ? 22.0f : i % 3 == 0 ? 10.0f : 16.0f;
        _enemies.push_back(world.entity().set(Transform2D{}).set(Enemy{.slot = i, .kind = i % 3})
            .set(RectRenderer{.offset = {-size / 2, -size / 2}, .size = {size, size}, .color = colors[i % 3], .y_sort = true, .outline = i % 3 == 2}));
    }
    _next.resize(enemies);
    shots.reserve(8192); sparks.reserve(8192); pickups.reserve(512);
    _impacts.reserve(96);
    reset(seed);
}

float Arena::random(float lo, float hi) {
    const auto [next, value] = split(_rng); _rng = next; return rng_f32(value, lo, hi);
}

void Arena::respawn(EcsEntity entity) {
    auto* enemy = entity.get_mut<Enemy>();
    enemy->hp = enemy->kind == 1 ? 3.0f : 1.0f;
    enemy->cooldown = random(1, 4);
    const float angle = random(0, 6.283185f), radius = random(420, 1200);
    entity.get_mut<Transform2D>()->pos = {
        std::clamp(player.x + std::cos(angle) * radius, 24.0f, 3048.0f),
        std::clamp(player.y + std::sin(angle) * radius, 24.0f, 2024.0f)};
}

void Arena::reset(u64 seed) {
    _powers = 1; _spent = 0;
    _stats = {}; _regen = 0;
    _facing = {1, 0}; _travel = {};
    _impacts.clear(); _muzzle = 0; events.clear();
    _rng = make_key(seed); player = {1536, 1024}; health = 100;
    kills = collected = ticks = 0; elapsed = dash_cooldown = _fire = _hurt = _dash = 0;
    shots.clear(); sparks.clear(); pickups.clear();
    for (auto enemy : _enemies) respawn(enemy);
}

bool Arena::can_buy_power(int id) const {
    return id > 0 && id < int(power_ups.size()) && !has_power(id)
        && has_power(power_ups[id].parent) && available_cores() >= power_ups[id].cost
        && health > 0 && !won();
}

bool Arena::buy_power(int id) {
    if (!can_buy_power(id)) return false;
    const auto& power=power_ups[id];
    _spent += power.cost; _powers |= u64{1} << id;
    const float v=power.value;
    switch (power.effect) {
    case PowerEffect::Move: _stats.move+=v; break;
    case PowerEffect::DashCooldown: _stats.dash_cooldown+=v; break;
    case PowerEffect::DashDuration: _stats.dash_duration+=v; break;
    case PowerEffect::DashSpeed: _stats.dash_speed+=v; break;
    case PowerEffect::Damage: _stats.damage+=v; break;
    case PowerEffect::Fire: _stats.fire*=v; break;
    case PowerEffect::Magnet: _stats.magnet+=v; break;
    case PowerEffect::Pull: _stats.pull+=v; break;
    case PowerEffect::Hull: _stats.hull+=int(v); health=std::min(max_health(),health+int(v)); break;
    case PowerEffect::Repair: _stats.repair+=int(v); break;
    case PowerEffect::ShotSpeed: _stats.shot_speed+=v; break;
    case PowerEffect::ShotLife: _stats.shot_life+=v; break;
    case PowerEffect::Pellets: _stats.pellets+=int(v); break;
    case PowerEffect::Armor: _stats.armor+=int(v); break;
    case PowerEffect::Regen: _stats.regen+=v; break;
    case PowerEffect::None: break;
    }
    return true;
}

ArenaInput Arena::autopilot() const {
    Vec2f aim{1, 0}; float nearest = 1e9f;
    for (auto entity : _enemies) {
        const Vec2f delta = sub(entity.get<Transform2D>()->pos, player);
        const float distance = length(delta);
        if (distance < nearest) { nearest = distance; aim = unit(delta); }
    }
    return {.move = {std::cos(elapsed * .7f), std::sin(elapsed * .7f)}, .aim = aim,
        .fire = true, .dash = ticks % 240 == 0};
}

void Arena::burst(Vec2f pos) {
    if (_impacts.size()<96) _impacts.push_back({pos});
    for (int i = 0; i < 8 && sparks.size() < 8192; ++i)
        sparks.push_back({pos, {random(-100, 100), random(-100, 100)}, random(.15f, .5f)});
}

void Arena::step(float dt, ArenaInput input, bool invincible) {
    KIN_PROFILE_SCOPE("example.arena.step");
    if (health <= 0 || (won() && !invincible)) return;
    _facing = unit(input.aim); _travel = length(input.move) > 0 ? unit(input.move) : Vec2f{};
    _muzzle = std::max(0.0f,_muzzle-dt);
    for (auto& impact : _impacts) impact.life -= dt;
    std::erase_if(_impacts,[](const Impact& impact) {return impact.life<=0;});
    ++ticks; elapsed += dt; _fire -= dt; _hurt -= dt; _dash -= dt; dash_cooldown -= dt;
    _regen += dt*_stats.regen;
    if (_regen>=1) {const int repair=int(_regen); health=std::min(max_health(),health+repair); _regen-=repair;}
    if (input.dash && dash_cooldown <= 0) {
        _dash = _stats.dash_duration; dash_cooldown = _stats.dash_cooldown; event(ArenaEvent::Kind::Dash, player);
    }
    if (length(input.move) > 0) player = add(player, mul(unit(input.move), dt * (_dash > 0 ? _stats.dash_speed : _stats.move)));
    player.x = std::clamp(player.x, 20.0f, 3052.0f); player.y = std::clamp(player.y, 20.0f, 2028.0f);
    if (input.fire && _fire <= 0 && shots.size() < 8190) {
        _muzzle = .045f; event(ArenaEvent::Kind::Fire, player);
        _fire = std::max(.02f,std::max(.05f, .13f - collected * .002f) * _stats.fire);
        const Vec2f direction = unit(input.aim);
        shots.push_back({player, mul(direction, _stats.shot_speed), _stats.shot_life, false});
        if (collected >= 10) shots.push_back({player, {direction.x * (_stats.shot_speed-30) - direction.y * 120, direction.y * (_stats.shot_speed-30) + direction.x * 120}, _stats.shot_life, false});
        for (int i=0;i<_stats.pellets && shots.size()<8192;++i) {
            const float angle=(i%2==0?1.0f:-1.0f)*(.22f+.12f*(i/2));
            const float c=std::cos(angle), s=std::sin(angle);
            shots.push_back({player,{(direction.x*c-direction.y*s)*_stats.shot_speed,
                (direction.y*c+direction.x*s)*_stats.shot_speed},_stats.shot_life,false});
        }
    }
    _grid.fill(-1);
    _movement.each([&](flecs::entity, Transform2D& t, Enemy& e) {
        Vec2f toward = sub(player, t.pos); const float distance = length(toward);
        Vec2f direction = unit(toward);
        if (e.kind == 2 && distance < 330) direction = {-direction.y, direction.x};
        t.pos = add(t.pos, mul(direction, dt * (e.kind == 1 ? 48.0f : 80.0f) * (1 + elapsed / 180)));
        e.cooldown -= dt;
        if (e.kind == 2 && distance < 650 && e.cooldown <= 0 && shots.size() < 8192) {
            shots.push_back({t.pos, mul(unit(toward), 180), 4, true}); e.cooldown = 3;
            event(ArenaEvent::Kind::EnemyFire, t.pos, e.kind);
        }
        if (distance < 22 && _hurt <= 0 && _dash <= 0 && !invincible) {
            health -= std::max(1,8-_stats.armor); _hurt = .35f; event(ArenaEvent::Kind::Hurt, player);
        }
        const int bucket = cell(t.pos); _next[e.slot] = _grid[bucket]; _grid[bucket] = e.slot;
    });
    // Spatial buckets limit projectile collision candidates; no world-wide pair scan.
    for (auto& shot : shots) {
        shot.pos = add(shot.pos, mul(shot.velocity, dt)); shot.life -= dt;
        if (shot.hostile) {
            if (length(sub(shot.pos, player)) < 13 && _dash <= 0) {
                if (_hurt <= 0 && !invincible) {
                    health -= std::max(1,5-_stats.armor); _hurt = .2f; event(ArenaEvent::Kind::Hurt, player);
                }
                shot.life = 0;
            }
            continue;
        }
        const int cx = std::clamp(int(shot.pos.x / 64), 0, 47), cy = std::clamp(int(shot.pos.y / 64), 0, 31);
        for (int y = std::max(0, cy - 1); y <= std::min(31, cy + 1) && shot.life > 0; ++y)
            for (int x = std::max(0, cx - 1); x <= std::min(47, cx + 1) && shot.life > 0; ++x)
                for (int slot = _grid[y * 48 + x]; slot >= 0; slot = _next[slot]) {
                    auto entity = _enemies[slot]; auto* enemy = entity.get_mut<Enemy>();
                    if (enemy->hp <= 0 || length(sub(entity.get<Transform2D>()->pos, shot.pos)) > 15) continue;
                    shot.life = 0; enemy->hp -= _stats.damage; burst(shot.pos);
                    event(enemy->hp <= 0 ? ArenaEvent::Kind::Kill : ArenaEvent::Kind::Hit, shot.pos, enemy->kind);
                    if (enemy->hp <= 0) {
                        ++kills;
                        if (pickups.size() < 512) pickups.push_back({shot.pos});
                    }
                    break;
                }
    }
    for (auto entity : _enemies) if (entity.get<Enemy>()->hp <= 0) respawn(entity);
    for (auto& spark : sparks) { spark.pos = add(spark.pos, mul(spark.velocity, dt)); spark.life -= dt; }
    for (auto& pickup : pickups) {
        pickup.life -= dt; const auto delta = sub(player, pickup.pos);
        if (length(delta) < _stats.magnet) pickup.pos = add(pickup.pos, mul(unit(delta), std::min(length(delta),_stats.pull*dt)));
        if (length(delta) < 20) {
            ++collected; health = std::min(max_health(), health + _stats.repair); pickup.life = 0;
            event(ArenaEvent::Kind::Pickup, pickup.pos);
        }
    }
    std::erase_if(shots, [](const Shot& s) { return s.life <= 0; });
    std::erase_if(sparks, [](const Spark& s) { return s.life <= 0; });
    std::erase_if(pickups, [](const Pickup& s) { return s.life <= 0; });
}

void Arena::collect(RenderQueue& queue, const RenderView& view) {
    KIN_PROFILE_SCOPE("example.arena.collect");
    queue.clear(); _render.propagate_transforms(); _render.collect_dynamic(queue, {.view = &view});
    const auto ring = [&](Vec2f p,float radius,Color color,int layer) {
        constexpr std::array<Vec2f,9> points{{{1,0},{.7071f,.7071f},{0,1},{-.7071f,.7071f},
            {-1,0},{-.7071f,-.7071f},{0,-1},{.7071f,-.7071f},{1,0}}};
        for (int i=0;i<8;++i) queue.draw_line({.layer=layer},add(p,mul(points[i],radius)),add(p,mul(points[i+1],radius)),color);
    };
    for (const auto& impact : _impacts) {
        const float radius=5+(1-impact.life/.3f)*24;
        if (!render_view_visible(view,{impact.pos.x-radius,impact.pos.y-radius,radius*2,radius*2})) continue;
        ring(impact.pos,radius,Color::rgba(255,189,112,u8(std::clamp(impact.life*600,0.0f,180.0f))),3);
    }
    // Small, culled details distinguish silhouettes without textures or extra
    // entities. Enemy base bodies still come from the ECS render system.
    for (auto entity : _enemies) {
        const auto p = entity.get<Transform2D>()->pos;
        if (!render_view_visible(view, {p.x-16,p.y-16,32,32})) continue;
        const auto& e = *entity.get<Enemy>();
        const float s = e.kind == 1 ? 22 : e.kind == 0 ? 10 : 16;
        queue.fill_rect({.layer=-1},{p.x-s*.5f+3,p.y-s*.5f+5,s,s},Color::rgba(0,0,0,100));
        if (e.kind == 0) {
            queue.fill_rect({.layer=1},{p.x-3,p.y-2,6,3},Color::rgb(255,214,210));
        } else if (e.kind == 1) {
            queue.fill_rect({.layer=1},{p.x-7,p.y-7,14,14},Color::rgb(73,46,38));
            queue.fill_rect({.layer=1},{p.x-5,p.y-2,std::max(1.0f,e.hp)*3,4},Color::rgb(255,213,135));
        } else {
            queue.fill_rect({.layer=1},{p.x-4,p.y-4,8,8},e.cooldown < .45f ? Color::rgb(250,218,255) : Color::rgb(164,134,233));
            queue.draw_line({.layer=1},{p.x-12,p.y},{p.x+12,p.y},Color::rgb(164,134,233));
        }
    }
    for (auto& shot : shots) {
        if (!render_view_visible(view,{shot.pos.x-18,shot.pos.y-18,36,36})) continue;
        const auto tail = sub(shot.pos,mul(shot.velocity,shot.hostile ? .045f : .018f));
        queue.draw_line({.layer=2},tail,shot.pos,shot.hostile?Color::rgb(219,99,68):Color::rgb(68,157,176));
        queue.fill_rect({.layer=2},{shot.pos.x-3,shot.pos.y-3,6,6},shot.hostile?Color::rgb(255,151,91):Color::rgb(206,255,245));
    }
    for (auto& spark : sparks) {
        const Rectf bounds{spark.pos.x, spark.pos.y, 3, 3};
        if (render_view_visible(view, bounds)) queue.draw_line({.layer=3},sub(spark.pos,mul(spark.velocity,.025f)),spark.pos,
            Color::rgba(255,207,141,u8(std::clamp(spark.life*510,0.0f,255.0f))));
    }
    for (auto& pickup : pickups) {
        const Rectf bounds{pickup.pos.x - 7, pickup.pos.y - 7, 14, 14};
        if (render_view_visible(view, bounds)) {
            const float bob=std::sin(elapsed*3+pickup.pos.x*.02f)*2;
            queue.fill_rect({.layer=1},{pickup.pos.x-8,pickup.pos.y-8,16,16},Color::rgba(47,184,116,24));
            queue.draw_rect({.layer=1},{bounds.x,bounds.y+bob,bounds.w,bounds.h},Color::rgba(74,181,130,140));
            queue.fill_rect({.layer=1},{pickup.pos.x-3,pickup.pos.y-3,6,6},Color::rgb(140,255,189));
        }
    }
    if (_dash > 0) for (int i=3;i>0;--i) {
        const auto p = sub(player,mul(_travel,float(i)*15));
        queue.draw_rect({.layer=3},{p.x-10,p.y-10,20,20},Color::rgba(103,239,221,u8(130-i*30)));
    }
    const auto body = _hurt > 0 ? Color::rgb(255,179,163) : _dash > 0 ? Color::rgb(245,255,250) : Color::rgb(97,224,219);
    queue.fill_rect({.layer=3},{player.x-9,player.y-5,22,22},Color::rgba(0,0,0,130));
    queue.draw_rect({.layer=4},{player.x-14,player.y-14,28,28},Color::rgb(39,103,108));
    queue.fill_rect({.layer=4},{player.x-9,player.y-9,18,18},body);
    queue.fill_rect({.layer=4},{player.x-5,player.y-5,10,10},Color::rgb(24,51,65));
    const auto nose=add(player,mul(_facing,16));
    queue.draw_line({.layer=4},player,nose,Color::rgb(223,255,247));
    queue.fill_rect({.layer=4},{nose.x-2,nose.y-2,4,4},body);
    const Vec2f side{-_facing.y,_facing.x};
    const auto rear=sub(player,mul(_facing,11));
    // Directional wing pods and twin thrusters make the player identifiable
    // independently of color. These never alter its collision footprint.
    for (float sign : {-1.0f,1.0f}) {
        const auto wing=add(rear,mul(side,sign*15));
        queue.draw_line({.layer=4},nose,wing,body);
        queue.draw_line({.layer=4},wing,rear,Color::rgb(77,153,174));
        queue.fill_rect({.layer=4},{wing.x-2,wing.y-2,4,4},Color::rgb(221,255,247));
        if (length(_travel)>0) {
            const float thrust=(_dash>0?30.0f:10.0f)+3*std::sin(elapsed*40);
            queue.draw_line({.layer=3},wing,sub(wing,mul(_facing,thrust)),Color::rgb(78,184,240));
        }
    }
    if (_muzzle>0) {
        const auto tip=add(nose,mul(_facing,9));
        queue.draw_line({.layer=4},nose,tip,Color::rgb(255,248,194));
        queue.draw_line({.layer=4},sub(tip,mul(side,5)),add(tip,mul(side,5)),Color::rgb(255,218,137));
    }
    if (_hurt>0) ring(player,23,Color::rgba(255,136,119,170),4);
}

void Arena::use_enemy_textures(const std::array<Texture, 3>& textures, const std::array<Vec2f, 3>& sizes) {
    for (auto entity : _enemies) {
        const int kind = entity.get<Enemy>()->kind;
        const Vec2f size = sizes[kind];
        entity.remove<RectRenderer>();
        entity.set(TextureRenderer{.texture = textures[kind], .offset = {-size.x / 2, -size.y / 2}, .size = size,
            .layer = 1, .y_sort = true});
    }
}

void Arena::collect_entities(RenderQueue& queue, const RenderView& view) {
    queue.clear(); _render.propagate_transforms(); _render.collect_dynamic(queue, {.view = &view});
}

u64 Arena::checksum() const {
    u64 value = u64(ticks) * 7919 + kills * 97 + collected * 31 + health;
    if (_spent) value = value * 33 + _powers * 97 + _spent;
    for (auto e : _enemies) { const auto p = e.get<Transform2D>()->pos; value = value * 33 + u64(p.x * 16) + u64(p.y * 16); }
    return value;
}
}
