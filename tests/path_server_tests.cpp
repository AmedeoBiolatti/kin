// kin::PathServer tests. Uses a hand-rolled REQUIRE instead of assert so the
// checks survive NDEBUG builds — the run-vs-run keystone must be able to fail
// in the release config the engine actually ships with.

#include <kin/pathfinding/path_server.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#define REQUIRE(cond)                                                                  \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::exit(1);                                                              \
        }                                                                              \
    } while (0)

namespace {

constexpr kin::f32 TileSize = 32.0f;

// 20x12 grid with a vertical wall at col 10 (gap at row 6) so paths from the
// left half to the right half need real A* while same-side paths take the
// direct-shortcut branch.
std::shared_ptr<const kin::NavGridSnapshot> make_snapshot(kin::u32 version) {
    auto nav = std::make_shared<kin::NavGridSnapshot>();
    nav->cols = 20;
    nav->rows = 12;
    nav->version = version;
    nav->tile_size = TileSize;
    nav->clearance_tiles = 0;
    nav->blocked.assign(static_cast<std::size_t>(nav->cols * nav->rows), 0);
    for (kin::i32 row = 0; row < nav->rows; ++row) {
        if (row == 6) {
            continue;
        }
        nav->blocked[static_cast<std::size_t>(row * nav->cols + 10)] = 1;
    }
    return nav;
}

kin::Vec2f tile_world(kin::i32 col, kin::i32 row) {
    return {(static_cast<kin::f32>(col) + 0.5f) * TileSize, (static_cast<kin::f32>(row) + 0.5f) * TileSize};
}

kin::u64 fnv1a(kin::u64 hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

kin::u64 hash_result(kin::u64 hash, const kin::PathServerResult& result) {
    hash = fnv1a(hash, &result.request_id, sizeof(result.request_id));
    hash = fnv1a(hash, &result.user_key, sizeof(result.user_key));
    hash = fnv1a(hash, &result.nav_version, sizeof(result.nav_version));
    for (const kin::Vec2f& p : result.path) {
        hash = fnv1a(hash, &p.x, sizeof(p.x));
        hash = fnv1a(hash, &p.y, sizeof(p.y));
    }
    return hash;
}

void test_drain_order_is_ascending_id_under_parallel_workers() {
    kin::PathServer server{{.worker_count = 4}};
    server.set_snapshot(make_snapshot(1));
    // Mixed-cost: cross-wall solves (full A*) interleaved with same-side
    // direct shortcuts, so workers finish out of submission order.
    for (int i = 0; i < 30; ++i) {
        kin::PathQuery query;
        query.start_world = tile_world(2, i % 12);
        query.goal_world = (i % 2 == 0) ? tile_world(18, 11 - i % 12) : tile_world(4, (i + 3) % 12);
        query.user_key = static_cast<kin::u64>(i);
        server.submit(query);
    }
    REQUIRE(server.admit(0, 30) == 30);
    std::vector<kin::u64> delivered;
    server.drain(kin::PathServer::DrainMode::BarrierOnAdmitted, [&](kin::PathServerResult&& result) {
        delivered.push_back(result.request_id);
        REQUIRE(!result.path.empty());
    });
    REQUIRE(delivered.size() == 30);
    for (std::size_t i = 0; i < delivered.size(); ++i) {
        REQUIRE(delivered[i] == i + 1);
    }
}

void test_barrier_delivers_exactly_the_admitted_set() {
    kin::PathServer server{{.worker_count = 2}};
    server.set_snapshot(make_snapshot(1));
    std::vector<kin::u64> ids;
    for (int i = 0; i < 5; ++i) {
        kin::PathQuery query;
        query.start_world = tile_world(2, 2);
        query.goal_world = tile_world(18, 9);
        ids.push_back(server.submit(query));
    }
    REQUIRE(server.admit(0, 3) == 3);
    std::vector<kin::u64> delivered;
    server.drain(kin::PathServer::DrainMode::BarrierOnAdmitted, [&](kin::PathServerResult&& result) {
        delivered.push_back(result.request_id);
    });
    REQUIRE(delivered == std::vector<kin::u64>({ids[0], ids[1], ids[2]}));
    REQUIRE(server.status(ids[0]) == kin::PathStatus::Unknown);
    REQUIRE(server.status(ids[3]) == kin::PathStatus::Queued);
    REQUIRE(server.status(ids[4]) == kin::PathStatus::Queued);
    // The remainder arrives at the next admit + drain.
    REQUIRE(server.admit(0, 8) == 2);
    delivered.clear();
    server.drain(kin::PathServer::DrainMode::BarrierOnAdmitted, [&](kin::PathServerResult&& result) {
        delivered.push_back(result.request_id);
    });
    REQUIRE(delivered == std::vector<kin::u64>({ids[3], ids[4]}));
}

void test_drain_to_quiescent_admits_all_lanes() {
    kin::PathServer server{{.worker_count = 2}};
    server.set_snapshot(make_snapshot(1));
    for (int i = 0; i < 4; ++i) {
        kin::PathQuery query;
        query.start_world = tile_world(2, 2);
        query.goal_world = tile_world(18, 9);
        query.lane = static_cast<kin::u32>(i % 2);
        server.submit(query);
    }
    std::size_t delivered = 0;
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [&](kin::PathServerResult&&) { ++delivered; });
    REQUIRE(delivered == 4);
    REQUIRE(server.stats().queued == 0);
    REQUIRE(server.stats().admitted_in_flight == 0);
}

void test_provisional_then_solved() {
    kin::PathServer server{{.worker_count = 1, .enable_provisional = true}};
    server.set_snapshot(make_snapshot(1));
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(18, 9); // cross-wall: direct segment blocked
    std::vector<kin::Vec2f> provisional;
    const kin::u64 id = server.submit(query, &provisional);
    REQUIRE(!provisional.empty());
    REQUIRE(server.status(id) == kin::PathStatus::Provisional);
    REQUIRE(server.admit(0, 1) == 1);
    REQUIRE(server.status(id) == kin::PathStatus::Admitted);
    bool got = false;
    server.drain(kin::PathServer::DrainMode::BarrierOnAdmitted, [&](kin::PathServerResult&& result) {
        got = true;
        REQUIRE(result.request_id == id);
        REQUIRE(result.path.size() > 1); // real solve routes through the gap
    });
    REQUIRE(got);
    REQUIRE(server.status(id) == kin::PathStatus::Unknown);
}

void test_cache_hit_and_version_eviction() {
    kin::PathServer server{{.worker_count = 1, .cache_bucket_world = TileSize * 4.0f}};
    server.set_snapshot(make_snapshot(1));
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(18, 9);
    REQUIRE(server.cached(query) == nullptr);
    server.submit(query);
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [](kin::PathServerResult&&) {});
    const std::vector<kin::Vec2f>* hit = server.cached(query);
    REQUIRE(hit != nullptr);
    REQUIRE(!hit->empty());
    // Same buckets, slightly different start: still a hit.
    kin::PathQuery near = query;
    near.start_world.x += TileSize;
    REQUIRE(server.cached(near) != nullptr);
    // Different domain: miss.
    kin::PathQuery other_domain = query;
    other_domain.domain = 7;
    REQUIRE(server.cached(other_domain) == nullptr);
    // use_cache=false: miss.
    kin::PathQuery uncached = query;
    uncached.use_cache = false;
    REQUIRE(server.cached(uncached) == nullptr);
    // Nav version bump clears the cache.
    server.set_snapshot(make_snapshot(2));
    REQUIRE(server.cached(query) == nullptr);
}

