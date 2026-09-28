#pragma once

#include <kin/core/types.hpp>

#include <chrono>
#include <iosfwd>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct ProfileEvent {
    std::string name;
    std::string category;
    std::string file;
    std::string function;
    u32 line = 0;
    u64 elapsed_ns = 0;
    u64 thread_id = 0;
    i64 frame = 0;
};

struct ProfileSummary {
    std::string name;
    std::string category;
    std::string file;
    std::string function;
    u32 line = 0;
    u64 calls = 0;
    u64 total_ns = 0;
    u64 min_ns = 0;
    u64 max_ns = 0;
    f64 mean_ms = 0.0;
    f64 median_ms = 0.0;
    f64 p95_ms = 0.0;
    f64 p99_ms = 0.0;
};

class ProfileSession {
public:
    void start(std::string_view name = {});
    void stop();
    void clear();

    bool active() const { return _active; }
    std::string_view name() const { return _name; }

    void set_frame(i64 frame) { _frame = frame; }
    i64 frame() const { return _frame; }

    void record(std::string_view name,
                std::string_view category,
                u64 elapsed_ns,
                std::source_location location = std::source_location::current());

    const std::vector<ProfileEvent>& events() const { return _events; }
    std::vector<ProfileSummary> summarize() const;

private:
    std::string _name;
    bool _active = false;
    i64 _frame = 0;
    std::vector<ProfileEvent> _events;
};

class ScopedProfile {
public:
    ScopedProfile(ProfileSession* session,
                  std::string_view name,
                  std::string_view category,
                  std::source_location location = std::source_location::current());
    ~ScopedProfile();

    ScopedProfile(const ScopedProfile&) = delete;
    ScopedProfile& operator=(const ScopedProfile&) = delete;
    ScopedProfile(ScopedProfile&&) = delete;
    ScopedProfile& operator=(ScopedProfile&&) = delete;

private:
    using Clock = std::chrono::steady_clock;

    ProfileSession* _session = nullptr;
    std::string_view _name;
    std::string_view _category;
    std::source_location _location;
    Clock::time_point _start;
};

void set_current_profile_session(ProfileSession* session);
ProfileSession* current_profile_session();

void write_profile_text(std::ostream& out,
                        const ProfileSession& session,
                        i32 max_rows = 32,
                        std::string_view title = "profile");
void write_profile_json(std::ostream& out,
                        const ProfileSession& session,
                        std::string_view schema,
                        i32 max_summary_rows = 128);

} // namespace kin

#if defined(KIN_ENABLE_PROFILING)
#define KIN_PROFILE_JOIN_DETAIL(a, b) a##b
#define KIN_PROFILE_JOIN(a, b) KIN_PROFILE_JOIN_DETAIL(a, b)
#define KIN_PROFILE_SCOPE(name) \
    ::kin::ScopedProfile KIN_PROFILE_JOIN(_kin_profile_scope_, __LINE__)( \
        ::kin::current_profile_session(), (name), "scope", std::source_location::current())
#define KIN_PROFILE_FUNCTION() \
    ::kin::ScopedProfile KIN_PROFILE_JOIN(_kin_profile_function_, __LINE__)( \
        ::kin::current_profile_session(), std::source_location::current().function_name(), "function", std::source_location::current())
#define KIN_PROFILE_LINE(name) \
    ::kin::ScopedProfile KIN_PROFILE_JOIN(_kin_profile_line_, __LINE__)( \
        ::kin::current_profile_session(), (name), "line", std::source_location::current())
#else
#define KIN_PROFILE_SCOPE(name) ((void)0)
#define KIN_PROFILE_FUNCTION() ((void)0)
#define KIN_PROFILE_LINE(name) ((void)0)
#endif
