#include <cassert>
#include <kin/core/jobs.hpp>
#include <mutex>
#include <stdexcept>
#include <vector>

int main() {
    kin::JobSystem jobs{{.single_threaded = true}};
    assert(jobs.worker_count() == 0);
    std::vector<int> applied;
    std::mutex submit_lock;
    kin::Job<int> first;
    {
        // Asset/path servers submit under their own locks. Running inline in
        // enqueue() would deadlock when the work tries to acquire the same lock.
        const std::lock_guard lock{submit_lock};
        first = jobs.run(
            [&] {
                const std::lock_guard work_lock{submit_lock};
                return 7;
            },
            [&](int& result) { applied.push_back(result); });
    }
    assert(!first.ready());
    assert(first.get() == 7); // wait helps the queue, without applying callbacks
    assert(applied.empty());
    jobs.run([] { return 8; },
             [&](int& result) {
                 applied.push_back(result);
                 jobs.run([] { return 9; }, [&](int& nested) { applied.push_back(nested); });
             });
    jobs.pump();
    assert((applied == std::vector<int>{7, 8}));
    jobs.drain();
    assert((applied == std::vector<int>{7, 8, 9}));

    auto nested = jobs.run([&] { return jobs.run([] { return 42; }).get(); });
    assert(nested.get() == 42);
    auto cancelled = jobs.run([] { assert(false); });
    cancelled.cancel();
    jobs.drain();
    assert(cancelled.cancelled());

    bool caught = false;
    auto failed = jobs.run([]() -> int { throw std::runtime_error("serial error"); });
    try {
        failed.get();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);
    jobs.run([] { throw std::runtime_error("callback error"); }, [] { assert(false); });
    caught = false;
    try {
        jobs.pump();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);
    jobs.drain();

    int sum = 0;
    jobs.parallel_for(4, [&](int i) { jobs.parallel_for(3, [&](int j) { sum += i + j; }); });
    assert(sum == 30);
    kin::Job<void> abandoned;
    {
        kin::JobSystem temporary{{.single_threaded = true}};
        abandoned = temporary.run([] { assert(false); });
    }
    assert(abandoned.cancelled());
    abandoned.wait(); // handle may outlive its cancelled queue
    assert(jobs.stats().queued == 0 && jobs.stats().awaiting_pump == 0);
}
