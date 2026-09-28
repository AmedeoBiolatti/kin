#include <kin/core/profile.hpp>

#include <kin/core/json.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <thread>
#include <unordered_map>

namespace kin {
namespace {

thread_local ProfileSession* current_session = nullptr;

u64 thread_id() {
    return static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

f64 ns_to_ms(u64 ns) {
    return static_cast<f64>(ns) / 1'000'000.0;
}

u64 percentile_ns(std::vector<u64> samples, f64 percent) {
    if (samples.empty()) {
        return 0;
    }
    std::ranges::sort(samples);
    const f64 raw = static_cast<f64>(samples.size() - 1) * percent;
    const auto index = static_cast<std::size_t>(std::clamp(std::ceil(raw), 0.0, static_cast<f64>(samples.size() - 1)));
    return samples[index];
}

std::string key_for(const ProfileEvent& event) {
    std::string key;
    key.reserve(event.name.size() + event.category.size() + event.file.size() + event.function.size() + 32);
    key += event.category;
    key.push_back('\x1f');
    key += event.name;
    key.push_back('\x1f');
    key += event.file;
    key.push_back('\x1f');
    key += event.function;
    key.push_back('\x1f');
    key += std::to_string(event.line);
    return key;
}

} // namespace

void ProfileSession::start(std::string_view session_name) {
    _name = std::string{session_name};
    _active = true;
}

void ProfileSession::stop() {
    _active = false;
}

void ProfileSession::clear() {
    _events.clear();
    _frame = 0;
}

void ProfileSession::record(std::string_view event_name,
                            std::string_view category,
                            u64 elapsed_ns,
                            std::source_location location) {
    if (!_active) {
        return;
    }

    _events.push_back({
        .name = std::string{event_name},
        .category = std::string{category},
        .file = location.file_name(),
        .function = location.function_name(),
        .line = location.line(),
        .elapsed_ns = elapsed_ns,
        .thread_id = thread_id(),
        .frame = _frame,
    });
}

std::vector<ProfileSummary> ProfileSession::summarize() const {
    struct Bucket {
        ProfileSummary summary;
        std::vector<u64> samples;
    };

    std::unordered_map<std::string, Bucket> buckets;
    for (const ProfileEvent& event : _events) {
        Bucket& bucket = buckets[key_for(event)];
        if (bucket.summary.calls == 0) {
            bucket.summary.name = event.name;
            bucket.summary.category = event.category;
            bucket.summary.file = event.file;
            bucket.summary.function = event.function;
            bucket.summary.line = event.line;
            bucket.summary.min_ns = event.elapsed_ns;
            bucket.summary.max_ns = event.elapsed_ns;
        }
        ++bucket.summary.calls;
        bucket.summary.total_ns += event.elapsed_ns;
        bucket.summary.min_ns = std::min(bucket.summary.min_ns, event.elapsed_ns);
        bucket.summary.max_ns = std::max(bucket.summary.max_ns, event.elapsed_ns);
        bucket.samples.push_back(event.elapsed_ns);
    }

    std::vector<ProfileSummary> result;
    result.reserve(buckets.size());
    for (auto& item : buckets) {
        Bucket& bucket = item.second;
        ProfileSummary& summary = bucket.summary;
        summary.mean_ms = summary.calls > 0
            ? static_cast<f64>(summary.total_ns) / static_cast<f64>(summary.calls) / 1'000'000.0
            : 0.0;
        summary.median_ms = ns_to_ms(percentile_ns(bucket.samples, 0.50));
        summary.p95_ms = ns_to_ms(percentile_ns(bucket.samples, 0.95));
        summary.p99_ms = ns_to_ms(percentile_ns(bucket.samples, 0.99));
        result.push_back(summary);
    }

    std::ranges::sort(result, [](const ProfileSummary& a, const ProfileSummary& b) {
        if (a.total_ns != b.total_ns) {
            return a.total_ns > b.total_ns;
        }
        return a.name < b.name;
    });
    return result;
}

ScopedProfile::ScopedProfile(ProfileSession* session,
                             std::string_view name,
                             std::string_view category,
                             std::source_location location)
    : _session(session),
      _name(name),
      _category(category),
      _location(location),
      _start(Clock::now()) {
}

ScopedProfile::~ScopedProfile() {
    if (!_session || !_session->active()) {
        return;
    }

    const auto end = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - _start).count();
    _session->record(_name, _category, static_cast<u64>(std::max<i64>(elapsed, 0)), _location);
}

void set_current_profile_session(ProfileSession* session) {
    current_session = session;
}

ProfileSession* current_profile_session() {
    return current_session;
}

void write_profile_text(std::ostream& out, const ProfileSession& session, i32 max_rows, std::string_view title) {
    const std::vector<ProfileSummary> summaries = session.summarize();
    out << title;
    if (!session.name().empty()) {
        out << " " << session.name();
    }
    out << " events=" << session.events().size() << " groups=" << summaries.size() << '\n';
    out << std::left << std::setw(28) << "name"
        << std::right << std::setw(8) << "calls"
        << std::setw(11) << "total"
        << std::setw(11) << "median"
        << std::setw(11) << "p95"
        << std::setw(11) << "p99"
        << "  source\n";

    const i32 rows = max_rows > 0 ? std::min<i32>(max_rows, static_cast<i32>(summaries.size()))
                                  : static_cast<i32>(summaries.size());
    for (i32 i = 0; i < rows; ++i) {
        const ProfileSummary& row = summaries[static_cast<std::size_t>(i)];
        out << std::left << std::setw(28) << row.name.substr(0, 27)
            << std::right << std::setw(8) << row.calls
            << std::setw(10) << std::fixed << std::setprecision(3) << ns_to_ms(row.total_ns) << " "
            << std::setw(10) << row.median_ms << " "
            << std::setw(10) << row.p95_ms << " "
            << std::setw(10) << row.p99_ms << "  "
            << row.file << ":" << row.line << '\n';
    }
}

void write_profile_json(std::ostream& out,
                        const ProfileSession& session,
                        std::string_view schema,
                        i32 max_summary_rows) {
    const std::vector<ProfileSummary> summaries = session.summarize();
    JsonWriter json(out);
    json.begin_object();
    json.field("schema", schema);
    json.field("name", session.name());
    json.field("events", static_cast<u64>(session.events().size()));
    json.key("summary").begin_array();
    const i32 rows = max_summary_rows > 0 ? std::min<i32>(max_summary_rows, static_cast<i32>(summaries.size()))
                                          : static_cast<i32>(summaries.size());
    for (i32 i = 0; i < rows; ++i) {
        const ProfileSummary& row = summaries[static_cast<std::size_t>(i)];
        json.begin_object();
        json.field("name", row.name);
        json.field("category", row.category);
        json.field("file", row.file);
        json.field("line", row.line);
        json.field("function", row.function);
        json.field("calls", row.calls);
        json.field("total_ns", row.total_ns);
        json.field("min_ns", row.min_ns);
        json.field("max_ns", row.max_ns);
        json.field("mean_ms", row.mean_ms);
        json.field("median_ms", row.median_ms);
        json.field("p95_ms", row.p95_ms);
        json.field("p99_ms", row.p99_ms);
        json.end_object();
    }
    json.end_array();
    json.key("samples").begin_array();
    for (const ProfileEvent& event : session.events()) {
        json.begin_object();
        json.field("name", event.name);
        json.field("category", event.category);
        json.field("file", event.file);
        json.field("line", event.line);
        json.field("function", event.function);
        json.field("elapsed_ns", event.elapsed_ns);
        json.field("thread_id", event.thread_id);
        json.field("frame", event.frame);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

} // namespace kin