void test_join_accept_and_reject() {
    kin::PathServer server{{
        .worker_count = 1,
        .cache_bucket_world = TileSize * 4.0f,
        .max_join_connector_world = TileSize * 4.0f,
    }};
    server.set_snapshot(make_snapshot(1));
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(18, 9);
    server.submit(query);
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [](kin::PathServerResult&&) {});
    const std::vector<kin::Vec2f>* cached = server.cached(query);
    REQUIRE(cached != nullptr && cached->size() > 1);
    // Same start bucket, one tile over: join accepted, tail of the cached path.
    kin::PathQuery joiner = query;
    joiner.start_world = tile_world(3, 2);
    const auto joined = server.try_join(joiner);
    REQUIRE(joined.has_value());
    REQUIRE(!joined->empty());
    REQUIRE(joined->back() == cached->back());
    REQUIRE(joined->size() <= cached->size());
    // allow_join=false: rejected.
    kin::PathQuery no_join = joiner;
    no_join.allow_join = false;
    REQUIRE(!server.try_join(no_join).has_value());
    // Start far outside the cached bucket: cache key misses, rejected.
    kin::PathQuery far = query;
    far.start_world = tile_world(2, 11);
    REQUIRE(!server.try_join(far).has_value());
}

void test_join_rejects_blocked_connector() {
    // Cache a path, then republish the SAME version with a wall between the
    // joiner and the path so the cache survives but segment_clear fails.
    auto nav = std::make_shared<kin::NavGridSnapshot>(*make_snapshot(1));
    kin::PathServer server{{
        .worker_count = 1,
        .cache_bucket_world = TileSize * 4.0f,
        .max_join_connector_world = TileSize * 4.0f,
    }};
    server.set_snapshot(nav);
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(18, 9);
    server.submit(query);
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [](kin::PathServerResult&&) {});
    REQUIRE(server.cached(query) != nullptr);
    auto walled = std::make_shared<kin::NavGridSnapshot>(*nav);
    for (kin::i32 row = 0; row < walled->rows; ++row) {
        walled->blocked[static_cast<std::size_t>(row * walled->cols + 3)] = 1;
    }
    server.set_snapshot(walled); // same version: cache kept
    REQUIRE(server.cached(query) != nullptr);
    kin::PathQuery joiner = query;
    joiner.start_world = tile_world(4, 2); // wall at col 3 sits between joiner and the cached start
    REQUIRE(!server.try_join(joiner).has_value());
}

