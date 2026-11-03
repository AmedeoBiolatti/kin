#include <kin/core/jobs.hpp>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/render.hpp>
#include <kin/pathfinding/navigation2d.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace kin {
namespace {
f32 length(Vec2f v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}
Vec2f sub(Vec2f a, Vec2f b) {
    return {a.x - b.x, a.y - b.y};
}
Vec2f mul(Vec2f a, f32 b) {
    return {a.x * b, a.y * b};
}
Vec2f add(Vec2f a, Vec2f b) {
    return {a.x + b.x, a.y + b.y};
}
WorldTransform transform(EcsEntity entity) {
    std::vector<Transform2D> parents;
    for (std::size_t guard = 0; entity && guard < 256; ++guard, entity = entity.parent())
        parents.push_back(entity.get<Transform2D>() ? *entity.get<Transform2D>() : Transform2D{});
    WorldTransform result;
    for (auto it = parents.rbegin(); it != parents.rend(); ++it)
        result = compose(result, *it);
    return result;
}
u64 lifetime(EcsEntity entity) {
    const auto* value = entity.get<EntityLifetime>();
    return value ? value->generation : 0;
}
} // namespace
void register_navigation2d_components(EcsWorld& world) {
    if (world.components().find("NavigationAgent2D"))
        return;
    world.components()
        .native<NavigationAgent2D>("NavigationAgent2D")
        .field("target", &NavigationAgent2D::target)
        .field("speed", &NavigationAgent2D::speed)
        .field("radius", &NavigationAgent2D::radius)
        .field("arrival_distance", &NavigationAgent2D::arrival_distance)
        .field("waypoint_distance", &NavigationAgent2D::waypoint_distance)
        .field("neighbour_distance", &NavigationAgent2D::neighbour_distance)
        .field("avoidance_weight", &NavigationAgent2D::avoidance_weight)
        .field("repath_interval", &NavigationAgent2D::repath_interval)
        .field("stuck_seconds", &NavigationAgent2D::stuck_seconds)
        .field("enabled", &NavigationAgent2D::enabled)
        .field("avoidance", &NavigationAgent2D::avoidance)
        .field("apply_motion", &NavigationAgent2D::apply_motion)
        .field("velocity", &NavigationAgent2D::velocity)
        .field("arrived", &NavigationAgent2D::arrived)
        .field("stuck", &NavigationAgent2D::stuck)
        .field("unreachable", &NavigationAgent2D::unreachable);
}
Navigation2D::Navigation2D(EcsWorld& world, std::shared_ptr<const NavGridSnapshot> snapshot,
                           NavigationOptions2D options)
    : _world(world), _options(options) {
    if (options.requests_per_step < 1 || !options.max_agents)
        throw std::invalid_argument("invalid navigation budget");
    register_navigation2d_components(world);
    set_snapshot(std::move(snapshot));
}
Navigation2D::~Navigation2D() = default;
void Navigation2D::set_snapshot(std::shared_ptr<const NavGridSnapshot> snapshot) {
    if (!snapshot || snapshot->cols <= 0 || snapshot->rows <= 0 || !std::isfinite(snapshot->tile_size) ||
        snapshot->tile_size < 1e-6f || static_cast<u64>(snapshot->cols) * snapshot->rows > 4194304 ||
        snapshot->blocked.size() != static_cast<std::size_t>(snapshot->cols) * snapshot->rows)
        throw std::invalid_argument("invalid navigation snapshot");
    if (static_cast<f64>(snapshot->cols) * snapshot->tile_size > 1e9 ||
        static_cast<f64>(snapshot->rows) * snapshot->tile_size > 1e9)
        throw std::invalid_argument("navigation world extent exceeds 1 billion units");
    _servers.clear();
    _entries.clear();
    _snapshot = std::move(snapshot);
}
bool Navigation2D::free(Vec2f p, f32 radius) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(radius) || radius < 0)
        return false;
    const auto& grid = *_snapshot;
    const f32 size = grid.tile_size;
    if (p.x - radius < 0 || p.y - radius < 0 || p.x + radius >= grid.cols * size || p.y + radius >= grid.rows * size)
        return false;
    const i32 min_x = std::max(0, static_cast<i32>(std::floor((p.x - radius) / size)));
    const i32 min_y = std::max(0, static_cast<i32>(std::floor((p.y - radius) / size)));
    const i32 max_x = std::min(grid.cols - 1, static_cast<i32>(std::floor((p.x + radius) / size)));
    const i32 max_y = std::min(grid.rows - 1, static_cast<i32>(std::floor((p.y + radius) / size)));
    for (i32 y = min_y; y <= max_y; ++y)
        for (i32 x = min_x; x <= max_x; ++x)
            if (grid.is_blocked({x, y})) {
                const f32 dx = p.x - std::clamp(p.x, x * size, (x + 1) * size);
                const f32 dy = p.y - std::clamp(p.y, y * size, (y + 1) * size);
                if (dx * dx + dy * dy <= radius * radius)
                    return false;
            }
    return true;
}
bool Navigation2D::clear(Vec2f a, Vec2f b, f32 radius) const {
    const f32 distance = length(sub(b, a));
    if (!std::isfinite(distance) || distance > _snapshot->tile_size * 512)
        return false;
    const auto steps = static_cast<std::size_t>(std::ceil(distance / (_snapshot->tile_size * 0.125f)));
    if (steps > 4096)
        return false;
    for (std::size_t i = 1; i <= std::max<std::size_t>(1, steps); ++i)
        if (!free(add(a, mul(sub(b, a), static_cast<f32>(i) / std::max<std::size_t>(1, steps))), radius))
            return false;
    return true;
}
Navigation2D::Server& Navigation2D::server(i32 radius) {
    if (auto it = _servers.find(radius); it != _servers.end())
        return it->second;
    auto inflated = std::make_shared<NavGridSnapshot>(*_snapshot);
    const f32 world_radius = radius * _snapshot->tile_size / 8;
    for (i32 y = 0; y < inflated->rows; ++y)
        for (i32 x = 0; x < inflated->cols; ++x)
            inflated->blocked[static_cast<std::size_t>(y) * inflated->cols + x] =
                !free(inflated->tile_center({x, y}), world_radius);
    auto paths =
        std::make_unique<PathServer>(PathServerConfig{.jobs = _options.jobs ? _options.jobs : &default_job_system()});
    paths->set_snapshot(inflated);
    return _servers.emplace(radius, Server{inflated, std::move(paths)}).first->second;
}
void Navigation2D::update(f32 dt) {
    if (!std::isfinite(dt) || dt < 0)
        throw std::invalid_argument("invalid navigation dt");
    _events.clear();
    for (auto& [radius, server] : _servers)
        server.paths->drain(PathServer::DrainMode::BarrierOnAdmitted, [&](PathServerResult&& result) {
            auto found = _entries.find(result.user_key);
            auto entity = _world.entity_by_id(result.user_key);
            if (found == _entries.end() || !entity || lifetime(entity) != found->second.lifetime ||
                found->second.radius != radius || found->second.request != result.request_id)
                return;
            const auto* current = entity.get<NavigationAgent2D>();
            if (!current)
                return;
            found->second.request = 0;
            found->second.path = std::move(result.path);
            found->second.next = 0;
            // PathServer considers destinations within two world units done.
            // Agents can request a tighter arrival tolerance: finish that short
            // segment ourselves, with the same radius/obstacle checks as motion.
            if (found->second.path.empty() &&
                clear(transform(entity).pos, found->second.request_target, current->radius))
                found->second.path.push_back(found->second.request_target);
            if (!found->second.path.empty() && found->second.path.back() != found->second.request_target)
                found->second.path.push_back(found->second.request_target);
            if (auto* agent = entity.get_mut<NavigationAgent2D>();
                agent && agent->target == found->second.request_target) {
                const bool was = agent->unreachable;
                agent->unreachable = found->second.path.empty();
                if (agent->unreachable && !was)
                    _events.push_back({entity.id(), NavigationEventKind2D::Unreachable});
            }
        });
    struct Actor {
        EcsEntity entity;
        Vec2f position;
        NavigationAgent2D agent;
        Vec2f preferred{};
        bool valid = true;
    };
    std::vector<Actor> actors;
    _world.query<NavigationAgent2D, Transform2D>().each_entity(
        [&](EcsEntity entity, const NavigationAgent2D& agent, const Transform2D&) {
            actors.push_back({entity, transform(entity).pos, agent});
        });
    if (actors.size() > _options.max_agents)
        throw std::runtime_error("navigation agent limit exceeded");
    std::ranges::sort(actors, {}, [](const Actor& a) { return a.entity.id(); });
    std::set<EcsId> seen;
    for (auto& actor : actors) {
        const auto id = actor.entity.id();
        seen.insert(id);
        auto& a = actor.agent;
        auto [where, fresh] = _entries.try_emplace(id);
        auto& e = where->second;
        const auto nonnegative = [](f32 value) { return std::isfinite(value) && value >= 0; };
        const bool valid = actor.valid =
            std::isfinite(actor.position.x) && std::isfinite(actor.position.y) && std::isfinite(a.target.x) &&
            std::isfinite(a.target.y) && nonnegative(a.speed) && nonnegative(a.radius) &&
            a.radius <= _snapshot->tile_size * 16 && nonnegative(a.arrival_distance) &&
            nonnegative(a.waypoint_distance) && nonnegative(a.neighbour_distance) && nonnegative(a.avoidance_weight) &&
            nonnegative(a.repath_interval) && nonnegative(a.stuck_seconds) &&
            std::isfinite(length(sub(a.target, actor.position))) && std::isfinite(a.speed * dt);
        const auto radius = valid ? static_cast<i32>(std::ceil(a.radius * 8 / _snapshot->tile_size)) : 0;
        const bool changed = fresh || e.lifetime != lifetime(actor.entity) || e.radius != radius;
        if (changed) {
            if (e.request && _servers.contains(e.radius))
                _servers.at(e.radius).paths->cancel(e.request);
            e = {.lifetime = lifetime(actor.entity),
                 .radius = radius,
                 .target = a.target,
                 .last_position = actor.position};
            a.arrived = a.stuck = a.unreachable = false;
        }
        if (valid && e.target != a.target) {
            e.target = a.target;
            a.arrived = a.unreachable = false;
            // Keep following the last valid path while a moving goal replans.
        }
        e.retry = std::max(0.0f, e.retry - dt);
        a.velocity = {};
        if (!valid || !a.enabled) {
            if (!valid) {
                if (e.request && _servers.contains(e.radius))
                    _servers.at(e.radius).paths->cancel(e.request);
                e.request = 0;
                e.path.clear();
            }
            if (!valid && !a.unreachable)
                _events.push_back({id, NavigationEventKind2D::Unreachable});
            a.unreachable = !valid;
            continue;
        }
        const auto distance = length(sub(a.target, actor.position));
        if (distance <= a.arrival_distance) {
            if (!a.arrived)
                _events.push_back({id, NavigationEventKind2D::Arrived});
            a.arrived = true;
            a.stuck = a.unreachable = false;
            if (e.request)
                server(radius).paths->cancel(e.request);
            e.request = 0;
            e.path.clear();
            continue;
        }
        a.arrived = false;
        if (!free(a.target, a.radius)) {
            if (!a.unreachable)
                _events.push_back({id, NavigationEventKind2D::Unreachable});
            a.unreachable = true;
            continue;
        }
        if (!e.request && e.retry == 0 && (e.path.empty() || a.stuck || e.target != e.request_target)) {
            e.request_target = a.target;
            e.request =
                server(radius).paths->submit({.start_world = actor.position, .goal_world = a.target, .user_key = id});
            e.retry = std::max(0.01f, a.repath_interval);
            _events.push_back({id, NavigationEventKind2D::Replanned});
        }
        while (e.next < e.path.size()) {
            const auto threshold =
                e.next + 1 == e.path.size() ? std::min(a.waypoint_distance, a.arrival_distance) : a.waypoint_distance;
            if (length(sub(e.path[e.next], actor.position)) > threshold)
                break;
            ++e.next;
        }
        if (e.next < e.path.size()) {
            const auto delta = sub(e.path[e.next], actor.position);
            const auto d = length(delta);
            actor.preferred = mul(delta, std::min(a.speed, dt > 0 ? d / dt : a.speed) / std::max(0.0001f, d));
        }
        if (length(sub(actor.position, e.last_position)) < std::max(0.001f, a.speed * dt * 0.02f) &&
            length(actor.preferred) > 0)
            e.stalled += dt;
        else
            e.stalled = 0;
        e.last_position = actor.position;
        const bool stuck = e.stalled >= std::max(0.01f, a.stuck_seconds);
        if (stuck && !a.stuck)
            _events.push_back({id, NavigationEventKind2D::Stuck});
        a.stuck = stuck;
    }
    for (auto& actor : actors) {
        auto& a = actor.agent;
        Vec2f velocity = actor.preferred;
        if (a.enabled && a.avoidance && length(velocity) > 0)
            for (const auto& other : actors) {
                if (other.entity.id() == actor.entity.id() || !other.agent.enabled || !other.valid)
                    continue;
                auto delta = sub(actor.position, other.position);
                auto d = length(delta);
                if (d > std::max(a.neighbour_distance, a.radius + other.agent.radius))
                    continue;
                const f32 safe = a.radius + other.agent.radius;
                const auto predicted = sub(add(actor.position, mul(actor.preferred, 0.5f)),
                                           add(other.position, mul(other.preferred, 0.5f)));
                if (d < safe * 1.2f) {
                    if (d < 0.0001f) {
                        delta = actor.entity.id() < other.entity.id() ? Vec2f{1, 0} : Vec2f{-1, 0};
                        d = 1;
                    }
                    velocity = add(velocity, mul(delta, a.speed * std::max(0.0f, a.avoidance_weight) *
                                                            (safe * 1.2f - d) / std::max(0.001f, safe * 1.2f) / d));
                } else if (length(predicted) < safe * 1.2f) {
                    const auto toward = sub(other.position, actor.position);
                    velocity = add(velocity, mul(Vec2f{-toward.y, toward.x},
                                                 a.speed * std::max(0.0f, a.avoidance_weight) / std::max(0.001f, d)));
                }
            }
        if (const auto speed = length(velocity); speed > a.speed && speed > 0)
            velocity = mul(velocity, a.speed / speed);
        if (!a.enabled || a.arrived || a.unreachable || !actor.valid)
            velocity = {};
        if (actor.valid && dt > 0 &&
            !clear(actor.position, add(actor.position, mul(velocity, dt)), std::max(0.0f, a.radius))) {
            Vec2f x{velocity.x, 0}, y{0, velocity.y};
            velocity = clear(actor.position, add(actor.position, mul(x, dt)), std::max(0.0f, a.radius))   ? x
                       : clear(actor.position, add(actor.position, mul(y, dt)), std::max(0.0f, a.radius)) ? y
                                                                                                          : Vec2f{};
        }
        a.velocity = velocity;
        if (a.apply_motion && actor.valid) {
            bool physics = false;
#if KIN_ENABLE_PHYSICS
            if (auto* character = actor.entity.get_mut<CharacterController2D>()) {
                character->velocity = velocity;
                physics = true;
            }
#endif
            if (!physics && dt > 0) {
                auto pose = transform(actor.entity);
                pose.pos = add(pose.pos, mul(velocity, dt));
                auto local =
                    to_local(actor.entity.parent() ? transform(actor.entity.parent()) : WorldTransform{}, pose);
                actor.entity.set(local);
            }
        }
        actor.entity.set(a);
    }
    std::erase_if(_entries, [&](const auto& item) {
        if (seen.contains(item.first))
            return false;
        if (item.second.request && _servers.contains(item.second.radius))
            _servers.at(item.second.radius).paths->cancel(item.second.request);
        return true;
    });
    i32 budget = _options.requests_per_step;
    for (auto& [radius, server] : _servers)
        budget -= server.paths->admit(0, budget);
}
} // namespace kin
