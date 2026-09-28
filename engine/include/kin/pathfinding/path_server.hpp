#pragma once

#include <kin/core/types.hpp>
#include <kin/pathfinding/pathfinding.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kin {

// PathServer: asynchronous, deterministic pathfinding service.
//
// Mirrors the AssetServer pattern: requests are submitted on the main thread
// and receive a serial, ascending request id; worker threads compute results
// as pure functions of (immutable nav snapshot, query); the main thread
// drains results in ascending request-id order behind a completion barrier.
// Because admission is a deterministic count budget and application order is
// the id order, worker completion *timing* is invisible to the simulation —
// run-to-run determinism and command-stream replay hold with any worker count.
//
// Determinism contract (callers must respect it):
//   - Every public method is main-thread-only.
//   - submit/cached/try_join/admit/drain must be called at fixed points in the
//     sim schedule (e.g. fixed ECS system orders).
//   - The post-process hook runs on a worker and must be a pure function of
//     its arguments: no live game state, wall clock, or RNG.

// Immutable navigation grid the workers read. Published via shared_ptr<const>
// so in-flight requests keep the snapshot they were submitted against even if
// a newer version is published.
struct NavGridSnapshot {
    i32 cols = 0;
    i32 rows = 0;
    u32 version = 0;
    // World units per tile.
    f32 tile_size = 1.0f;
    // Halo (in tiles) sampled around each segment point by segment_clear;
    // 0 = test only the sampled tile itself.
    i32 clearance_tiles = 0;
    // cols * rows entries, row-major; 0 = open, non-zero = blocked.
    std::vector<u8> blocked{};
    // Optional cols * rows entries, row-major: Chebyshev distance (in tiles,
    // clamped to 255) to the nearest blocked or out-of-bounds tile. Used by
    // cost hooks (e.g. formation pathfinding that prefers wide corridors).
    // Empty = not computed; clearance_at returns 0.
    std::vector<u8> clearance{};

    bool in_bounds(Vec2i tile) const {
        return tile.x >= 0 && tile.x < cols && tile.y >= 0 && tile.y < rows;
    }

    i32 tile_index(Vec2i tile) const { return tile.y * cols + tile.x; }

    bool is_blocked(Vec2i tile) const {
        return in_bounds(tile) && blocked[static_cast<std::size_t>(tile_index(tile))] != 0;
    }

    u8 clearance_at(Vec2i tile) const {
        if (!in_bounds(tile) || clearance.empty()) {
            return 0;
        }
        return clearance[static_cast<std::size_t>(tile_index(tile))];
    }

    Vec2i world_to_tile(Vec2f pos) const;
    Vec2f tile_center(Vec2i tile) const;

    // Nearest open tile by tile-center distance (full-grid scan; grids served
    // by this server are small). Returns `tile` unchanged when already open
    // or when the grid has no open tile.
    Vec2i nearest_open_tile(Vec2i tile) const;

    // True when the straight segment from a to b, sampled every
    // tile_size * 0.25 world units with a clearance_tiles halo, touches no
    // blocked tile. ignore_a/ignore_b (typically the segment's start and goal
    // tiles) are excluded from the blocked test; pass {-1, -1} to ignore none.
    bool segment_clear(Vec2f a, Vec2f b, Vec2i ignore_a = {-1, -1}, Vec2i ignore_b = {-1, -1}) const;
};

enum class PathStatus : u8 {
    Unknown,      // id not tracked: never submitted, or already delivered/flushed
    Queued,       // submitted, not yet admitted
    Provisional,  // submitted with a provisional chain handed out, not yet admitted
    Admitted,     // handed to workers; result delivered at the next drain
    Cancelled,    // cancelled; the entry is flushed at the next drain
};

enum class PathResultSource : u8 {
    Solved,
    CacheHit,
    Joined,
};

// Post-solve hook, run ON A WORKER THREAD after the raw A* tile path is found.
// Receives the snapshot, the request's world start, the (possibly goal-snapped)
// final goal in world space, the raw tile path (start..goal inclusive), and the
// start/goal tiles. Returns the world-space waypoint path. MUST be pure.
// When null, waypoints are the tile centers of tiles[1..] with the final point
// replaced by the final goal.
using PathPostProcess = std::function<std::vector<Vec2f>(
    const NavGridSnapshot& nav,
    Vec2f start_world,
    Vec2f final_goal_world,
    std::span<const Vec2i> tiles,
    Vec2i start_tile,
    Vec2i goal_tile)>;

