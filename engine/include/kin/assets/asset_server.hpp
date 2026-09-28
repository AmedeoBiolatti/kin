#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/assets/asset_manager.hpp>
#include <kin/assets/load_job.hpp>
#include <kin/core/types.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace kin {

// Per-asset load lifecycle. Mirrors Bevy's LoadState closely enough for agent
// assertions: NotLoaded (never requested), Loading (in flight), Loaded, Failed.
enum class LoadState {
    NotLoaded,
    Loading,
    Loaded,
    Failed,
};

std::string_view load_state_name(LoadState state);

// Emitted into a per-frame buffer drained at the start of each pump().
struct AssetEvent {
    enum class Kind {
        Added,    // asset (and its own bytes) finished loading
        Modified, // reserved for hot-reload (future)
        Failed,   // loader threw / asset could not be produced
    };

    Kind kind = Kind::Added;
    std::string path;
    AssetType type = AssetType::Unknown;
    u64 request_id = 0;
};

struct AssetServerStats {
    std::size_t pending = 0; // queued + actively loading on workers
    std::size_t loading = 0; // tracked in LoadState::Loading
    std::size_t loaded = 0;
    std::size_t failed = 0;
};

struct AssetServerConfig {
    // Number of background worker threads. 0 picks a small default.
    int worker_count = 0;
};

// Async loader signature. Distinct from AssetManager's synchronous Loader<T>:
// it additionally receives a LoadContext so it can declare dependencies.
template<typename T>
using AsyncLoader = std::function<T(const std::filesystem::path&, class LoadContext&)>;

class AssetServer;

// Forward declaration is required before AssetServer refers to the concrete
// typed job in its private commit() declaration. The friend declaration below
// does not introduce a class template for ordinary lookup.
template<typename T>
struct TypedLoadJob;

// Passed to an async loader while it runs on a worker thread. Recording a
// dependency only appends a thunk; the actual enqueue happens later on the main
// thread (during pump), so this never touches the cache off-thread.
class LoadContext {
public:
    template<typename U>
    void require(std::string_view relative_path);

    // Internal: thunks that enqueue dependencies and return their cache keys.
    std::vector<std::function<std::string(AssetServer&)>> deps;
};

// Bevy-style asynchronous asset server layered additively over AssetManager.
//
// load_async() returns immediately with a handle whose storage is not yet
// loaded; background workers run the loader, and pump() applies completed loads
// on the main thread in deterministic request-id order. The synchronous
// AssetManager API is untouched and shares the same cache storage.
class AssetServer {
public:
    enum class PumpMode {
        Budgeted,          // apply whatever finished; never blocks (windowed)
        DrainToQuiescent,  // block until all in-flight work (incl. deps) is done
    };

    explicit AssetServer(AssetManager& manager, AssetServerConfig config = {});
    ~AssetServer();

    AssetServer(const AssetServer&) = delete;
    AssetServer& operator=(const AssetServer&) = delete;

    // Requests an asset without blocking. Idempotent: repeat calls (or a prior
    // sync load) dedupe to the same cache storage.
    template<typename T>
    AssetHandle<T> load_async(std::string_view relative_path);

    template<typename T>
    LoadState load_state(std::string_view relative_path) const {
        const std::string key = _manager.cache_key<T>(relative_path);
        return effective_state(key);
    }

    // True when the asset and its entire transitive dependency tree are Loaded.
    template<typename T>
    bool ready(std::string_view relative_path) const {
        std::set<std::string> visiting;
        return ready_key(_manager.cache_key<T>(relative_path), visiting);
    }

    // Register a dependency-aware loader for a type. Without one, a type falls
    // back to the AssetManager's synchronous loader (no dependencies).
    template<typename T>
    void register_async_loader(AsyncLoader<T> loader) {
        _async_loaders[std::type_index(typeid(T))] =
            std::make_shared<AsyncLoader<T>>(std::move(loader));
    }

    template<typename T>
    std::string key_for(std::string_view relative_path) const {
        return _manager.cache_key<T>(relative_path);
    }

    // Apply completed loads. In DrainToQuiescent (headless/server) this blocks
    // until everything queued and in flight is resolved, applying in ascending
    // request-id order for run-to-run determinism.
    void pump(PumpMode mode);
    void drain() { pump(PumpMode::DrainToQuiescent); }

    std::span<const AssetEvent> events() const { return _events; }
    AssetServerStats stats() const;

    AssetManager& manager() { return _manager; }

private:
    struct AssetRecord {
        LoadState state = LoadState::NotLoaded;
        std::vector<std::string> dependencies;
        // Storage identity separates generations when AssetManager unloads or
        // clears an asset while an async request is still running.
        std::shared_ptr<IAssetStorage> storage;
    };

    template<typename U>
    friend struct TypedLoadJob;

    template<typename T>
    AsyncLoader<T> find_async_loader() const {
        if (const auto it = _async_loaders.find(std::type_index(typeid(T)));
            it != _async_loaders.end()) {
            return *std::static_pointer_cast<AsyncLoader<T>>(it->second);
        }
        AssetManager* manager = &_manager;
        return [manager](const std::filesystem::path& resolved, LoadContext&) -> T {
            return manager->run_loader<T>(resolved);
        };
    }

