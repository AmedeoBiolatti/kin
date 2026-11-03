#pragma once
#include <kin/ecs/world.hpp>
#include <kin/pathfinding/path_server.hpp>
#include <map>

namespace kin {
struct NavigationAgent2D {
    Vec2f target{};
    f32 speed = 100;
    f32 radius = 8;
    f32 arrival_distance = 2;
    f32 waypoint_distance = 1;
    f32 neighbour_distance = 48;
    f32 avoidance_weight = 1;
    f32 repath_interval = 0.5f;
    f32 stuck_seconds = 1;
    bool enabled = true;
    bool avoidance = true;
    // Without a CharacterController2D, move Transform2D directly. False:
    // expose velocity for a game's own movement/physics system instead.
    bool apply_motion = true;
    Vec2f velocity{};
    bool arrived = false;
    bool stuck = false;
    bool unreachable = false;
};
enum class NavigationEventKind2D { Arrived, Stuck, Unreachable, Replanned };
struct NavigationEvent2D {
    EcsId entity = 0;
    NavigationEventKind2D kind;
};
struct NavigationOptions2D {
    JobSystem* jobs = nullptr;
    i32 requests_per_step = 8;
    std::size_t max_agents = 4096;
};
void register_navigation2d_components(EcsWorld& world);

// Deterministic square-grid agent service. Owns its PathServers so draining
// never consumes another system's requests. Agent radii use conservative 1/8
// cell buckets, with separate inflated snapshots (up to 16 cells of radius).
class Navigation2D {
public:
    Navigation2D(EcsWorld& world, std::shared_ptr<const NavGridSnapshot> snapshot, NavigationOptions2D options = {});
    ~Navigation2D();
    void set_snapshot(std::shared_ptr<const NavGridSnapshot> snapshot);
    void update(f32 dt);
    const std::vector<NavigationEvent2D>& events() const { return _events; }

private:
    struct Entry {
        u64 request = 0, lifetime = 0;
        i32 radius = -1;
        Vec2f target{}, request_target{}, last_position{};
        std::vector<Vec2f> path;
        std::size_t next = 0;
        f32 retry = 0, stalled = 0;
    };
    struct Server {
        std::shared_ptr<const NavGridSnapshot> grid;
        std::unique_ptr<PathServer> paths;
    };
    bool free(Vec2f position, f32 radius) const;
    bool clear(Vec2f from, Vec2f to, f32 radius) const;
    Server& server(i32 radius);
    EcsWorld& _world;
    NavigationOptions2D _options;
    std::shared_ptr<const NavGridSnapshot> _snapshot;
    std::map<i32, Server> _servers;
    std::map<EcsId, Entry> _entries;
    std::vector<NavigationEvent2D> _events;
};
} // namespace kin
