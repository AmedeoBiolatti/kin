#include <kin/core/jobs.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <latch>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// A gate jobs can block on until the test opens it. Tests synchronise on gates
// and latches, never on sleeps, so they do not depend on timing.
class Gate {
public:
    void open() { _promise.set_value(); }
    void wait() const { _future.wait(); }

private:
    std::promise<void> _promise;
    std::shared_future<void> _future = _promise.get_future().share();
};

void test_run_returns_results() {
    kin::JobSystem jobs{{.workers = 2}};
    assert(jobs.worker_count() == 2);
    kin::Job<int> answer = jobs.run([] { return 6 * 7; });
    kin::Job<std::string> text = jobs.run([] { return std::string{"kin"}; });
    kin::Job<void> nothing = jobs.run([] {});
    assert(answer.get() == 42);
    assert(text.get() == "kin");
    nothing.get();
    assert(answer.ready() && nothing.ready());
    assert(!answer.cancelled());
    assert(!kin::Job<int>{}.valid());
}

void test_exceptions_reach_get_and_pump() {
    kin::JobSystem jobs{{.workers = 1}};
    kin::Job<int> failing = jobs.run([]() -> int { throw std::runtime_error("boom"); });
    bool caught = false;
    try {
        failing.get();
    } catch (const std::runtime_error& e) {
        caught = std::string{e.what()} == "boom";
    }
    assert(caught);

    bool then_ran = false;
    jobs.run([]() -> int { throw std::logic_error("bad"); }, [&](int&) { then_ran = true; });
    caught = false;
    try {
        jobs.drain();
    } catch (const std::logic_error&) {
        caught = true;
    }
    assert(caught && !then_ran);
    jobs.drain(); // the failed job was consumed; nothing is left
    assert(jobs.stats().awaiting_pump == 0);
}

void test_then_runs_in_submission_order() {
    kin::JobSystem jobs{{.workers = 4, .max_background = 4}};
    // Later jobs finish first: each job waits for the one after it to start.
    constexpr int count = 6;
    std::vector<Gate> gates(count);
    std::vector<int> order;
    for (int i = 0; i < count; ++i) {
        jobs.run(
            [&gates, i] {
                if (i + 1 < count) {
                    gates[static_cast<std::size_t>(i + 1)].wait();
                }
                return i;
            },
            [&order](int& value) { order.push_back(value); });
    }
    // Nothing can finish yet except the last job; pump applies nothing out of order.
    jobs.pump();
    assert(order.empty() || order.front() == 0);
    for (int i = count - 1; i > 0; --i) {
        gates[static_cast<std::size_t>(i)].open();
    }
    jobs.drain();
    std::vector<int> expected(count);
    std::iota(expected.begin(), expected.end(), 0);
    assert(order == expected);
}

void test_pump_stops_at_first_unfinished_job() {
    kin::JobSystem jobs{{.workers = 2, .max_background = 2}};
    Gate first_may_finish;
    std::latch second_done{1};
    std::vector<int> applied;
    jobs.run([&] { first_may_finish.wait(); return 1; }, [&](int& v) { applied.push_back(v); });
    jobs.run([&] { second_done.count_down(); return 2; }, [&](int& v) { applied.push_back(v); });
    second_done.wait();
    // The second job is finished but the first is not, so nothing is applied.
    for (int i = 0; i < 3; ++i) {
        jobs.pump();
    }
    assert(applied.empty());
    first_may_finish.open();
    jobs.drain();
    assert((applied == std::vector<int>{1, 2}));
}

void test_then_can_start_more_jobs() {
    kin::JobSystem jobs{{.workers = 2}};
    std::vector<int> applied;
    jobs.run([] { return 1; }, [&](int& v) {
        applied.push_back(v);
        jobs.run([] { return 2; }, [&](int& w) { applied.push_back(w); });
    });
    jobs.drain();
    assert((applied == std::vector<int>{1, 2}));
}

