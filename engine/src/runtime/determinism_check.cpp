#include "runtime_internal.hpp"

#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/log.hpp>
#include <kin/platform/process.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/runtime/state_hash.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace kin::runtime_detail {
namespace {

// Lines of the lockstep protocol start with this; anything else a child prints
// (the game's own output) is ignored.
constexpr std::string_view marker = "#kin-state ";
constexpr std::chrono::seconds reply_timeout{120};
constexpr i32 default_frames = 300;
constexpr std::size_t max_differences = 20;
constexpr std::size_t max_value_bytes = 256;      // shown of a component
constexpr std::size_t max_report_bytes = 64 * 1024; // shown of a scene's report

std::size_t shown_bytes(const StateItem& item) {
    return std::min(item.bytes.size(), item.component == "(report)" ? max_report_bytes : max_value_bytes);
}

constexpr char hex_digits[] = "0123456789abcdef";

std::string to_hex(const u8* data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(hex_digits[data[i] >> 4]);
        out.push_back(hex_digits[data[i] & 15]);
    }
    return out;
}

std::vector<u8> from_hex(std::string_view text) {
    std::vector<u8> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        const auto digit = [](char c) -> u8 {
            return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        out.push_back(static_cast<u8>(digit(text[i]) << 4 | digit(text[i + 1])));
    }
    return out;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            fields.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return fields;
}

