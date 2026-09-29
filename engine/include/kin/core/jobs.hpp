#pragma once

#include <kin/core/types.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kin {

// Background jobs and parallel loops on a pool of worker threads.
//
//   kin::JobSystem jobs;
//   kin::Job<Shadows> job = jobs.run([map] { return compute_shadows(map); });
//   ...
//   if (job.ready()) { use(job.get()); }            // poll from a later frame
//
//   jobs.run([map] { return compute_shadows(map); },
//            [this](Shadows& s) { _shadows = std::move(s); }); // applied in pump()
//   jobs.pump();                                      // once per frame, main thread
//
//   jobs.parallel_for(rows, [&](i32 row) { blur_row(row); });
//
// Determinism: `then` callbacks run in pump() on the thread that calls it, strictly
// in submission order, so the order results are applied never depends on thread
// timing. Which frame a result arrives in still does; headless and replayed runs
// call drain() at fixed points instead, which waits for every job first.

class JobSystem;

struct JobSystemConfig {
    // Worker threads. 0: one per hardware thread minus one for the main thread,
    // at least 1.
    i32 workers = 0;
    // Most workers running background jobs at once, so parallel_for always has
    // threads free. 0: half the workers, at least 1.
    i32 max_background = 0;
};

struct JobSystemStats {
    i32 workers = 0;
    i32 max_background = 0;
    std::size_t queued = 0;        // background jobs waiting for a worker
    std::size_t running = 0;       // background jobs on a worker now
    std::size_t awaiting_pump = 0; // jobs whose `then` has not run yet
};

namespace detail {

enum class JobStatus : u8 { Queued, Running, Done, Cancelled };

struct JobStateBase {
    virtual ~JobStateBase() = default;

    std::function<void(std::stop_token)> task;
    std::function<void()> then;
    std::stop_source stop;
    std::exception_ptr error;
    std::atomic<JobStatus> status{JobStatus::Queued};
    mutable std::mutex mutex;
    mutable std::condition_variable finished;

    bool is_finished() const {
        const JobStatus s = status.load(std::memory_order_acquire);
        return s == JobStatus::Done || s == JobStatus::Cancelled;
    }
    void wait() const {
        std::unique_lock lock{mutex};
        finished.wait(lock, [this] { return is_finished(); });
    }
    void finish(JobStatus final_status) {
        {
            std::lock_guard lock{mutex};
            status.store(final_status, std::memory_order_release);
        }
        finished.notify_all();
    }
    // Queued -> Running; false when the job was cancelled first.
    bool start() {
        JobStatus expected = JobStatus::Queued;
        return status.compare_exchange_strong(expected, JobStatus::Running, std::memory_order_acq_rel);
    }
    // A queued job becomes Cancelled and will never start; a running one is asked
    // to stop through its stop token.
    void cancel() {
        stop.request_stop();
        std::unique_lock lock{mutex};
        JobStatus expected = JobStatus::Queued;
        if (status.compare_exchange_strong(expected, JobStatus::Cancelled, std::memory_order_acq_rel)) {
            lock.unlock();
            finished.notify_all();
        }
    }
};

template<typename T>
struct JobState final : JobStateBase {
    // std::monostate stands in for void results.
    std::optional<std::conditional_t<std::is_void_v<T>, std::monostate, T>> value;
};

} // namespace detail

// Handle to a job started with JobSystem::run(). Copies share the same job. A job
// keeps running when its handles are gone; its result is then discarded.
template<typename T>
class Job {
public:
    Job() = default;

    bool valid() const { return _state != nullptr; }
    // Finished: done, failed or cancelled. Never blocks.
    bool ready() const { return _state && _state->is_finished(); }
    void wait() const {
        if (_state) {
            _state->wait();
        }
    }
    // True once the job was cancelled before it started, so it never ran.
    bool cancelled() const {
        return _state && _state->status.load(std::memory_order_acquire) == detail::JobStatus::Cancelled;
    }

    // Waits, then returns the result. Rethrows the job's exception; throws
    // std::runtime_error when the job was cancelled before it ran.
    std::add_lvalue_reference_t<T> get() {
        wait();
        if (!_state) {
            throw std::runtime_error("kin::Job::get: empty job");
        }
        if (_state->error) {
            std::rethrow_exception(_state->error);
        }
        if (!_state->value) {
            throw std::runtime_error("kin::Job::get: job was cancelled");
        }
        if constexpr (!std::is_void_v<T>) {
            return *_state->value;
        }
    }

    // A queued job never runs. A running job has stop requested on the
    // std::stop_token it was given (work that takes none runs to the end).
    void cancel() {
        if (_state) {
            _state->cancel();
        }
    }

private:
    friend class JobSystem;
    explicit Job(std::shared_ptr<detail::JobState<T>> state) : _state(std::move(state)) {}

