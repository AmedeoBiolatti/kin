#include <kin/core/instrumentation.hpp>

#include <algorithm>
#include <atomic>

namespace kin {
namespace {

std::atomic<u64> g_allocation_count{0};
std::atomic<u64> g_allocation_bytes{0};
thread_local bool t_fail_on_allocation = false;

TimingSummary* find_summary(std::vector<TimingSummary>& summaries, std::string_view name) {
    const auto found = std::ranges::find_if(summaries, [name](const TimingSummary& summary) {
        return summary.name == name;
    });
    return found == summaries.end() ? nullptr : &*found;
}

const TimingSummary* find_summary(const std::vector<TimingSummary>& summaries, std::string_view name) {
    const auto found = std::ranges::find_if(summaries, [name](const TimingSummary& summary) {
        return summary.name == name;
    });
    return found == summaries.end() ? nullptr : &*found;
}

} // namespace

AllocationSnapshot allocation_snapshot() {
    return {
        .count = g_allocation_count.load(std::memory_order_relaxed),
        .bytes = g_allocation_bytes.load(std::memory_order_relaxed),
    };
}

AllocationSnapshot allocation_delta(AllocationSnapshot before, AllocationSnapshot after) {
    return {
        .count = after.count >= before.count ? after.count - before.count : 0,
        .bytes = after.bytes >= before.bytes ? after.bytes - before.bytes : 0,
    };
}

void reset_allocation_counters() {
    g_allocation_count.store(0, std::memory_order_relaxed);
    g_allocation_bytes.store(0, std::memory_order_relaxed);
}

AllocationScope::AllocationScope(bool fail_on_allocation)
    : _baseline(allocation_snapshot()) {
    if (fail_on_allocation) {
        set_fail_on_allocation(true);
    }
}

AllocationScope::~AllocationScope() {
    if (_has_fail_guard_override) {
        detail::set_allocation_failure_guard(_previous_fail_guard);
    }
}

AllocationSnapshot AllocationScope::snapshot() const {
    return allocation_delta(_baseline, allocation_snapshot());
}

AllocationSnapshot AllocationScope::reset() {
    const AllocationSnapshot result = snapshot();
    _baseline = allocation_snapshot();
    return result;
}

AllocationSnapshot AllocationScope::reset_after_warmup() {
    return reset();
}

void AllocationScope::set_fail_on_allocation(bool enabled) {
    if (!_has_fail_guard_override) {
        _previous_fail_guard = detail::set_allocation_failure_guard(enabled);
        _has_fail_guard_override = true;
        return;
    }
    detail::set_allocation_failure_guard(enabled);
}

void TimingCollector::clear() {
    _summaries.clear();
}

void TimingCollector::reset_stats() {
    for (TimingSummary& summary : _summaries) {
        summary.stats = {};
    }
}

void TimingCollector::record(std::string_view name, u64 elapsed_ns) {
    TimingSummary* summary = find_summary(_summaries, name);
    if (!summary) {
        if (detail::allocation_failure_guard_enabled()) {
            return;
        }
        _summaries.push_back({.name = std::string{name}, .stats = {}});
        summary = &_summaries.back();
    }

    ++summary->stats.calls;
    summary->stats.total_ns += elapsed_ns;
    summary->stats.max_ns = std::max(summary->stats.max_ns, elapsed_ns);
}

TimingStats TimingCollector::stats(std::string_view name) const {
    const TimingSummary* summary = find_summary(_summaries, name);
    return summary ? summary->stats : TimingStats{};
}

std::vector<TimingSummary> TimingCollector::summaries() const {
    return _summaries;
}

TimingScope::TimingScope(TimingCollector* collector, std::string_view name)
    : _collector(collector),
      _name(name),
      _start(Clock::now()) {
}

TimingScope::~TimingScope() {
    if (!_collector) {
        return;
    }

    const auto end = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - _start).count();
    _collector->record(_name, static_cast<u64>(std::max<i64>(elapsed, 0)));
}

namespace detail {

void record_allocation(std::size_t bytes) noexcept {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    g_allocation_bytes.fetch_add(static_cast<u64>(bytes), std::memory_order_relaxed);
}

bool allocation_failure_guard_enabled() noexcept {
    return t_fail_on_allocation;
}

bool set_allocation_failure_guard(bool enabled) noexcept {
    const bool previous = t_fail_on_allocation;
    t_fail_on_allocation = enabled;
    return previous;
}

} // namespace detail

} // namespace kin
