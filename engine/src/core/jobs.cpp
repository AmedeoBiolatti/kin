#include <kin/core/jobs.hpp>

#include <algorithm>

namespace kin {
namespace {

// Set while a thread runs parallel_for items, so a parallel_for inside one runs
// serially instead of waiting on the run that contains it.
thread_local bool t_in_parallel_for = false;

std::atomic<JobSystem*> g_default_override{nullptr};

struct ParallelScope {
    bool previous = t_in_parallel_for;
    ParallelScope() { t_in_parallel_for = true; }
    ~ParallelScope() { t_in_parallel_for = previous; }
};

} // namespace

JobSystem& default_job_system() {
    if (JobSystem* installed = g_default_override.load(std::memory_order_acquire)) {
        return *installed;
    }
    // Deliberately never deleted: systems that use it may be destroyed during
    // static destruction, after a function-local object would already be gone.
    static JobSystem* built_in = new JobSystem{};
    return *built_in;
}

void set_default_job_system(JobSystem* jobs) {
    g_default_override.store(jobs, std::memory_order_release);
}

JobSystem::JobSystem(JobSystemConfig config) {
    const i32 hardware = static_cast<i32>(std::max(1u, std::thread::hardware_concurrency()));
    const i32 workers = config.workers > 0 ? config.workers : std::max(1, hardware - 1);
    _max_background = config.max_background > 0 ? std::min(config.max_background, workers)
                                                 : std::max(1, workers / 2);
    _workers.reserve(static_cast<std::size_t>(workers));
    for (i32 i = 0; i < workers; ++i) {
        _workers.emplace_back([this] { worker_loop(); });
    }
}

JobSystem::~JobSystem() {
    std::deque<StatePtr> queued;
    std::vector<StatePtr> running;
    {
        std::lock_guard lock{_mutex};
        _stopping = true;
        queued.swap(_queue);
        running = _running;
        _awaiting_then.clear();
    }
    for (const StatePtr& job : queued) {
        job->cancel();
    }
    for (const StatePtr& job : running) {
        job->stop.request_stop();
    }
    _work_cv.notify_all();
    for (std::thread& worker : _workers) {
        worker.join();
    }
}

void JobSystem::enqueue(StatePtr state, bool has_then) {
    {
        std::lock_guard lock{_mutex};
        if (has_then) {
            _awaiting_then.push_back(state);
        }
        _queue.push_back(std::move(state));
    }
    _work_cv.notify_one();
}

void JobSystem::worker_loop() {
    u64 seen_parallel = 0;
    std::unique_lock lock{_mutex};
    for (;;) {
        _work_cv.wait(lock, [&] {
            return _stopping || (_parallel && _parallel_generation != seen_parallel) ||
                   (!_queue.empty() && _running.size() < static_cast<std::size_t>(_max_background));
        });
        if (_stopping) {
            return;
        }
        // parallel_for work first: its caller is waiting on it.
        if (_parallel && _parallel_generation != seen_parallel) {
            seen_parallel = _parallel_generation;
            const std::shared_ptr<ParallelRun> run = _parallel;
            lock.unlock();
            run_parallel_items(*run);
            lock.lock();
            continue;
        }
        StatePtr job = std::move(_queue.front());
        _queue.pop_front();
        if (!job->start()) {
            // Cancelled while queued. drain() may be waiting for the queue to empty.
            if (_queue.empty()) {
                _idle_cv.notify_all();
            }
            continue;
        }
        _running.push_back(job);
        lock.unlock();
        try {
            job->task(job->stop.get_token());
        } catch (...) {
            job->error = std::current_exception();
        }
        job->task = nullptr; // release what the work captured
        job->finish(detail::JobStatus::Done);
        lock.lock();
        std::erase(_running, job);
        // A background slot is free again, and drain() may be waiting.
        _work_cv.notify_one();
        _idle_cv.notify_all();
    }
}

void JobSystem::run_parallel_items(ParallelRun& run) {
    const ParallelScope scope;
    for (i32 i = run.next.fetch_add(1); i < run.count; i = run.next.fetch_add(1)) {
        try {
            (*run.body)(i);
        } catch (...) {
            std::lock_guard error_lock{run.error_mutex};
            if (!run.error) {
                run.error = std::current_exception();
            }
        }
        if (run.completed.fetch_add(1, std::memory_order_acq_rel) + 1 == run.count) {
            std::lock_guard lock{_mutex};
            _parallel_cv.notify_all();
        }
    }
}

void JobSystem::parallel_for(i32 count, const std::function<void(i32)>& body) {
    if (count <= 0) {
        return;
    }
    if (count == 1 || t_in_parallel_for) {
        for (i32 i = 0; i < count; ++i) {
            body(i);
        }
        return;
    }
    std::lock_guard one_at_a_time{_parallel_mutex};
    auto run = std::make_shared<ParallelRun>();
    run->body = &body;
    run->count = count;
    {
        std::lock_guard lock{_mutex};
        _parallel = run;
        ++_parallel_generation;
    }
    _work_cv.notify_all();
    run_parallel_items(*run);
    {
        std::unique_lock lock{_mutex};
        _parallel_cv.wait(lock, [&] { return run->completed.load(std::memory_order_acquire) == count; });
        _parallel = nullptr;
    }
    if (run->error) {
        std::rethrow_exception(run->error);
    }
}

void JobSystem::pump() {
    for (;;) {
        StatePtr job;
        {
            std::lock_guard lock{_mutex};
            if (_awaiting_then.empty() || !_awaiting_then.front()->is_finished()) {
                return;
            }
            job = std::move(_awaiting_then.front());
            _awaiting_then.pop_front();
        }
        std::function<void()> then = std::move(job->then);
        job->then = nullptr;
        if (job->error) {
            std::rethrow_exception(job->error);
        }
        if (job->status.load(std::memory_order_acquire) == detail::JobStatus::Done) {
            then();
        }
    }
}

void JobSystem::drain() {
    for (;;) {
        {
            std::unique_lock lock{_mutex};
            _idle_cv.wait(lock, [&] { return _stopping || (_queue.empty() && _running.empty()); });
        }
        pump(); // may start more jobs
        std::lock_guard lock{_mutex};
        if (_stopping || (_queue.empty() && _running.empty() && _awaiting_then.empty())) {
            return;
        }
    }
}

JobSystemStats JobSystem::stats() const {
    std::lock_guard lock{_mutex};
    JobSystemStats out{
        .workers = worker_count(),
        .max_background = _max_background,
        .running = _running.size(),
        .awaiting_pump = _awaiting_then.size(),
    };
    out.queued = static_cast<std::size_t>(std::ranges::count_if(_queue, [](const StatePtr& job) {
        return job->status.load(std::memory_order_acquire) == detail::JobStatus::Queued;
    }));
    return out;
}

} // namespace kin