    std::shared_ptr<detail::JobState<T>> _state;
};

class JobSystem {
public:
    explicit JobSystem(JobSystemConfig config = {});
    // Cancels queued jobs, requests stop on running ones and waits for them.
    // `then` callbacks that have not run are dropped.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    i32 worker_count() const { return static_cast<i32>(_workers.size()); }

    // Runs `work()` or `work(std::stop_token)` on a worker. The result is kept in
    // the returned Job.
    template<typename F>
    auto run(F&& work) {
        return submit(std::forward<F>(work), nullptr);
    }

    // As above, and `then(result&)` (or `then()` for void work) runs in pump() or
    // drain() on the calling thread, in submission order. A job that throws makes
    // pump() rethrow; a cancelled job's `then` is skipped.
    template<typename F, typename Then>
    auto run(F&& work, Then&& then) {
        return submit(std::forward<F>(work), std::forward<Then>(then));
    }

    // Calls body(i) once for each i in [0, count), on the workers and the calling
    // thread together, and returns when all are done. Takes priority over
    // background jobs. A parallel_for inside another runs serially.
    void parallel_for(i32 count, const std::function<void(i32)>& body);

    // Runs the `then` of finished jobs in submission order, stopping at the first
    // job still queued or running. Call from one thread (normally the main one).
    void pump();
    // Waits for every job, including ones started by `then` callbacks, running
    // each `then` in submission order.
    void drain();

    JobSystemStats stats() const;

private:
    using StatePtr = std::shared_ptr<detail::JobStateBase>;

    template<typename F, typename Then>
    auto submit(F&& work, Then&& then);

    void enqueue(StatePtr state, bool has_then);
    void worker_loop();

    // One parallel_for's bookkeeping; see parallel_for().
    struct ParallelRun {
        const std::function<void(i32)>* body = nullptr;
        std::atomic<i32> next{0};
        std::atomic<i32> completed{0};
        i32 count = 0;
        std::exception_ptr error;
        std::mutex error_mutex;
    };
    void run_parallel_items(ParallelRun& run);

    i32 _max_background = 1;
    mutable std::mutex _mutex;
    std::condition_variable _work_cv;     // wakes workers
    std::condition_variable _parallel_cv; // wakes parallel_for's caller
    std::condition_variable _idle_cv;     // wakes drain() when a job finishes
    std::deque<StatePtr> _queue;
    std::deque<StatePtr> _awaiting_then; // jobs with a `then`, in submission order
    std::vector<StatePtr> _running;       // background jobs on a worker now
    std::shared_ptr<ParallelRun> _parallel;
    u64 _parallel_generation = 0;
    std::mutex _parallel_mutex; // one parallel_for at a time
    bool _stopping = false;
    std::vector<std::thread> _workers;
};

// The pool kin's own systems share: the ECS scheduler's parallel systems, the
// asset server and the path server, unless one is given another. Games can use
// it too. Created on first use with default settings, and kept until the program
// exits (never destroyed, so nothing can outlive it).
JobSystem& default_job_system();

// Makes `jobs` the pool default_job_system() returns; null restores the
// built-in one. Call it before creating worlds or servers, which keep the pool
// they started with, and keep `jobs` alive while any of them exists.
void set_default_job_system(JobSystem* jobs);

template<typename F, typename Then>
auto JobSystem::submit(F&& work, Then&& then) {
    constexpr bool takes_stop = std::is_invocable_v<std::decay_t<F>&, std::stop_token>;
    using R = typename std::conditional_t<takes_stop,
                                 std::invoke_result<std::decay_t<F>&, std::stop_token>,
                                 std::invoke_result<std::decay_t<F>&>>::type;
    auto state = std::make_shared<detail::JobState<R>>();
    detail::JobState<R>* raw = state.get();
    state->task = [raw, fn = std::forward<F>(work)](std::stop_token stop) mutable {
        if constexpr (std::is_void_v<R>) {
            if constexpr (takes_stop) {
                fn(stop);
            } else {
                fn();
            }
            raw->value.emplace();
        } else if constexpr (takes_stop) {
            raw->value.emplace(fn(stop));
        } else {
            raw->value.emplace(fn());
        }
    };
    constexpr bool has_then = !std::is_same_v<std::decay_t<Then>, std::nullptr_t>;
    if constexpr (has_then) {
        state->then = [raw, next = std::forward<Then>(then)]() mutable {
            if constexpr (std::is_void_v<R>) {
                next();
            } else {
                next(*raw->value);
            }
        };
    }
    Job<R> job{state};
    enqueue(std::move(state), has_then);
    return job;
}

} // namespace kin
