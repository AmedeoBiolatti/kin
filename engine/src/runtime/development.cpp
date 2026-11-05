#include <kin/runtime/development.hpp>

#include "runtime_internal.hpp"

#include <kin/core/hash.hpp>
#include <kin/core/json.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <exception>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {

constexpr std::size_t max_files = 4096;
constexpr std::size_t max_bytes = 64 * 1024 * 1024;
constexpr std::size_t max_rpc_edit_bytes = 4 * 1024 * 1024;
constexpr auto disk_poll_interval = std::chrono::milliseconds(150);
constexpr std::string_view publishing_marker = ".kin-import-publishing";
constexpr std::string_view import_report = ".kin-import-report.json";

std::string digest(std::string_view bytes) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash_bytes(bytes.data(), bytes.size());
    return out.str();
}

// A normalized relative content path with no hidden ('.'-prefixed) or empty part.
bool development_path(const std::string& name) {
    if (name.empty() || name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos) {
        return false;
    }
    const std::filesystem::path path{name};
    if (path.is_absolute() || path.lexically_normal().generic_string() != name) {
        return false;
    }
    return std::ranges::none_of(
        path, [](const std::filesystem::path& part) { return part.empty() || part.string().starts_with('.'); });
}

struct SourceFile {
    std::string name;
    std::filesystem::path path;
    u64 bytes = 0;
    std::filesystem::file_time_type time{};
};

// The content folder's files, without reading them: hidden files and folders
// are left out, symlinks refused.
std::vector<SourceFile> list_sources(const std::filesystem::path& source) {
    if (!std::filesystem::is_directory(source)) {
        throw std::runtime_error("development requires a loose content directory");
    }
    std::vector<SourceFile> files;
    for (auto it = std::filesystem::recursive_directory_iterator(source);
         it != std::filesystem::recursive_directory_iterator{}; ++it) {
        std::string name = it->path().lexically_relative(source).generic_string();
        if (!development_path(name)) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_symlink()) {
            throw std::runtime_error("development content cannot contain symlinks: " + name);
        }
        if (!it->is_regular_file()) {
            continue;
        }
        if (files.size() >= max_files) {
            throw std::runtime_error("too many development files");
        }
        files.push_back({.name = std::move(name),
                         .path = it->path(),
                         .bytes = static_cast<u64>(it->file_size()),
                         .time = it->last_write_time()});
    }
    std::ranges::sort(files, {}, &SourceFile::name);
    return files;
}

void add_stamp(std::string& stamps, std::string_view name, u64 bytes, std::filesystem::file_time_type time) {
    stamps.append(name);
    // The clock's count is wider than long long in libc++ (__int128), which to_string has no overload for.
    const auto ticks = static_cast<long long>(time.time_since_epoch().count());
    stamps += '\0' + std::to_string(bytes) + '\0' + std::to_string(ticks) + '\n';
}

// What the watcher compares, without reading a file: each file's size and
// time, and the importer's publication marker and report.
std::string folder_stamps(const std::filesystem::path& source, const std::vector<SourceFile>& files) {
    std::string stamps;
    if (std::filesystem::exists(source / publishing_marker)) {
        add_stamp(stamps, publishing_marker, 0, {});
    }
    const auto report = source / import_report;
    if (std::filesystem::exists(report)) {
        add_stamp(stamps, import_report, static_cast<u64>(std::filesystem::file_size(report)),
                  std::filesystem::last_write_time(report));
    }
    for (const SourceFile& file : files) {
        add_stamp(stamps, file.name, file.bytes, file.time);
    }
    return stamps;
}

std::string read_source(const std::filesystem::path& file) {
    auto bytes = read_content_file(file);
    if (!bytes) {
        throw std::runtime_error("cannot snapshot " + file.string());
    }
    return std::move(*bytes);
}

std::string read_report(const std::filesystem::path& source) {
    const auto report = source / import_report;
    return std::filesystem::exists(report) ? read_source(report) : std::string{};
}

} // namespace

DevelopmentSession::Snapshot DevelopmentSession::snapshot(MemoryContent::Files files) {
    if (files.size() > max_files) {
        throw std::runtime_error("development snapshot exceeds 4096 files");
    }
    std::size_t bytes = 0;
    std::string inventory;
    std::vector<std::string> hashes;
    hashes.reserve(files.size());
    for (const auto& [path, data] : files) {
        if (!development_path(path)) {
            throw std::runtime_error("invalid development path: " + path);
        }
        bytes += data.size();
        if (bytes > max_bytes) {
            throw std::runtime_error("development snapshot exceeds 64 MiB");
        }
        hashes.push_back(digest(data));
        inventory += path + '\0' + std::to_string(data.size()) + '\0' + hashes.back() + '\n';
    }
    return {.revision = {.content = MemoryContent::create(std::move(files)), .id = digest(inventory)},
            .hashes = std::move(hashes),
            .stamps = {}};
}