    // Called by TypedLoadJob::apply_on_main on the main thread.
    template<typename T>
    void commit(TypedLoadJob<T>& job);

    // Reconciles the server's lifecycle record with manager storage. The
    // manager can unload or clear an asset without knowing about this server.
    LoadState effective_state(std::string_view key) const;
    bool ready_key(const std::string& key, std::set<std::string>& visiting) const;
    u64 next_request_id() { return _next_id++; }
    void enqueue(std::unique_ptr<LoadJob> job);
    void worker_loop(std::stop_token stop);

    AssetManager& _manager;

    mutable std::mutex _mutex;
    std::condition_variable _cv;      // wakes workers
    std::condition_variable _done_cv; // wakes the main thread during drain
    std::deque<std::unique_ptr<LoadJob>> _queue;
    std::vector<std::unique_ptr<LoadJob>> _completed;
    int _active = 0;
    bool _stopping = false;
    std::vector<std::jthread> _workers;

    u64 _next_id = 1;
    // One server-owned record keeps lifecycle and dependency edges together.
    // Manager storage remains the shared data owner used by sync and async APIs.
    std::unordered_map<std::string, AssetRecord> _assets;
    std::unordered_map<std::type_index, std::shared_ptr<void>> _async_loaders;
    std::vector<AssetEvent> _events;
};

// Concrete job carrying the typed staging buffer produced off-thread.
template<typename T>
struct TypedLoadJob final : LoadJob {
    std::string key;
    std::string path;
    std::filesystem::path resolved;
    AsyncLoader<T> loader;
    std::shared_ptr<AssetStorage<T>> storage;
    LoadContext ctx;
    std::optional<T> staging;
    std::optional<std::string> error;

    void run_off_thread() override {
        try {
            staging = loader(resolved, ctx);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown error";
        }
    }

    void apply_on_main(AssetServer& server) override { server.commit(*this); }
};

template<typename U>
void LoadContext::require(std::string_view relative_path) {
    std::string path{relative_path};
    deps.push_back([path](AssetServer& server) -> std::string {
        server.load_async<U>(path);
        return server.template key_for<U>(path);
    });
}

template<typename T>
void AssetServer::commit(TypedLoadJob<T>& job) {
    const auto record = _assets.find(job.key);
    // A newer request may have replaced the manager storage while this job
    // was running. Its completion belongs to the old generation and must be
    // ignored without touching the new record.
    if (record == _assets.end() || record->second.storage.get() != job.storage.get()) {
        return;
    }

    if (job.error.has_value()) {
        if (!_manager.is_current_storage<T>(job.path, job.storage)) {
            if (!_manager.contains_key(job.key)) {
                _assets.erase(record);
            }
            return;
        }
        record->second.state = LoadState::Failed;
        _events.push_back({AssetEvent::Kind::Failed, job.path, infer_asset_type(job.path), job.request_id});
        return;
    }

    if (!_manager.is_current_storage<T>(job.path, job.storage)) {
        // The request was unloaded or superseded while running. Its completion
        // must not repopulate a newer cache entry.
        if (!_manager.contains_key(job.key)) {
            _assets.erase(record);
        }
        return;
    }
    _manager.replace_loaded<T>(job.path, std::move(*job.staging));
    record->second.state = LoadState::Loaded;
    _events.push_back({AssetEvent::Kind::Added, job.path, infer_asset_type(job.path), job.request_id});

    // Enqueue dependencies discovered by the loader and record the edges. This
    // runs on the main thread, so load_async (and its cache access) is safe.
    std::vector<std::string>& edges = record->second.dependencies;
    for (auto& thunk : job.ctx.deps) {
        edges.push_back(thunk(*this));
    }
}

template<typename T>
AssetHandle<T> AssetServer::load_async(std::string_view relative_path) {
    const std::string key = _manager.cache_key<T>(relative_path);
    std::shared_ptr<AssetStorage<T>> storage = _manager.reserve_storage<T>(relative_path);
    AssetHandle<T> handle{storage};
    AssetRecord& record = _assets[key];

    if (storage->is_loaded) {
        record.state = LoadState::Loaded;
        record.storage = storage;
        return handle;
    }
    if (record.state == LoadState::Loading && record.storage.get() == storage.get()) {
        return handle; // already requested
    }

    // AssetManager::unload()/clear() can invalidate storage without knowing
    // about this server. Start a fresh generation and discard old dependency
    // edges instead of trusting stale lifecycle data.
    record.state = LoadState::Loading;
    record.storage = storage;
    record.dependencies.clear();

    auto job = std::make_unique<TypedLoadJob<T>>();
    job->request_id = next_request_id();
    job->key = key;
    job->path = std::string(relative_path);
    job->storage = storage;
    job->resolved = _manager.resolve(relative_path);
    job->loader = find_async_loader<T>();
    enqueue(std::move(job));
    return handle;
}

} // namespace kin
