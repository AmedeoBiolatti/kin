#include <kin/core/instrumentation.hpp>

#include <cassert>
#include <new>
#include <vector>

namespace {

void allocation_scope_counts_new_and_stl_growth() {
    kin::reset_allocation_counters();
    kin::AllocationScope scope;

    int* value = new int{7};
    delete value;

    std::vector<int> values;
    values.reserve(64);

    const kin::AllocationSnapshot snapshot = scope.snapshot();
    assert(snapshot.count >= 2);
    assert(snapshot.bytes >= sizeof(int) + sizeof(int) * 64);
}

void allocation_scope_reset_after_warmup_excludes_warmup() {
    kin::reset_allocation_counters();
    kin::AllocationScope scope;

    std::vector<int> values;
    values.reserve(8);
    const kin::AllocationSnapshot warmup = scope.reset_after_warmup();
    assert(warmup.count >= 1);
    assert(warmup.bytes >= sizeof(int) * 8);

    values.reserve(16);
    const kin::AllocationSnapshot measured = scope.snapshot();
    assert(measured.count >= 1);
    assert(measured.bytes >= sizeof(int) * 16);
}

void fail_on_allocation_throws_and_restores() {
    kin::reset_allocation_counters();

    bool threw = false;
    {
        kin::AllocationScope guard{true};
        try {
            int* value = new int{1};
            delete value;
        } catch (const std::bad_alloc&) {
            threw = true;
        }
    }
    assert(threw);

    int* value = new int{2};
    delete value;
}

void nested_fail_guards_restore_previous_state() {
    kin::reset_allocation_counters();

    kin::AllocationScope outer;
    outer.set_fail_on_allocation(true);

    bool outer_threw = false;
    try {
        int* value = new int{3};
        delete value;
    } catch (const std::bad_alloc&) {
        outer_threw = true;
    }
    assert(outer_threw);

    {
        kin::AllocationScope inner;
        inner.set_fail_on_allocation(false);
        int* value = new int{4};
        delete value;
    }

    bool restored_threw = false;
    try {
        int* value = new int{5};
        delete value;
    } catch (const std::bad_alloc&) {
        restored_threw = true;
    }
    assert(restored_threw);

    outer.set_fail_on_allocation(false);
    int* value = new int{6};
    delete value;
}

void timing_collector_records_aggregates() {
    kin::TimingCollector collector;
    collector.record("step", 10);
    collector.record("step", 30);
    collector.record("other", 7);

    const kin::TimingStats step = collector.stats("step");
    assert(step.calls == 2);
    assert(step.total_ns == 40);
    assert(step.average_ns() == 20);
    assert(step.max_ns == 30);

    const kin::TimingStats other = collector.stats("other");
    assert(other.calls == 1);
    assert(other.total_ns == 7);
    assert(other.average_ns() == 7);
    assert(other.max_ns == 7);

    assert(collector.summaries().size() == 2);
}

} // namespace

int main() {
    allocation_scope_counts_new_and_stl_growth();
    allocation_scope_reset_after_warmup_excludes_warmup();
    fail_on_allocation_throws_and_restores();
    nested_fail_guards_restore_previous_state();
    timing_collector_records_aggregates();
    return 0;
}