// Per-tile cost hook, run ON A WORKER THREAD during the A* solve. MUST be a
// pure function of its arguments (no live game state). Return values are
// kin::find_path tile-cost multipliers: 10 = baseline; keep >= 10 so the
// A* heuristic stays admissible. Only consulted when set AND the query's
// cost_context is non-zero.
struct PathQuery;
using PathTileCost = std::function<i32(const NavGridSnapshot& nav, Vec2i tile, const PathQuery& query)>;

struct PathServerConfig {
    // Worker thread count; clamped to >= 1.
    i32 worker_count = 1;
    // Cache bucket edge in world units; 0 disables the result cache.
    f32 cache_bucket_world = 0.0f;
    // When true, submit() can hand out an immediate provisional waypoint chain.
    bool enable_provisional = false;
    // Maximum world-space distance from a query start to a cached path's join
    // waypoint for try_join; 0 disables joining.
    f32 max_join_connector_world = 0.0f;
    GridPathOptions grid_options{};
    PathPostProcess post_process{};
    // Optional per-tile cost (see PathTileCost); applied only to queries with
    // cost_context != 0.
    PathTileCost tile_cost{};
};

struct PathQuery {
    Vec2f start_world{};
    Vec2f goal_world{};
    // Admission lane: budgets are applied per lane (e.g. gameplay vs preview).
    u32 lane = 0;
    // Cache-key discriminator so unrelated request families never share
    // entries (e.g. a game's PathRequestKind).
    u32 domain = 0;
    // Whether the drained result is inserted into the cache and whether
    // cached()/try_join() may serve this query.
    bool use_cache = true;
    bool allow_join = true;
    // Opaque caller data echoed in the result (e.g. a unit id).
    u64 user_key = 0;
    // Opaque context for the config's tile_cost hook (e.g. a formation's
    // half-width in tiles). It is part of the result-cache key. 0 = uniform
    // cost (hook not consulted).
    u32 cost_context = 0;
};

struct PathServerResult {
    u64 request_id = 0;
    u64 user_key = 0;
    u32 lane = 0;
    u32 domain = 0;
    // Snapshot version the request was solved against.
    u32 nav_version = 0;
    PathResultSource source = PathResultSource::Solved;
    // World-space waypoints; empty means no (or zero-length) path.
    std::vector<Vec2f> path{};
};

struct PathServerStats {
    u64 submitted = 0;
    u64 solved = 0;
    u64 cache_hits = 0;
    u64 joins = 0;
    u64 provisionals = 0;
    u64 cancelled = 0;
    i64 queued = 0;
    i64 admitted_in_flight = 0;
    u64 last_drain_count = 0;
    // Wall-time the last drain spent blocked on the barrier. Diagnostic only —
    // never read this from simulation logic.
    f64 last_barrier_wait_ms = 0.0;
    std::size_t cache_entries = 0;
};

class PathServer {
public:
    enum class DrainMode {
        // Block until every ADMITTED request has a staged result, then deliver
        // them. Queued-but-unadmitted requests are untouched. The per-tick mode.
        BarrierOnAdmitted,
        // Admit everything still queued (all lanes), then block until the
        // server is empty. For warm-up / loading phases.
        DrainToQuiescent,
    };

    using ResultSink = std::function<void(PathServerResult&&)>;

    explicit PathServer(PathServerConfig config = {});
    ~PathServer();

    PathServer(const PathServer&) = delete;
    PathServer& operator=(const PathServer&) = delete;

    // Publishes the grid all FUTURE submissions are solved against. A snapshot
    // with a version different from the current one clears the result cache.
    void set_snapshot(std::shared_ptr<const NavGridSnapshot> snapshot);
    const NavGridSnapshot* snapshot() const { return _snapshot.get(); }

    // Queues a request and returns its serial ascending id (the determinism
    // backbone). When provisional paths are enabled and out_provisional is
    // non-null, it receives an immediate deterministic fallback chain and the
    // request's status starts at Provisional instead of Queued.
    u64 submit(const PathQuery& query, std::vector<Vec2f>* out_provisional = nullptr);

    // Cache lookup without submitting. Returns nullptr on miss (or when the
    // cache is disabled / query.use_cache is false). The pointer is valid only
    // until the next drain()/set_snapshot()/clear_cache().
    const std::vector<Vec2f>* cached(const PathQuery& query) const;

