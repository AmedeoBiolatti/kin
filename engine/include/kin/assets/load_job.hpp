#pragma once

#include <kin/core/types.hpp>

namespace kin {

class AssetServer;

// Type-erased unit of asynchronous loading work.
//
// `run_off_thread` executes on a background worker and must only touch state
// owned by the job itself (its staging buffer / load context). `apply_on_main`
// executes on the main thread during AssetServer::pump and is allowed to mutate
// the server, the cache, and the event buffer.
struct LoadJob {
    u64 request_id = 0;

    LoadJob() = default;
    explicit LoadJob(u64 id) : request_id(id) {}
    virtual ~LoadJob() = default;

    LoadJob(const LoadJob&) = delete;
    LoadJob& operator=(const LoadJob&) = delete;

    virtual void run_off_thread() = 0;
    virtual void apply_on_main(AssetServer& server) = 0;
};

} // namespace kin