DevelopmentSession::Snapshot DevelopmentSession::capture() const {
    const auto publishing = _source / publishing_marker;
    if (std::filesystem::exists(publishing)) {
        throw std::runtime_error("asset import publication is pending");
    }
    const std::string report_before = read_report(_source);
    const std::vector<SourceFile> sources = list_sources(_source);
    std::string stamps = folder_stamps(_source, sources);
    MemoryContent::Files files;
    std::size_t bytes = 0;
    for (const SourceFile& file : sources) {
        bytes += file.bytes;
        if (bytes > max_bytes) {
            throw std::runtime_error("development snapshot exceeds 64 MiB");
        }
        files.emplace(file.name, read_source(file.path));
    }
    if (std::filesystem::exists(publishing) || read_report(_source) != report_before) {
        throw std::runtime_error("asset import changed during snapshot; retry after publication");
    }
    Snapshot result = snapshot(std::move(files));
    result.stamps = std::move(stamps);
    return result;
}

DevelopmentSession::DevelopmentSession(std::filesystem::path source)
    : _source(std::filesystem::absolute(source).lexically_normal()), _active(capture()),
      _disk_seen(_active.revision.id), _settled_stamps(_active.stamps) {}

void DevelopmentSession::add_target(std::string name, Prepare prepare) {
    if (name.empty() || !prepare ||
        std::ranges::any_of(_targets, [&](const auto& target) { return target.first == name; })) {
        throw std::invalid_argument("duplicate/empty development target");
    }
    _targets.emplace_back(std::move(name), std::move(prepare));
}

bool DevelopmentSession::queue(const std::function<Snapshot()>& make, std::string& error) {
    if (pending()) {
        error = "a development reload is already pending";
        return false;
    }
    try {
        _pending = make();
        _status = "queued";
        _error.clear();
        return true;
    } catch (const std::exception& e) {
        error = _error = e.what();
        _status = "rejected";
        return false;
    }
}

bool DevelopmentSession::request(std::string& error) {
    return queue(
        [&] {
            Snapshot next = capture();
            _disk_seen = next.revision.id;
            _settled_stamps = next.stamps;
            return next;
        },
        error);
}

bool DevelopmentSession::request(const JsonValue& changes, std::string& error) {
    return queue(
        [&] {
            if (!changes.is_object()) {
                throw std::runtime_error("reload changes must be an object");
            }
            MemoryContent::Files files = _active.revision.content->files();
            for (const auto& [name, value] : changes.members()) {
                if (!development_path(name)) {
                    throw std::runtime_error("reload path must be a normalized relative content path");
                }
                const JsonValue* hex = value.is_object() && value.members().size() == 1 ? value.find("hex") : nullptr;
                if (value.is_null()) {
                    files.erase(name);
                } else if (value.is_string()) {
                    files[name] = value.as_string();
                } else if (hex && hex->is_string()) {
                    auto bytes = hex->as_string().size() <= max_bytes * 2 ? runtime_detail::hex_decode(hex->as_string())
                                                                          : std::nullopt;
                    if (!bytes) {
                        throw std::runtime_error("invalid reload hex");
                    }
                    files[name] = std::move(*bytes);
                } else {
                    throw std::runtime_error("reload edit must be text, {hex:string}, or null");
                }
            }
            return snapshot(std::move(files));
        },
        error);
}

void DevelopmentSession::reject_request(std::string error) {
    _pending = {};
    _status = "rejected";
    _error = std::move(error);
}