// Names and values can hold anything; tabs and newlines would break a line.
std::string clean(std::string_view text) {
    std::string out{text};
    std::replace_if(out.begin(), out.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return out;
}

template<typename T>
bool parse_number(std::string_view text, T& value, int base = 10) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// The child's command line: the game's own, run headless for the frames to
// compare, without the options that make it do something else.
std::vector<std::string> child_args(const HeadlessOptions& options, i32 frames) {
    std::vector<std::string> args{options.args.front()};
    constexpr std::string_view dropped_with_value[] = {"--frames", "--report", "--profile-json", "--profile-text",
                                                       "--log-level", "--max-fps"};
    constexpr std::string_view dropped_prefixes[] = {"--check-determinism", "--report", "--probe-", "--profile",
                                                     "--frames", "--server", "--port=", "--log-level",
                                                     "--max-fps"};
    constexpr std::string_view dropped[] = {"--headless", "--state-lockstep", "--list-actions", "--game-info",
                                            "--list-info"};
    for (std::size_t i = 1; i < options.args.size(); ++i) {
        const std::string_view arg = options.args[i];
        if (std::ranges::find(dropped_with_value, arg) != std::end(dropped_with_value)) {
            ++i;
            continue;
        }
        if (std::ranges::find(dropped, arg) != std::end(dropped) ||
            std::ranges::any_of(dropped_prefixes, [&](std::string_view prefix) { return arg.starts_with(prefix); })) {
            continue;
        }
        args.emplace_back(arg);
    }
    args.emplace_back("--headless");
    args.emplace_back("--state-lockstep");
    args.push_back("--frames=" + std::to_string(frames));
    args.emplace_back("--log-level=error");
    return args;
}

struct Variant {
    std::string name;
    std::vector<std::pair<std::string, std::string>> environment;
};

struct Difference {
    StateItem baseline; // an empty component name: not in that run
    StateItem run;
};

struct RunResult {
    std::string status = "ok"; // ok, diverged, error
    i32 frames = 0;
    i32 first_frame = 0;
    std::string reason;
    std::vector<Difference> differences;
    std::size_t total_differences = 0;
};

class Child {
public:
    bool start(const std::vector<std::string>& args, const Variant& variant) {
        return _process.start({.args = args, .errors_to_output = false, .environment = variant.environment});
    }
    const std::string& error() const { return _process.error(); }

    void send(std::string_view command) {
        _process.write(std::string{command} + "\n");
        _process.pump();
    }

    // The next protocol line, without the marker; empty once the child has
    // ended (or stopped answering).
    std::optional<std::string> next() {
        const auto deadline = std::chrono::steady_clock::now() + reply_timeout;
        while (true) {
            while (std::optional<std::string> line = _process.read_line()) {
                if (line->starts_with(marker)) {
                    return line->substr(marker.size());
                }
            }
            if (!_process.running()) {
                // Lines that arrived as it ended.
                while (std::optional<std::string> line = _process.read_line()) {
                    if (line->starts_with(marker)) {
                        return line->substr(marker.size());
                    }
                }
                return std::nullopt;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                _timed_out = true;
                _process.kill(true);
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    // The next frame line: (frame, hash). Coverage lines on the way are kept.
    std::optional<std::pair<i32, u64>> next_frame() {
        while (std::optional<std::string> line = next()) {
            const std::vector<std::string_view> fields = split_tabs(*line);
            if (fields.size() == 3 && fields[0] == "frame") {
                i32 frame = 0;
                u64 hash = 0;
                if (parse_number(fields[1], frame) && parse_number(fields[2], hash, 16)) {
                    return std::pair{frame, hash};
                }
            } else if (fields.size() >= 4 && fields[0] == "coverage") {
                take_coverage(fields);
            }
        }
        return std::nullopt;
    }

    std::vector<StateItem> detail() {
        send("d");
        std::vector<StateItem> items;
        while (std::optional<std::string> line = next()) {
            const std::vector<std::string_view> fields = split_tabs(*line);
            if (fields.size() == 1 && fields[0] == "end") {
                break;
            }
            if (fields.size() != 8 || fields[0] != "item") {
                continue;
            }
            StateItem item;
            parse_number(fields[1], item.scene);
            item.scene_name = fields[2];
            parse_number(fields[3], item.entity);
            item.entity_name = fields[4];
            item.component = fields[5];
            parse_number(fields[6], item.hash, 16);
            item.bytes = from_hex(fields[7]);
            items.push_back(std::move(item));
        }
        return items;
    }

    // Tells it to stop and waits for it, reading its last coverage line.
    void finish() {
        if (_process.running()) {
            send("q");
        }
        while (next_frame()) {
        }
        _process.wait(std::chrono::seconds(10));
        if (_process.running()) {
            _process.kill(true);
        }
    }

    bool timed_out() const { return _timed_out; }
    std::optional<i64> exit_code() const { return _process.exit_code(); }
    const StateCoverage& coverage() const { return _coverage; }

private:
    void take_coverage(const std::vector<std::string_view>& fields) {
        parse_number(fields[1], _coverage.scenes);
        parse_number(fields[2], _coverage.entities);
        parse_number(fields[3], _coverage.values);
        _coverage.not_compared.clear();
        for (std::size_t i = 4; i < fields.size(); ++i) {
            _coverage.not_compared.emplace_back(fields[i]);
        }
    }

    Process _process;
    StateCoverage _coverage;
    bool _timed_out = false;
};

std::vector<Difference> compare_items(const std::vector<StateItem>& baseline, const std::vector<StateItem>& run,
                                      std::size_t& total) {
    using Key = std::tuple<i32, u64, std::string>;
    std::map<Key, const StateItem*> before;
    for (const StateItem& item : baseline) {
        before[{item.scene, item.entity, item.component}] = &item;
    }
    std::map<Key, Difference> differences;
    for (const StateItem& item : run) {
        const Key key{item.scene, item.entity, item.component};
        const auto found = before.find(key);
        if (found == before.end()) {
            differences[key] = {.run = item};
        } else {
            if (found->second->hash != item.hash) {
                differences[key] = {.baseline = *found->second, .run = item};
            }
            before.erase(found);
        }
    }
    for (const auto& [key, item] : before) {
        differences[key] = {.baseline = *item};
    }
    total = differences.size();
    std::vector<Difference> out;
    for (auto& [key, difference] : differences) {
        if (out.size() == max_differences) {
            break;
        }
        out.push_back(std::move(difference));
    }
    return out;
}

struct FieldDifference {
    std::string path;
    const JsonValue* baseline = nullptr; // null: missing
    const JsonValue* run = nullptr;
};

bool same_leaf(const JsonValue& a, const JsonValue& b) {
    if (a.type() != b.type()) {
        return false;
    }
    switch (a.type()) {
    case JsonValue::Type::Null:
        return true;
    case JsonValue::Type::Bool:
        return a.as_bool() == b.as_bool();
    case JsonValue::Type::Number:
        return a.as_number() == b.as_number();
    case JsonValue::Type::String:
        return a.as_string() == b.as_string();
    case JsonValue::Type::Array:
    case JsonValue::Type::Object:
        return true;
    }
    return true;
}

// The paths (".systems[0].last_duration_ms") where two reports differ.
void diff_json(const JsonValue* a, const JsonValue* b, const std::string& path, std::vector<FieldDifference>& out) {
    if (out.size() >= max_differences) {
        return;
    }
    if (!a || !b || a->type() != b->type() || !same_leaf(*a, *b)) {
        out.push_back({path.empty() ? "." : path, a, b});
        return;
    }
    if (a->is_object()) {
        for (const auto& [key, value] : a->members()) {
            const auto other = b->members().find(key);
            diff_json(&value, other == b->members().end() ? nullptr : &other->second, path + "." + key, out);
        }
        for (const auto& [key, value] : b->members()) {
            if (!a->members().contains(key)) {
                diff_json(nullptr, &value, path + "." + key, out);
            }
        }
    } else if (a->is_array()) {
        const std::size_t count = std::max(a->items().size(), b->items().size());
        for (std::size_t i = 0; i < count; ++i) {
            diff_json(i < a->items().size() ? &a->items()[i] : nullptr, i < b->items().size() ? &b->items()[i] : nullptr,
                      path + "[" + std::to_string(i) + "]", out);
        }
    }
}

std::string_view text_of(const StateItem& item) {
    return {reinterpret_cast<const char*>(item.bytes.data()), item.bytes.size()};
}

void write_value(JsonWriter& json, const StateItem& item) {
    json.begin_object();
    if (item.component == "(report)") {
        json.field("json", text_of(item));
    } else {
        json.field("hex", to_hex(item.bytes.data(), shown_bytes(item)));
        if (item.component != "(type)" && item.bytes.size() % 4 == 0 && item.bytes.size() <= 64) {
            // Most game data is floats; the same bytes read as f32s.
            json.key("as_f32").begin_array();
            for (std::size_t i = 0; i < item.bytes.size(); i += 4) {
                f32 value = 0.0f;
                std::memcpy(&value, item.bytes.data() + i, 4);
                json.value(static_cast<f64>(value));
            }
            json.end_array();
        }
    }
    json.end_object();
}

void write_difference(JsonWriter& json, const Difference& difference) {
    const StateItem& any = difference.run.component.empty() ? difference.baseline : difference.run;
    json.begin_object();
    json.field("scene", any.scene_name);
    if (any.entity != 0) {
        json.field("entity", any.entity);
        json.field("name", any.entity_name);
    }
    json.field("component", any.component);
    if (difference.baseline.component.empty()) {
        json.field("change", "only in this run");
    } else if (difference.run.component.empty()) {
        json.field("change", "only in the baseline");
    } else {
        json.field("change", "value");
    }
    // Reports are JSON: name the fields that differ rather than show both whole.
    if (any.component == "(report)" && !difference.baseline.component.empty() && !difference.run.component.empty()) {
        const JsonParseResult baseline = parse_json(text_of(difference.baseline));
        const JsonParseResult run = parse_json(text_of(difference.run));
        if (baseline.ok() && run.ok()) {
            std::vector<FieldDifference> fields;
            diff_json(&*baseline.value, &*run.value, "", fields);
            json.key("fields").begin_array();
            for (const FieldDifference& field : fields) {
                json.begin_object();
                json.field("path", field.path);
                json.key("baseline");
                field.baseline ? write_json(json, *field.baseline) : static_cast<void>(json.value_null());
                json.key("run");
                field.run ? write_json(json, *field.run) : static_cast<void>(json.value_null());
                json.end_object();
            }
            json.end_array();
            json.end_object();
            return;
        }
    }
    if (!difference.baseline.component.empty()) {
        json.key("baseline");
        write_value(json, difference.baseline);
    }
    if (!difference.run.component.empty()) {
        json.key("run");
        write_value(json, difference.run);
    }
    json.end_object();
}

void write_report(std::ostream& out,
                  const std::vector<Variant>& variants,
                  const std::vector<RunResult>& results,
                  const std::vector<std::string>& args,
                  const StateCoverage& coverage,
                  i32 frames,
                  u64 seed,
                  const std::string& error) {
    const bool diverged = std::ranges::any_of(results, [](const RunResult& r) { return r.status == "diverged"; });
    const bool failed = !error.empty() || std::ranges::any_of(results, [](const RunResult& r) { return r.status == "error"; });
    JsonWriter json(out);
    json.begin_object();
    json.field("schema", "kin.determinism/1");
    json.field("status", failed ? "error" : diverged ? "diverged" : "ok");
    if (!error.empty()) {
        json.field("error", error);
    }
    json.field("frames", frames);
    json.field("seed", seed);
    json.key("command").begin_array();
    for (const std::string& arg : args) {
        json.value(arg);
    }
    json.end_array();
    json.key("coverage").begin_object();
    json.field("scenes", coverage.scenes);
    json.field("entities", coverage.entities);
    json.field("values", coverage.values);
    json.key("not_compared").begin_array();
    for (const std::string& name : coverage.not_compared) {
        json.value(name);
    }
    json.end_array();
    json.end_object();
    json.key("runs").begin_array();
    for (std::size_t i = 0; i < results.size(); ++i) {
        const RunResult& result = results[i];
        json.begin_object();
        json.field("name", variants[i + 1].name);
        json.key("environment").begin_object();
        for (const auto& [name, value] : variants[i + 1].environment) {
            json.field(name, value);
        }
        json.end_object();
        json.field("status", result.status);
        json.field("frames", result.frames);
        if (!result.reason.empty()) {
            json.field("reason", result.reason);
        }
        if (result.status == "diverged") {
            json.field("first_frame", result.first_frame);
            json.field("total_differences", static_cast<u64>(result.total_differences));
            json.key("differences").begin_array();
            for (const Difference& difference : result.differences) {
                write_difference(json, difference);
            }
            json.end_array();
        }
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

} // namespace

int run_determinism_check(const SceneAppConfig& config) {
    const HeadlessOptions& options = config.headless;
    const i32 frames = options.frames > 0 ? options.frames : default_frames;
    // The baseline, then the runs compared with it: the same again (anything
    // that differs between two runs of one build: uninitialized memory, clocks,
    // addresses), and with one job worker (anything that depends on how work
    // is split between threads).
    const std::vector<Variant> variants{
        {.name = "baseline"},
        {.name = "repeat"},
        {.name = "one worker", .environment = {{"KIN_JOB_WORKERS", "1"}}},
    };
    std::vector<RunResult> results(variants.size() - 1);
    std::vector<std::string> args;
    std::string error;
    StateCoverage coverage;

    if (options.args.empty()) {
        error = "no command line to run (HeadlessOptions::args is empty)";
    } else {
        args = child_args(options, frames);
        std::vector<Child> children(variants.size());
        for (std::size_t i = 0; i < children.size() && error.empty(); ++i) {
            if (!children[i].start(args, variants[i])) {
                error = children[i].error();
            }
        }
        // Which runs are still being compared with the baseline.
        std::vector<bool> active(variants.size(), error.empty());
        active[0] = false;
        while (error.empty() && std::ranges::count(active, true) > 0) {
            const std::optional<std::pair<i32, u64>> base = children[0].next_frame();
            std::optional<std::vector<StateItem>> base_items;
            for (std::size_t i = 1; i < children.size(); ++i) {
                if (!active[i]) {
                    continue;
                }
                RunResult& result = results[i - 1];
                const std::optional<std::pair<i32, u64>> mine = children[i].next_frame();
                if (!base || !mine) {
                    if (base || mine) {
                        result.status = "diverged";
                        result.first_frame = (base ? base->first : mine->first);
                        result.reason = base ? "this run ended before the baseline" : "this run went on after the baseline ended";
                    }
                    if (children[i].timed_out() || children[0].timed_out()) {
                        result.status = "error";
                        result.reason = "a run stopped answering";
                    }
                    active[i] = false;
                    continue;
                }
                result.frames = mine->first;
                if (*base == *mine) {
                    continue;
                }
                result.status = "diverged";
                result.first_frame = mine->first;
                if (!base_items) {
                    base_items = children[0].detail();
                }
                result.differences = compare_items(*base_items, children[i].detail(), result.total_differences);
                if (result.total_differences == 0) {
                    result.reason = "the states hash differently but every item matches (a hash collision?)";
                }
                active[i] = false;
                children[i].finish();
            }
            if (!base) {
                break;
            }
            for (std::size_t i = 0; i < children.size(); ++i) {
                if (i == 0 ? std::ranges::count(active, true) > 0 : static_cast<bool>(active[i])) {
                    children[i].send("n");
                }
            }
        }
        for (std::size_t i = 0; i < children.size(); ++i) {
            children[i].finish();
        }
        coverage = children[0].coverage();
        for (std::size_t i = 0; i < children.size() && error.empty(); ++i) {
            const std::optional<i64> code = children[i].exit_code();
            if (code && *code != 0 && i > 0 && results[i - 1].status == "ok") {
                results[i - 1].status = "error";
                results[i - 1].reason = "exited with code " + std::to_string(*code);
            }
        }
    }

    const auto emit = [&](std::ostream& out) {
        write_report(out, variants, results, args, coverage, frames, options.seed, error);
    };
    if (config.determinism_output) {
        emit(*config.determinism_output);
    } else if (options.determinism_path.empty() || options.determinism_path == "-") {
        emit(std::cout);
    } else {
        const std::filesystem::path parent = std::filesystem::path{options.determinism_path}.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        std::ofstream file(options.determinism_path);
        if (!file) {
            KIN_LOG_ERROR_F("runtime", "failed to open determinism report path",
                            (LogFields{{.name = "path", .value = options.determinism_path}}));
            return 1;
        }
        emit(file);
    }

    const bool ok = error.empty() && std::ranges::all_of(results, [](const RunResult& r) { return r.status == "ok"; });
    if (!error.empty()) {
        KIN_LOG_ERROR_F("runtime", "determinism check failed to run", (LogFields{{.name = "error", .value = error}}));
    }
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].status != "ok") {
            KIN_LOG_WARN_F("runtime", "determinism check: run differs from the baseline",
                           (LogFields{{.name = "run", .value = variants[i + 1].name},
                                      {.name = "status", .value = results[i].status},
                                      {.name = "frame", .value = std::to_string(results[i].first_frame)}}));
        }
    }
    return ok ? 0 : 1;
}

bool state_lockstep_step(SceneManager& scenes, i32 frame, StateCoverage& coverage) {
    StateCoverage now;
    const u64 hash = hash_state(scenes, &now);
    coverage.scenes = std::max(coverage.scenes, now.scenes);
    coverage.entities = std::max(coverage.entities, now.entities);
    coverage.values = std::max(coverage.values, now.values);
    for (std::string& name : now.not_compared) {
        if (std::ranges::find(coverage.not_compared, name) == coverage.not_compared.end()) {
            coverage.not_compared.push_back(std::move(name));
        }
    }
    std::cout << marker << "frame\t" << frame << '\t' << std::hex << hash << std::dec << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "n") {
            return true;
        }
        if (command == "q") {
            return false;
        }
        if (command == "d") {
            for (const StateItem& item : describe_state(scenes)) {
                std::cout << marker << "item\t" << item.scene << '\t' << clean(item.scene_name) << '\t' << item.entity << '\t'
                          << clean(item.entity_name) << '\t' << clean(item.component) << '\t' << std::hex << item.hash
                          << std::dec << '\t' << to_hex(item.bytes.data(), shown_bytes(item))
                          << '\n';
            }
            std::cout << marker << "end" << std::endl;
        }
    }
    return false; // the parent is gone
}

void state_lockstep_finish(const StateCoverage& coverage) {
    std::vector<std::string> names = coverage.not_compared;
    std::sort(names.begin(), names.end());
    std::cout << marker << "coverage\t" << coverage.scenes << '\t' << coverage.entities << '\t' << coverage.values;
    for (const std::string& name : names) {
        std::cout << '\t' << clean(name);
    }
    std::cout << std::endl;
}

} // namespace kin::runtime_detail