void test_path_blocked() {
    const auto nav = make_snapshot(1);
    const kin::Vec2f pos = tile_world(2, 2);
    // Straight through the wall at col 10, row != 6: blocked.
    const std::vector<kin::Vec2f> through_wall{tile_world(18, 2)};
    REQUIRE(kin::PathServer::path_blocked(*nav, through_wall, pos));
    // Through the gap at row 6: clear.
    const std::vector<kin::Vec2f> via_gap{tile_world(9, 6), tile_world(11, 6), tile_world(18, 9)};
    REQUIRE(!kin::PathServer::path_blocked(*nav, via_gap, tile_world(2, 6)));
    // Empty remaining path: never blocked.
    REQUIRE(!kin::PathServer::path_blocked(*nav, {}, pos));
}

void test_unreachable_goal_returns_no_path() {
    kin::PathServer server{{.worker_count = 2}};
    auto nav = std::make_shared<kin::NavGridSnapshot>();
    nav->cols = 5;
    nav->rows = 3;
    nav->version = 1;
    nav->tile_size = TileSize;
    nav->blocked.assign(static_cast<std::size_t>(nav->cols * nav->rows), 0);
    for (kin::i32 row = 0; row < nav->rows; ++row) {
        nav->blocked[static_cast<std::size_t>(row * nav->cols + 2)] = 1;
    }
    server.set_snapshot(nav);

    kin::PathQuery query;
    query.start_world = tile_world(0, 1);
    query.goal_world = tile_world(4, 1);
    server.submit(query);
    bool delivered = false;
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [&](kin::PathServerResult&& result) {
        delivered = true;
        REQUIRE(result.path.empty());
    });
    REQUIRE(delivered);
}

void test_cache_separates_cost_contexts() {
    kin::PathServer server{{.worker_count = 1, .cache_bucket_world = TileSize * 4.0f}};
    server.set_snapshot(make_snapshot(1));
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(4, 2);
    query.cost_context = 1;
    server.submit(query);
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [](kin::PathServerResult&&) {});
    REQUIRE(server.cached(query) != nullptr);
    query.cost_context = 2;
    REQUIRE(server.cached(query) == nullptr);
}

void test_weighted_same_tile_returns_goal() {
    kin::PathServer server{{.worker_count = 1,
                            .tile_cost = [](const kin::NavGridSnapshot&, kin::Vec2i, const kin::PathQuery&) {
                                return 1.0f;
                            }}};
    auto nav = std::make_shared<kin::NavGridSnapshot>();
    nav->cols = 2;
    nav->rows = 2;
    nav->tile_size = TileSize;
    nav->version = 1;
    nav->blocked.assign(4, 0);
    server.set_snapshot(nav);

    kin::PathQuery query;
    query.start_world = {4.0f, 4.0f};
    query.goal_world = {28.0f, 28.0f};
    query.cost_context = 1;
    server.submit(query);
    bool delivered = false;
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [&](kin::PathServerResult&& result) {
        delivered = true;
        REQUIRE(result.path.size() == 1);
        REQUIRE(result.path.front() == query.goal_world);
    });
    REQUIRE(delivered);
}

void test_provisional_path_helper() {
    const auto nav = make_snapshot(1);
    // Clear straight line: single goal waypoint.
    const auto direct = kin::PathServer::provisional_path(*nav, tile_world(2, 2), tile_world(8, 2));
    REQUIRE(direct.size() == 1);
    REQUIRE(direct[0] == tile_world(8, 2));
    // Blocked straight line: short deterministic step chain toward the goal.
    const auto chain = kin::PathServer::provisional_path(*nav, tile_world(2, 2), tile_world(18, 2), 2);
    REQUIRE(chain.size() == 2);
    REQUIRE(chain[0].x > tile_world(2, 2).x);
}