void test_cancel() {
    kin::JobSystem jobs{{.workers = 1, .max_background = 1}};
    Gate release;
    std::latch started{1};
    // The only worker is busy, so the second job stays queued.
    kin::Job<int> busy = jobs.run([&] { started.count_down(); release.wait(); return 1; });
    started.wait();
    bool then_ran = false;
    kin::Job<int> queued = jobs.run([] { return 2; }, [&](int&) { then_ran = true; });
    assert(jobs.stats().queued == 1);
    queued.cancel();
    assert(queued.ready() && queued.cancelled());
    bool threw = false;
    try {
        queued.get();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    release.open();
    jobs.drain();
    assert(busy.get() == 1 && !then_ran);

    // A running job sees stop requested through its stop token.
    std::latch running{1};
    kin::Job<int> stoppable = jobs.run([&](std::stop_token stop) {
        running.count_down();
        while (!stop.stop_requested()) {
            std::this_thread::yield();
        }
        return -1;
    });
    running.wait();
    stoppable.cancel();
    assert(stoppable.get() == -1);
    assert(!stoppable.cancelled()); // it ran, and stopped early
}

void test_background_limit() {
    kin::JobSystem jobs{{.workers = 4, .max_background = 2}};
    assert(jobs.stats().max_background == 2);
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    Gate release;
    std::vector<kin::Job<void>> started;
    for (int i = 0; i < 6; ++i) {
        started.push_back(jobs.run([&] {
            const int now = active.fetch_add(1) + 1;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            release.wait();
            active.fetch_sub(1);
        }));
    }
    // Two jobs fill the background slots; parallel_for still gets the other
    // workers and the calling thread.
    while (active.load() < 2) {
        std::this_thread::yield();
    }
    std::vector<std::atomic<int>> hits(1000);
    jobs.parallel_for(1000, [&](kin::i32 i) { hits[static_cast<std::size_t>(i)].fetch_add(1); });
    assert(std::ranges::all_of(hits, [](const std::atomic<int>& h) { return h.load() == 1; }));
    assert(peak.load() <= 2);
    release.open();
    jobs.drain();
    assert(peak.load() == 2);
}

void test_parallel_for() {
    kin::JobSystem jobs{{.workers = 3}};
    std::vector<int> squares(500);
    jobs.parallel_for(500, [&](kin::i32 i) { squares[static_cast<std::size_t>(i)] = i * i; });
    for (int i = 0; i < 500; ++i) {
        assert(squares[static_cast<std::size_t>(i)] == i * i);
    }
    jobs.parallel_for(0, [](kin::i32) { assert(false); });

    // Nested loops run serially inside the outer one instead of deadlocking.
    std::atomic<int> total{0};
    jobs.parallel_for(8, [&](kin::i32) {
        jobs.parallel_for(10, [&](kin::i32) { total.fetch_add(1); });
    });
    assert(total.load() == 80);

    // A background job can run its own parallel_for.
    kin::Job<int> nested = jobs.run([&] {
        std::atomic<int> sum{0};
        jobs.parallel_for(100, [&](kin::i32 i) { sum.fetch_add(i); });
        return sum.load();
    });
    assert(nested.get() == 4950);

    bool caught = false;
    try {
        jobs.parallel_for(50, [](kin::i32 i) {
            if (i == 17) {
                throw std::runtime_error("item 17");
            }
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);
}

void test_destructor_cancels_and_stops() {
    kin::Job<int> queued;
    kin::Job<int> running;
    {
        kin::JobSystem jobs{{.workers = 1, .max_background = 1}};
        std::latch started{1};
        running = jobs.run([&](std::stop_token stop) {
            started.count_down();
            while (!stop.stop_requested()) {
                std::this_thread::yield();
            }
            return 7;
        });
        started.wait();
        queued = jobs.run([] { return 8; });
    } // must not hang: queued is cancelled, running is asked to stop
    assert(queued.cancelled());
    assert(running.get() == 7);
}

} // namespace

int main() {
    test_run_returns_results();
    test_exceptions_reach_get_and_pump();
    test_then_runs_in_submission_order();
    test_pump_stops_at_first_unfinished_job();
    test_then_can_start_more_jobs();
    test_cancel();
    test_background_limit();
    test_parallel_for();
    test_destructor_cancels_and_stops();
    return 0;
}
