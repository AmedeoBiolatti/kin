#include <kin/assets/asset_server.hpp>

#include <kin/platform/log.hpp>

#include <algorithm>

namespace kin {

std::string_view load_state_name(LoadState state) {
    switch (state) {
    case LoadState::NotLoaded: return "NotLoaded";
    case LoadState::Loading: return "Loading";
    case LoadState::Loaded: return "Loaded";
    case LoadState::Failed: return "Failed";
    }
    return "NotLoaded";
}

AssetServer::AssetServer(AssetManager& manager, AssetServerConfig config)
    : _manager(manager),
      _jobs(config.jobs ? config.jobs : &default_job_system()),
      _max_tasks(config.worker_count > 0 ? config.worker_count : 2) {
    KIN_LOG_INFO_F("asset",
                   "asset server created",
                   (LogFields{{.name = "max_loads", .value = std::to_string(_max_tasks)}}));
}

AssetServer::~AssetServer() {
    // Unstarted loads are dropped. Tasks still queued on the job system never
    // run; running ones finish their current load and stop.
    std::vector<Job<void>> tasks;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
        _queue.clear();
        tasks.swap(_task_handles);
    }
    for (Job<void>& task : tasks) {
        task.cancel();
    }
    for (Job<void>& task : tasks) {
        task.wait();
    }
}

void AssetServer::enqueue(std::unique_ptr<LoadJob> job) {
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.push_back(std::move(job));
    start_tasks_locked();
}

void AssetServer::start_tasks_locked() {
    std::erase_if(_task_handles, [](const Job<void>& task) { return task.ready(); });
    // Tasks not running a load will take the next queued ones; start another
    // only when there are more queued loads than such idle tasks.
    while (!_stopping && _tasks < _max_tasks && static_cast<int>(_queue.size()) > _tasks - _active) {
        ++_tasks;
        _task_handles.push_back(_jobs->run([this] { run_task(); }));
    }
}

void AssetServer::run_task() {
    for (;;) {
        std::unique_ptr<LoadJob> job;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stopping || _queue.empty()) {
                --_tasks;
                break;
            }
            job = std::move(_queue.front());
            _queue.pop_front();
            ++_active;
        }
        job->run_off_thread();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _completed.push_back(std::move(job));
            --_active;
        }
        _done_cv.notify_all();
    }
    _done_cv.notify_all();
}

bool AssetServer::run_one_queued() {
    std::unique_ptr<LoadJob> job;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.empty()) {
            return false;
        }
        job = std::move(_queue.front());
        _queue.pop_front();
        ++_active;
    }
    job->run_off_thread();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _completed.push_back(std::move(job));
        --_active;
    }
    return true;
}

namespace {

void apply_sorted(std::vector<std::unique_ptr<LoadJob>>& batch, AssetServer& server) {
    std::sort(batch.begin(), batch.end(),
              [](const std::unique_ptr<LoadJob>& a, const std::unique_ptr<LoadJob>& b) {
                  return a->request_id < b->request_id;
              });
    for (auto& job : batch) {
        job->apply_on_main(server); // may enqueue dependency jobs
    }
}

} // namespace

void AssetServer::pump(PumpMode mode) {
    _events.clear();

    if (mode == PumpMode::Budgeted) {
        // Non-blocking: apply whatever finished so far, once.
        std::vector<std::unique_ptr<LoadJob>> batch;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            batch.swap(_completed);
        }
        apply_sorted(batch, *this);
        return;
    }

    // DrainToQuiescent: process one generation at a time. Wait until all queued
    // and in-flight work has drained into _completed, then apply that whole
    // generation in ascending request-id order. Applying may enqueue dependency
    // jobs; those always carry higher ids (allocated during this apply) and form
    // the next generation, so the global apply/event order is deterministic
    // regardless of how worker threads were scheduled.
    for (;;) {
        // Help with the queued loads instead of only waiting: the job system's
        // workers may all be busy with other jobs.
        while (run_one_queued()) {
        }
        std::vector<std::unique_ptr<LoadJob>> batch;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _done_cv.wait(lock, [&] { return _queue.empty() && _active == 0; });
            if (_completed.empty()) {
                return; // fully quiescent
            }
            batch.swap(_completed);
        }
        apply_sorted(batch, *this);
    }
}

LoadState AssetServer::effective_state(std::string_view key) const {
    const auto it = _assets.find(std::string{key});
    if (it == _assets.end()) {
        return LoadState::NotLoaded;
    }
    switch (it->second.state) {
    case LoadState::Loaded:
        return _manager.loaded_key(key) ? LoadState::Loaded : LoadState::NotLoaded;
    case LoadState::Loading:
        if (!_manager.contains_key(key)) {
            return LoadState::NotLoaded;
        }
        // A synchronous load can complete the same reserved storage while an
        // async job is still queued. Reflect the storage's truth immediately.
        return _manager.loaded_key(key) ? LoadState::Loaded : LoadState::Loading;
    case LoadState::Failed:
        if (!_manager.contains_key(key)) {
            return LoadState::NotLoaded;
        }
        // The caller may recover a failed request through AssetManager's sync
        // API; do not keep reporting the obsolete failure.
        return _manager.loaded_key(key) ? LoadState::Loaded : LoadState::Failed;
    case LoadState::NotLoaded:
        return LoadState::NotLoaded;
    }
    return LoadState::NotLoaded;
}

bool AssetServer::ready_key(const std::string& key, std::set<std::string>& visiting) const {
    if (effective_state(key) != LoadState::Loaded) {
        return false;
    }
    if (!visiting.insert(key).second) {
        return true; // cycle guard: already proven on this path
    }
    if (const auto asset = _assets.find(key); asset != _assets.end()) {
        for (const std::string& child : asset->second.dependencies) {
            if (!ready_key(child, visiting)) {
                return false;
            }
        }
    }
    return true;
}

AssetServerStats AssetServer::stats() const {
    AssetServerStats result{};
    for (const auto& [key, asset] : _assets) {
        (void)asset;
        switch (effective_state(key)) {
        case LoadState::Loading:
            ++result.loading;
            break;
        case LoadState::Loaded:
            ++result.loaded;
            break;
        case LoadState::Failed:
            ++result.failed;
            break;
        case LoadState::NotLoaded: break;
        }
    }
    std::lock_guard<std::mutex> lock(_mutex);
    result.pending = _queue.size() + static_cast<std::size_t>(_active);
    return result;
}

} // namespace kin