void test_cancel_semantics() {
    kin::PathServer server{{.worker_count = 1}};
    server.set_snapshot(make_snapshot(1));
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(18, 9);
    const kin::u64 queued_id = server.submit(query);
    const kin::u64 kept_id = server.submit(query);
    const kin::u64 admitted_id = server.submit(query);
    // Cancel while queued: dropped before admission.
    server.cancel(queued_id);
    REQUIRE(server.status(queued_id) == kin::PathStatus::Cancelled);
    REQUIRE(server.admit(0, 3) == 2); // queued_id no longer admissible
    // Cancel after admission: still computes, dropped at drain.
    server.cancel(admitted_id);
    std::vector<kin::u64> delivered;
    server.drain(kin::PathServer::DrainMode::BarrierOnAdmitted, [&](kin::PathServerResult&& result) {
        delivered.push_back(result.request_id);
    });
    REQUIRE(delivered == std::vector<kin::u64>({kept_id}));
    REQUIRE(server.status(queued_id) == kin::PathStatus::Unknown);   // flushed at drain
    REQUIRE(server.status(admitted_id) == kin::PathStatus::Unknown); // dropped at drain
}

// Keystone: a scripted 200-request sequence with interleaved admit budgets and
// a mid-sequence snapshot bump must drain byte-identically for any worker
// count. This is the property the whole design exists to provide.
kin::u64 run_scripted_sequence(kin::i32 worker_count) {
    kin::PathServer server{{
        .worker_count = worker_count,
        .cache_bucket_world = TileSize * 4.0f,
        .enable_provisional = true,
        .max_join_connector_world = TileSize * 4.0f,
    }};
    server.set_snapshot(make_snapshot(1));
    kin::u64 hash = 1469598103934665603ull;
    kin::u64 rng = 0x9e3779b97f4a7c15ull;
    const auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    int submitted = 0;
    int tick = 0;
    while (submitted < 200 || server.stats().queued > 0 || server.stats().admitted_in_flight > 0) {
        ++tick;
        if (tick == 12) {
            server.set_snapshot(make_snapshot(2)); // mid-sequence nav change
        }
        const int batch = submitted < 200 ? static_cast<int>(next() % 9) : 0;
        for (int i = 0; i < batch && submitted < 200; ++i, ++submitted) {
            kin::PathQuery query;
            query.start_world = tile_world(static_cast<kin::i32>(next() % 9), static_cast<kin::i32>(next() % 12));
            query.goal_world = tile_world(static_cast<kin::i32>(12 + next() % 8), static_cast<kin::i32>(next() % 12));
            query.lane = static_cast<kin::u32>(next() % 2);
            query.domain = static_cast<kin::u32>(next() % 3);
            query.use_cache = (next() % 4) != 0;
            query.user_key = static_cast<kin::u64>(submitted);
            std::vector<kin::Vec2f> provisional;
            const kin::u64 id = server.submit(query, &provisional);
            hash = fnv1a(hash, &id, sizeof(id));
            for (const kin::Vec2f& p : provisional) {
                hash = fnv1a(hash, &p.x, sizeof(p.x));
                hash = fnv1a(hash, &p.y, sizeof(p.y));
            }
            // cached()/try_join() outcomes are sim decisions: hash them too.
            const bool cache_hit = server.cached(query) != nullptr;
            hash = fnv1a(hash, &cache_hit, sizeof(cache_hit));
            const auto joined = server.try_join(query);
            const bool join_hit = joined.has_value();
            hash = fnv1a(hash, &join_hit, sizeof(join_hit));
            // Sparse deterministic cancels.
            if (next() % 17 == 0) {
                server.cancel(id);
            }
        }
        server.admit(0, static_cast<kin::i32>(next() % 7));
        server.admit(1, static_cast<kin::i32>(next() % 3));
        server.drain(kin::PathServer::DrainMode::BarrierOnAdmitted, [&](kin::PathServerResult&& result) {
            hash = hash_result(hash, result);
        });
    }
    return hash;
}