void DevelopmentSession::apply(u64 tick) {
    ++_attempt;
    _last_attempted = _pending.revision.id;
    std::vector<std::unique_ptr<DevelopmentChange>> candidates;
    std::size_t activated = 0;
    try {
        for (const auto& [name, prepare] : _targets) {
            std::string error;
            auto candidate = prepare(_pending.revision, error);
            if (!candidate || !candidate->activate || !candidate->rollback) {
                throw std::runtime_error(name + ": " + (error.empty() ? "invalid reload candidate" : error));
            }
            candidates.push_back(std::move(candidate));
        }
        for (auto& candidate : candidates) {
            ++activated;
            std::string error;
            if (!candidate->activate(error)) {
                throw std::runtime_error(error.empty() ? "reload activation failed" : error);
            }
        }
    } catch (const std::exception& e) {
        while (activated) {
            try {
                candidates[--activated]->rollback();
            } catch (...) {
                std::terminate();
            }
        }
        // Errors name the snapshot's virtual files; show the folder's instead.
        _error = e.what();
        const std::string root = _pending.revision.content->root().string();
        const std::string source = _source.string();
        for (std::size_t at = 0; (at = _error.find(root, at)) != std::string::npos; at += source.size()) {
            _error.replace(at, root.size(), source);
        }
        _pending = {};
        _status = "rejected";
        KIN_LOG_WARN("development", _error);
        return;
    }
    for (auto& candidate : candidates) {
        if (candidate->finalize) {
            try {
                candidate->finalize();
            } catch (...) {
                std::terminate();
            }
        }
    }
    _active = std::move(_pending);
    _pending = {};
    ++_generation;
    _activation_tick = tick;
    _status = "applied";
    _error.clear();
    KIN_LOG_INFO("development", "activated content revision " + _active.revision.id);
}

void DevelopmentSession::watch(u64 tick) {
    const auto now = std::chrono::steady_clock::now();
    if (now - _last_poll < disk_poll_interval) {
        return;
    }
    _last_poll = now;
    try {
        std::string stamps = folder_stamps(_source, list_sources(_source));
        if (stamps == _settled_stamps) {
            _changing_stamps.clear();
            return;
        }
        if (stamps != _changing_stamps) {
            _changing_stamps = std::move(stamps); // still being written: look again next time
            return;
        }
        _changing_stamps.clear();
        _settled_stamps = std::move(stamps); // read once, whether or not it can be
        Snapshot candidate = capture();
        if (candidate.revision.id == _disk_seen) {
            return; // touched, not changed
        }
        _disk_seen = candidate.revision.id;
        if (candidate.revision.id == _active.revision.id) {
            _status = "idle"; // back to what runs
            _error.clear();
            return;
        }
        if (candidate.revision.id == _last_attempted) {
            return; // the edit rejected last time
        }
        _pending = std::move(candidate);
        apply(tick);
    } catch (const std::exception& e) {
        _error = e.what();
        _status = "rejected";
    }
}

void DevelopmentSession::advance(u64 tick, bool watch_disk) {
    if (pending()) {
        apply(tick);
    } else if (watch_disk) {
        watch(tick);
    }
}

void DevelopmentSession::write_summary_fields(JsonWriter& json) const {
    json.field("generation", _generation)
        .field("attempt", _attempt)
        .field("pending", pending())
        .field("status", _status)
        .field("error", _error)
        .field("revision", _active.revision.id)
        .field("activation_tick", _activation_tick);
}

void DevelopmentSession::write_summary(JsonWriter& json) const {
    json.begin_object();
    write_summary_fields(json);
    json.end_object();
}

void DevelopmentSession::write_status(JsonWriter& json) const {
    json.begin_object();
    write_summary_fields(json);
    json.key("sources").begin_array();
    std::size_t index = 0;
    for (const auto& [path, bytes] : _active.revision.content->files()) {
        json.begin_object()
            .field("path", path)
            .field("hash", _active.hashes[index++])
            .field("bytes", static_cast<u64>(bytes.size()))
            .end_object();
    }
    json.end_array();
    json.key("targets").begin_array();
    for (const auto& [name, prepare] : _targets) {
        json.value(name);
    }
    json.end_array().end_object();
}

JsonValue DevelopmentSession::pending_changes() const {
    JsonValue::Object changes;
    if (!pending()) {
        return JsonValue{std::move(changes)};
    }
    const auto& before = _active.revision.content->files();
    const auto& after = _pending.revision.content->files();
    std::size_t bytes = 0;
    for (const auto& [path, data] : after) {
        const auto old = before.find(path);
        if (old != before.end() && old->second == data) {
            continue;
        }
        bytes += data.size();
        if (bytes > max_rpc_edit_bytes) {
            throw std::runtime_error("RPC reload edits exceed 4 MiB; use disk development watching for larger changes");
        }
        changes[path] = JsonValue{JsonValue::Object{{"hex", JsonValue{runtime_detail::hex_encode(data)}}}};
    }
    for (const auto& [path, data] : before) {
        if (!after.contains(path)) {
            changes[path] = JsonValue{};
        }
    }
    return JsonValue{std::move(changes)};
}

} // namespace kin
