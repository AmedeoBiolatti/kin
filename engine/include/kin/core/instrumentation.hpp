#pragma once

#include <kin/core/types.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct AllocationSnapshot {
    u64 count = 0;
    u64 bytes = 0;
};

[[nodiscard]] AllocationSnapshot allocation_snapshot();
[[nodiscard]] AllocationSnapshot allocation_delta(AllocationSnapshot before, AllocationSnapshot after);
void reset_allocation_counters();

class AllocationScope {
public:
    explicit AllocationScope(bool fail_on_allocation = false);
    ~AllocationScope();

    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;
    AllocationScope(AllocationScope&&) = delete;
    AllocationScope& operator=(AllocationScope&&) = delete;

    [[nodiscard]] AllocationSnapshot snapshot() const;
    AllocationSnapshot reset();
    AllocationSnapshot reset_after_warmup();
    void set_fail_on_allocation(bool enabled);

private:
    AllocationSnapshot _baseline{};
    bool _has_fail_guard_override = false;
    bool _previous_fail_guard = false;
};

struct TimingStats {
    u64 total_ns = 0;
    u64 calls = 0;
    u64 max_ns = 0;

    [[nodiscard]] u64 average_ns() const {
        return calls > 0 ? total_ns / calls : 0;
    }
};

struct TimingSummary {
    std::string name;
    TimingStats stats;
};

class TimingCollector {
public:
    void clear();
    void reset_stats();
    void record(std::string_view name, u64 elapsed_ns);
    [[nodiscard]] TimingStats stats(std::string_view name) const;
    [[nodiscard]] std::vector<TimingSummary> summaries() const;

private:
    std::vector<TimingSummary> _summaries;
};

class TimingScope {
public:
    TimingScope(TimingCollector* collector, std::string_view name);
    ~TimingScope();

    TimingScope(const TimingScope&) = delete;
    TimingScope& operator=(const TimingScope&) = delete;
    TimingScope(TimingScope&&) = delete;
    TimingScope& operator=(TimingScope&&) = delete;

private:
    using Clock = std::chrono::steady_clock;

    TimingCollector* _collector = nullptr;
    std::string_view _name;
    Clock::time_point _start;
};

namespace detail {

void record_allocation(std::size_t bytes) noexcept;
bool allocation_failure_guard_enabled() noexcept;
bool set_allocation_failure_guard(bool enabled) noexcept;

} // namespace detail

} // namespace kin

#if defined(KIN_ENABLE_INSTRUMENTATION)
#define KIN_INSTRUMENTATION_JOIN_DETAIL(a, b) a##b
#define KIN_INSTRUMENTATION_JOIN(a, b) KIN_INSTRUMENTATION_JOIN_DETAIL(a, b)
#define KIN_TIMING_SCOPE(collector, name) \
    ::kin::TimingScope KIN_INSTRUMENTATION_JOIN(_kin_timing_scope_, __LINE__)((collector), (name))
#else
#define KIN_TIMING_SCOPE(collector, name) ((void)0)
#endif