    // Join attempt without submitting: on a cache hit for the query's goal
    // bucket, picks the cached waypoint nearest to the query start (front-to-
    // back scan, index-order tie-break), validates distance <=
    // max_join_connector_world and segment_clear(start, waypoint), and returns
    // the cached path's tail from that waypoint. nullopt on any failure.
    std::optional<std::vector<Vec2f>> try_join(const PathQuery& query) const;

    // Supersedes a request. Queued requests are dropped immediately; admitted
    // requests still compute but their result is discarded at drain.
    void cancel(u64 request_id);

    // End-of-tick admission: moves up to `budget` queued requests of `lane`
    // (in ascending request-id order) to the workers. Returns the number
    // admitted. The budget — not wall time — bounds per-tick path work, which
    // keeps multi-tick latency deterministic.
    i32 admit(u32 lane, i32 budget);

    // Start-of-tick delivery: applies the barrier for `mode`, inserts cacheable
    // results into the cache, and invokes `sink` for each non-cancelled result
    // in ascending request-id order.
    void drain(DrainMode mode, const ResultSink& sink);

    PathStatus status(u64 request_id) const;
    PathServerStats stats() const;
    void clear_cache();

    // True when any remaining segment (current_pos -> remaining[0] -> ...)
    // crosses a blocked tile of `nav`. Pure; used for nav-change invalidation.
    static bool path_blocked(const NavGridSnapshot& nav, std::span<const Vec2f> remaining, Vec2f current_pos);

    // Deterministic immediate fallback chain: {goal} when the straight segment
    // is clear, otherwise up to max_tiles single-tile steps toward the goal.
    // Pure function of (nav, start, goal).
    static std::vector<Vec2f> provisional_path(const NavGridSnapshot& nav, Vec2f start, Vec2f goal, i32 max_tiles = 1);

private:
    struct CacheKey {
        u32 nav_version = 0;
        u32 domain = 0;
        u32 cost_context = 0;
        Vec2i start_bucket{};
        Vec2i goal_bucket{};

        bool operator==(const CacheKey& other) const {
            return nav_version == other.nav_version && domain == other.domain &&
                   cost_context == other.cost_context &&
                   start_bucket == other.start_bucket && goal_bucket == other.goal_bucket;
        }
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& key) const {
            std::size_t value = static_cast<std::size_t>(key.nav_version);
            value ^= static_cast<std::size_t>(static_cast<u32>(key.start_bucket.x)) * 73856093u;
            value ^= static_cast<std::size_t>(static_cast<u32>(key.start_bucket.y)) * 19349663u;
            value ^= static_cast<std::size_t>(static_cast<u32>(key.goal_bucket.x)) * 83492791u;
            value ^= static_cast<std::size_t>(static_cast<u32>(key.goal_bucket.y)) * 2654435761u;
            value ^= static_cast<std::size_t>(key.domain) << 24u;
            value ^= static_cast<std::size_t>(key.cost_context) * 2246822519u;
            return value;
        }
    };

    struct Job {
        u64 request_id = 0;
        PathQuery query{};
        std::shared_ptr<const NavGridSnapshot> snapshot{};
        std::vector<Vec2f> path{};
        bool computed = false;
    };

    CacheKey cache_key_for(const PathQuery& query) const;
    const std::vector<Vec2f>* cached_locked(const PathQuery& query) const;
    std::vector<Vec2f> solve(const Job& job) const;
    void worker_loop(std::stop_token stop);
    void wait_admitted_complete(std::unique_lock<std::mutex>& lock);
    std::vector<PathServerResult> collect_completed_locked();

    PathServerConfig _config;
    std::shared_ptr<const NavGridSnapshot> _snapshot{};

    mutable std::mutex _mutex;
    std::condition_variable_any _work_cv;
    std::condition_variable _done_cv;
    // Queued per submission order (ascending id); admission filters by lane.
    std::deque<Job> _queued;
    std::deque<Job> _admitted;
    std::vector<Job> _completed;
    std::unordered_map<u64, PathStatus> _tracked;
    std::unordered_map<CacheKey, std::vector<Vec2f>, CacheKeyHash> _cache;
    i32 _active_workers = 0;
    u64 _next_id = 1;
    PathServerStats _stats{};
    std::vector<std::jthread> _workers;
};

} // namespace kin
