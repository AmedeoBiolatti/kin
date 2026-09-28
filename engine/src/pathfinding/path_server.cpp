#include <kin/pathfinding/path_server.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace kin {

namespace {

f32 world_distance(Vec2f a, Vec2f b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

// ---- NavGridSnapshot ------------------------------------------------------
// The float math below is a verbatim port of the PathService snapshot
// helpers of the game it was extracted from (same expressions, same operation order) so a game
// migrating onto the server reproduces its previous paths bit-for-bit.

Vec2i NavGridSnapshot::world_to_tile(Vec2f pos) const {
    return {
        std::clamp(static_cast<i32>(std::floor(pos.x / tile_size)), 0, cols - 1),
        std::clamp(static_cast<i32>(std::floor(pos.y / tile_size)), 0, rows - 1),
    };
}

Vec2f NavGridSnapshot::tile_center(Vec2i tile) const {
    return {
        (static_cast<f32>(tile.x) + 0.5f) * tile_size,
        (static_cast<f32>(tile.y) + 0.5f) * tile_size,
    };
}

Vec2i NavGridSnapshot::nearest_open_tile(Vec2i tile) const {
    if (in_bounds(tile) && blocked[static_cast<std::size_t>(tile_index(tile))] == 0) {
        return tile;
    }
    Vec2i best = tile;
    f32 best_d = 1e9f;
    for (i32 row = 0; row < rows; ++row) {
        for (i32 col = 0; col < cols; ++col) {
            const Vec2i current{col, row};
            if (blocked[static_cast<std::size_t>(tile_index(current))] != 0) {
                continue;
            }
            const f32 d = world_distance(tile_center(current), tile_center(tile));
            if (d < best_d) {
                best_d = d;
                best = current;
            }
        }
    }
    return best;
}

bool NavGridSnapshot::segment_clear(Vec2f a, Vec2f b, Vec2i ignore_a, Vec2i ignore_b) const {
    const f32 segment_len = world_distance(a, b);
    const i32 steps = std::max(1, static_cast<i32>(std::ceil(segment_len / (tile_size * 0.25f))));
    for (i32 i = 0; i <= steps; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
        const Vec2f p{
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
        };
        const Vec2i sample = world_to_tile(p);
        for (i32 row = sample.y - clearance_tiles; row <= sample.y + clearance_tiles; ++row) {
            for (i32 col = sample.x - clearance_tiles; col <= sample.x + clearance_tiles; ++col) {
                const Vec2i current{col, row};
                if (!in_bounds(current)) {
                    continue;
                }
                if (current == ignore_a || current == ignore_b) {
                    continue;
                }
                if (blocked[static_cast<std::size_t>(tile_index(current))] != 0) {
                    return false;
                }
            }
        }
    }
    return true;
}

// ---- PathServer -----------------------------------------------------------

PathServer::PathServer(PathServerConfig config)
    : _config(std::move(config)) {
    const i32 worker_count = std::max(1, _config.worker_count);
    _workers.reserve(static_cast<std::size_t>(worker_count));
    for (i32 i = 0; i < worker_count; ++i) {
        _workers.emplace_back([this](std::stop_token stop) { worker_loop(stop); });
    }
}

PathServer::~PathServer() {
    for (std::jthread& worker : _workers) {
        worker.request_stop();
    }
    _work_cv.notify_all();
    // std::jthread destructors join.
}

void PathServer::set_snapshot(std::shared_ptr<const NavGridSnapshot> snapshot) {
    std::scoped_lock lock{_mutex};
    const bool version_changed = snapshot == nullptr || _snapshot == nullptr ||
                                 snapshot->version != _snapshot->version;
    _snapshot = std::move(snapshot);
    if (version_changed) {
        _cache.clear();
    }
}

u64 PathServer::submit(const PathQuery& query, std::vector<Vec2f>* out_provisional) {
    std::scoped_lock lock{_mutex};
    Job job;
    job.request_id = _next_id++;
    job.query = query;
    job.snapshot = _snapshot;
    PathStatus initial = PathStatus::Queued;
    if (_config.enable_provisional && out_provisional != nullptr && _snapshot != nullptr) {
        *out_provisional = provisional_path(*_snapshot, query.start_world, query.goal_world);
        initial = PathStatus::Provisional;
        ++_stats.provisionals;
    }
    _tracked[job.request_id] = initial;
    _queued.push_back(std::move(job));
    ++_stats.submitted;
    return _queued.back().request_id;
}

PathServer::CacheKey PathServer::cache_key_for(const PathQuery& query) const {
    const f32 bucket = _config.cache_bucket_world;
    CacheKey key;
    key.nav_version = _snapshot != nullptr ? _snapshot->version : 0;
    key.domain = query.domain;
    key.cost_context = query.cost_context;
    key.start_bucket = {
        static_cast<i32>(std::floor(query.start_world.x / bucket)),
        static_cast<i32>(std::floor(query.start_world.y / bucket)),
    };
    key.goal_bucket = {
        static_cast<i32>(std::floor(query.goal_world.x / bucket)),
        static_cast<i32>(std::floor(query.goal_world.y / bucket)),
    };
    return key;
}

const std::vector<Vec2f>* PathServer::cached_locked(const PathQuery& query) const {
    if (_config.cache_bucket_world <= 0.0f || !query.use_cache) {
        return nullptr;
    }
    const auto it = _cache.find(cache_key_for(query));
    return it != _cache.end() ? &it->second : nullptr;
}

const std::vector<Vec2f>* PathServer::cached(const PathQuery& query) const {
    std::scoped_lock lock{_mutex};
    const std::vector<Vec2f>* hit = cached_locked(query);
    if (hit != nullptr) {
        ++const_cast<PathServerStats&>(_stats).cache_hits;
    }
    return hit;
}

std::optional<std::vector<Vec2f>> PathServer::try_join(const PathQuery& query) const {
    std::scoped_lock lock{_mutex};
    if (_config.max_join_connector_world <= 0.0f || !query.allow_join || _snapshot == nullptr) {
        return std::nullopt;
    }
    const std::vector<Vec2f>* hit = cached_locked(query);
    if (hit == nullptr || hit->empty()) {
        return std::nullopt;
    }
    const std::vector<Vec2f>& path = *hit;
    // Nearest waypoint to the query start; front-to-back scan with strict `<`
    // keeps the earliest index on ties (deterministic).
    std::size_t join_index = 0;
    f32 join_distance = world_distance(query.start_world, path[0]);
    for (std::size_t i = 1; i < path.size(); ++i) {
        const f32 d = world_distance(query.start_world, path[i]);
        if (d < join_distance) {
            join_distance = d;
            join_index = i;
        }
    }
    if (join_distance > _config.max_join_connector_world) {
        return std::nullopt;
    }
    if (!_snapshot->segment_clear(query.start_world, path[join_index])) {
        return std::nullopt;
    }
    std::vector<Vec2f> joined(path.begin() + static_cast<std::ptrdiff_t>(join_index), path.end());
    ++const_cast<PathServerStats&>(_stats).joins;
    return joined;
}

void PathServer::cancel(u64 request_id) {
    std::scoped_lock lock{_mutex};
    const auto it = _tracked.find(request_id);
    if (it == _tracked.end() || it->second == PathStatus::Cancelled) {
        return;
    }
    if (it->second == PathStatus::Queued || it->second == PathStatus::Provisional) {
        const auto queued_it = std::find_if(_queued.begin(), _queued.end(), [&](const Job& job) {
            return job.request_id == request_id;
        });
        if (queued_it != _queued.end()) {
            _queued.erase(queued_it);
        }
    }
    // Admitted requests keep computing; the result is dropped at drain.
    it->second = PathStatus::Cancelled;
    ++_stats.cancelled;
}

i32 PathServer::admit(u32 lane, i32 budget) {
    i32 admitted = 0;
    {
        std::scoped_lock lock{_mutex};
        auto it = _queued.begin();
        while (it != _queued.end() && admitted < budget) {
            if (it->query.lane != lane) {
                ++it;
                continue;
            }
            const auto tracked = _tracked.find(it->request_id);
            if (tracked != _tracked.end() && tracked->second != PathStatus::Cancelled) {
                tracked->second = PathStatus::Admitted;
            }
            _admitted.push_back(std::move(*it));
            it = _queued.erase(it);
            ++admitted;
        }
    }
    if (admitted > 0) {
        _work_cv.notify_all();
    }
    return admitted;
}

void PathServer::wait_admitted_complete(std::unique_lock<std::mutex>& lock) {
    _done_cv.wait(lock, [this]() { return _admitted.empty() && _active_workers == 0; });
}

std::vector<PathServerResult> PathServer::collect_completed_locked() {
    // Called with _mutex held: sorts staged jobs into ascending request-id
    // order, performs cache inserts and bookkeeping, and returns the results
    // for delivery. The sink runs after the lock is released.
    std::sort(_completed.begin(), _completed.end(), [](const Job& a, const Job& b) {
        return a.request_id < b.request_id;
    });
    std::vector<PathServerResult> results;
    results.reserve(_completed.size());
    _stats.last_drain_count = _completed.size();
    for (Job& job : _completed) {
        const auto tracked = _tracked.find(job.request_id);
        const bool cancelled = tracked != _tracked.end() && tracked->second == PathStatus::Cancelled;
        if (tracked != _tracked.end()) {
            _tracked.erase(tracked);
        }
        if (cancelled) {
            continue;
        }
        if (_config.cache_bucket_world > 0.0f && job.query.use_cache && job.snapshot != nullptr) {
            CacheKey key;
            key.nav_version = job.snapshot->version;
            key.domain = job.query.domain;
            key.cost_context = job.query.cost_context;
            key.start_bucket = {
                static_cast<i32>(std::floor(job.query.start_world.x / _config.cache_bucket_world)),
                static_cast<i32>(std::floor(job.query.start_world.y / _config.cache_bucket_world)),
            };
            key.goal_bucket = {
                static_cast<i32>(std::floor(job.query.goal_world.x / _config.cache_bucket_world)),
                static_cast<i32>(std::floor(job.query.goal_world.y / _config.cache_bucket_world)),
            };
            _cache[key] = job.path;
        }
        PathServerResult result;
        result.request_id = job.request_id;
        result.user_key = job.query.user_key;
        result.lane = job.query.lane;
        result.domain = job.query.domain;
        result.nav_version = job.snapshot != nullptr ? job.snapshot->version : 0;
        result.source = PathResultSource::Solved;
        result.path = std::move(job.path);
        ++_stats.solved;
        results.push_back(std::move(result));
    }
    _completed.clear();
    // Flush requests cancelled while still queued: their job was erased at
    // cancel time, so no staged result will ever retire the tracked entry.
    // The barrier guarantees nothing cancelled is still in flight here.
    std::erase_if(_tracked, [](const auto& entry) { return entry.second == PathStatus::Cancelled; });
    return results;
}

void PathServer::drain(DrainMode mode, const ResultSink& sink) {
    const auto wait_start = std::chrono::steady_clock::now();
    std::vector<PathServerResult> results;
    {
        std::unique_lock lock{_mutex};
        if (mode == DrainMode::DrainToQuiescent) {
            while (!_queued.empty()) {
                const auto tracked = _tracked.find(_queued.front().request_id);
                if (tracked != _tracked.end() && tracked->second != PathStatus::Cancelled) {
                    tracked->second = PathStatus::Admitted;
                }
                _admitted.push_back(std::move(_queued.front()));
                _queued.pop_front();
            }
            _work_cv.notify_all();
        }
        wait_admitted_complete(lock);
        _stats.last_barrier_wait_ms =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count() / 1'000'000.0;
        results = collect_completed_locked();
    }
    for (PathServerResult& result : results) {
        sink(std::move(result));
    }
}

PathStatus PathServer::status(u64 request_id) const {
    std::scoped_lock lock{_mutex};
    const auto it = _tracked.find(request_id);
    return it != _tracked.end() ? it->second : PathStatus::Unknown;
}

PathServerStats PathServer::stats() const {
    std::scoped_lock lock{_mutex};
    PathServerStats out = _stats;
    out.queued = static_cast<i64>(_queued.size());
    out.admitted_in_flight = static_cast<i64>(_admitted.size()) + _active_workers;
    out.cache_entries = _cache.size();
    return out;
}

void PathServer::clear_cache() {
    std::scoped_lock lock{_mutex};
    _cache.clear();
}

bool PathServer::path_blocked(const NavGridSnapshot& nav, std::span<const Vec2f> remaining, Vec2f current_pos) {
    // Tile-exact sampling (no clearance halo): invalidation should fire only
    // when the path actually crosses a blocked tile, not when it merely runs
    // near one — units routinely hug obstacles.
    Vec2f anchor = current_pos;
    for (const Vec2f& waypoint : remaining) {
        const f32 segment_len = world_distance(anchor, waypoint);
        const i32 steps = std::max(1, static_cast<i32>(std::ceil(segment_len / (nav.tile_size * 0.25f))));
        for (i32 i = 0; i <= steps; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
            const Vec2f p{
                anchor.x + (waypoint.x - anchor.x) * t,
                anchor.y + (waypoint.y - anchor.y) * t,
            };
            if (nav.is_blocked(nav.world_to_tile(p))) {
                return true;
            }
        }
        anchor = waypoint;
    }
    return false;
}

std::vector<Vec2f> PathServer::provisional_path(const NavGridSnapshot& nav, Vec2f start, Vec2f goal, i32 max_tiles) {
    if (nav.cols <= 0 || nav.rows <= 0) {
        return {};
    }
    const Vec2i start_tile = nav.world_to_tile(start);
    const Vec2i goal_tile = nav.world_to_tile(goal);
    if (nav.segment_clear(start, goal, start_tile, goal_tile)) {
        return world_distance(start, goal) > 2.0f ? std::vector<Vec2f>{goal} : std::vector<Vec2f>{};
    }
    std::vector<Vec2f> chain;
    Vec2f cursor = start;
    for (i32 step = 0; step < std::max(1, max_tiles); ++step) {
        const f32 dx = goal.x - cursor.x;
        const f32 dy = goal.y - cursor.y;
        const f32 len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.001f) {
            break;
        }
        const Vec2f next{
            std::clamp(cursor.x + (dx / len) * nav.tile_size, 0.0f, static_cast<f32>(nav.cols) * nav.tile_size),
            std::clamp(cursor.y + (dy / len) * nav.tile_size, 0.0f, static_cast<f32>(nav.rows) * nav.tile_size),
        };
        chain.push_back(next);
        cursor = next;
    }
    return chain;
}

std::vector<Vec2f> PathServer::solve(const Job& job) const {
    // post_process, when set, turns the tile path into waypoints in place of
    // the default tile centres (and must be pure — it runs on a worker).
    if (job.snapshot == nullptr) {
        return {};
    }
    const NavGridSnapshot& nav = *job.snapshot;
    if (nav.cols <= 0 || nav.rows <= 0 || nav.blocked.empty()) {
        return {};
    }
    const Vec2f start_world = job.query.start_world;
    const Vec2f goal_world = job.query.goal_world;
    const Vec2i start = nav.world_to_tile(start_world);
    const Vec2i raw_goal = nav.world_to_tile(goal_world);
    const Vec2i goal = nav.nearest_open_tile(raw_goal);
    // nearest_open_tile() returns its input when the grid contains no open
    // tile. Do not manufacture a route by treating a blocked goal as an
    // exception in the A* callback below.
    if (!nav.in_bounds(goal) || nav.is_blocked(goal)) {
        return {};
    }
    const Vec2f final_goal = (goal == raw_goal) ? goal_world : nav.tile_center(goal);
    const bool weighted = _config.tile_cost && job.query.cost_context != 0;
    if (!weighted && nav.segment_clear(start_world, final_goal, start, goal)) {
        return world_distance(start_world, final_goal) > 2.0f ? std::vector<Vec2f>{final_goal} : std::vector<Vec2f>{};
    }
    GridNav grid{};
    grid.cols = nav.cols;
    grid.rows = nav.rows;
    grid.blocked = [&nav, start, goal](i32 col, i32 row) {
        if ((col == start.x && row == start.y) || (col == goal.x && row == goal.y)) {
            return false;
        }
        return nav.blocked[static_cast<std::size_t>(nav.tile_index({col, row}))] != 0;
    };
    if (_config.tile_cost && job.query.cost_context != 0) {
        // Pure function of (snapshot, tile, query) — worker-safe and
        // deterministic per the PathTileCost contract.
        grid.cost = [&nav, &job, this](i32 col, i32 row) {
            return _config.tile_cost(nav, Vec2i{col, row}, job.query);
        };
    }
    const std::vector<Vec2i> tiles = find_path(grid, start, goal, _config.grid_options);
    if (tiles.empty()) {
        return {};
    }
    std::vector<Vec2f> path;
    if (_config.post_process) {
        path = _config.post_process(nav, start_world, final_goal, tiles, start, goal);
    } else {
        if (tiles.size() == 1 && start == goal) {
            return world_distance(start_world, final_goal) > 2.0f
                       ? std::vector<Vec2f>{final_goal}
                       : std::vector<Vec2f>{};
        }
        path.reserve(tiles.size());
        for (std::size_t i = 1; i < tiles.size(); ++i) {
            path.push_back(i + 1 == tiles.size() ? final_goal : nav.tile_center(tiles[i]));
        }
    }
    return path;
}

void PathServer::worker_loop(std::stop_token stop) {
    std::unique_lock lock{_mutex};
    for (;;) {
        _work_cv.wait(lock, stop, [this]() { return !_admitted.empty(); });
        if (stop.stop_requested() && _admitted.empty()) {
            return;
        }
        if (_admitted.empty()) {
            continue;
        }
        Job job = std::move(_admitted.front());
        _admitted.pop_front();
        ++_active_workers;
        lock.unlock();
        job.path = solve(job);
        job.computed = true;
        lock.lock();
        _completed.push_back(std::move(job));
        --_active_workers;
        if (_admitted.empty() && _active_workers == 0) {
            _done_cv.notify_all();
        }
    }
}

} // namespace kin