void test_tile_cost_hook_prefers_wide_route() {
    // Map: a wall at col 10 with a narrow 1-tile gap at row 2 (short route)
    // and a wide 6-tile opening at rows 6-11 (detour). With the clearance
    // cost hook and a non-zero cost_context, the solve must take the wide
    // opening; with cost_context = 0 the hook is ignored and the narrow gap
    // (shorter) wins.
    auto nav = std::make_shared<kin::NavGridSnapshot>();
    nav->cols = 20;
    nav->rows = 12;
    nav->version = 1;
    nav->tile_size = TileSize;
    // Halo of 1 makes the straight segment through the 1-tile gap fail the
    // direct-shortcut LOS check, forcing the A* solve this test exercises.
    nav->clearance_tiles = 1;
    nav->blocked.assign(static_cast<std::size_t>(nav->cols * nav->rows), 0);
    for (kin::i32 row = 0; row < nav->rows; ++row) {
        if (row == 2 || row >= 6) {
            continue; // gap at row 2; open rows 6..11
        }
        nav->blocked[static_cast<std::size_t>(row * nav->cols + 10)] = 1;
    }
    // Minimal clearance field: distance to the wall column only (enough for
    // the hook to discriminate narrow vs wide passages).
    nav->clearance.assign(nav->blocked.size(), 0);
    for (kin::i32 row = 0; row < nav->rows; ++row) {
        for (kin::i32 col = 0; col < nav->cols; ++col) {
            const std::size_t i = static_cast<std::size_t>(row * nav->cols + col);
            if (nav->blocked[i] != 0) {
                continue;
            }
            // crude per-tile clearance: min distance to any blocked tile in
            // the same column band / row band (sufficient for this map).
            kin::i32 best = 255;
            for (kin::i32 r2 = 0; r2 < nav->rows; ++r2) {
                for (kin::i32 c2 = 0; c2 < nav->cols; ++c2) {
                    if (nav->blocked[static_cast<std::size_t>(r2 * nav->cols + c2)] != 0) {
                        best = std::min(best, std::max(std::abs(r2 - row), std::abs(c2 - col)));
                    }
                }
            }
            nav->clearance[i] = static_cast<kin::u8>(std::min(best, 255));
        }
    }
    kin::PathServerConfig config;
    config.worker_count = 1;
    config.tile_cost = [](const kin::NavGridSnapshot& snapshot, kin::Vec2i tile, const kin::PathQuery& query) {
        const kin::i32 needed = static_cast<kin::i32>(query.cost_context);
        const kin::i32 clearance = static_cast<kin::i32>(snapshot.clearance_at(tile));
        return clearance >= needed ? 10 : 10 + 25 * (needed - clearance);
    };
    kin::PathServer server{config};
    server.set_snapshot(nav);
    kin::PathQuery query;
    query.start_world = tile_world(2, 2);
    query.goal_world = tile_world(18, 2);
    query.use_cache = false;
    query.cost_context = 3; // wants 3 tiles of clearance -> narrow gap penalized
    const kin::u64 wide_id = server.submit(query);
    query.cost_context = 0; // uniform cost -> shortest (narrow) route
    const kin::u64 narrow_id = server.submit(query);
    kin::f32 wide_max_y = 0.0f;
    kin::f32 narrow_max_y = 0.0f;
    server.drain(kin::PathServer::DrainMode::DrainToQuiescent, [&](kin::PathServerResult&& result) {
        kin::f32 max_y = 0.0f;
        for (const kin::Vec2f& p : result.path) {
            max_y = std::max(max_y, p.y);
        }
        if (result.request_id == wide_id) {
            wide_max_y = max_y;
        } else if (result.request_id == narrow_id) {
            narrow_max_y = max_y;
        }
    });
    // The cost-aware solve detours down to the wide opening (rows >= 6);
    // the uniform solve stays in the top band (row 2 gap).
    REQUIRE(wide_max_y >= tile_world(0, 6).y);
    REQUIRE(narrow_max_y < tile_world(0, 5).y);
}

void test_run_vs_run_keystone() {
    const kin::u64 serial = run_scripted_sequence(1);
    const kin::u64 parallel = run_scripted_sequence(4);
    const kin::u64 parallel_again = run_scripted_sequence(4);
    REQUIRE(serial == parallel);
    REQUIRE(parallel == parallel_again);
}

} // namespace

int main() {
    test_drain_order_is_ascending_id_under_parallel_workers();
    test_barrier_delivers_exactly_the_admitted_set();
    test_drain_to_quiescent_admits_all_lanes();
    test_provisional_then_solved();
    test_cache_hit_and_version_eviction();
    test_join_accept_and_reject();
    test_join_rejects_blocked_connector();
    test_path_blocked();
    test_unreachable_goal_returns_no_path();
    test_cache_separates_cost_contexts();
    test_weighted_same_tile_returns_goal();
    test_provisional_path_helper();
    test_cancel_semantics();
    test_tile_cost_hook_prefers_wide_route();
    test_run_vs_run_keystone();
    std::printf("path_server_tests: all tests passed\n");
    return 0;
}
