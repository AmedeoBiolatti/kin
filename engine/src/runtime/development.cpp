#include <algorithm>
#include <fstream>
#include <iomanip>
#include <kin/core/hash.hpp>
#include <kin/core/json.hpp>
#include <kin/platform/log.hpp>
#include <kin/runtime/development.hpp>
#include <sstream>
#include <stdexcept>
namespace kin {
namespace {
constexpr std::size_t max_files = 4096, max_bytes = 64 * 1024 * 1024;
std::string digest(std::string_view bytes) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash_bytes(bytes.data(), bytes.size());
    return out.str();
}
bool relative_path(const std::string& name) {
    if (name.empty() || name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos)
        return false;
    const std::filesystem::path path{name};
    if (path.is_absolute() || path.lexically_normal().generic_string() != name)
        return false;
    for (const auto& part : path)
        if (part == ".." || part == "." || part.empty() || part.string().starts_with('.'))
            return false;
    return true;
}
std::string read(const std::filesystem::path& file) {
    const auto size = std::filesystem::file_size(file);
    if (size > max_bytes)
        throw std::runtime_error("development file exceeds 64 MiB: " + file.string());
    std::ifstream stream(file, std::ios::binary);
    std::string value(static_cast<std::size_t>(size), '\0');
    if (!stream.read(value.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot snapshot " + file.string());
    return value;
}
std::string unhex(std::string_view value) {
    if (value.size() % 2 || value.size() > max_bytes * 2)
        throw std::runtime_error("invalid binary reload edit");
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int a = digit(value[i]), b = digit(value[i + 1]);
        if (a < 0 || b < 0)
            throw std::runtime_error("invalid reload hex");
        bytes.push_back(static_cast<char>(a * 16 + b));
    }
    return bytes;
}
} // namespace
DevelopmentRevision DevelopmentSession::revision(MemoryContent::Files files) {
    if (files.size() > max_files)
        throw std::runtime_error("development snapshot exceeds 4096 files");
    std::size_t bytes = 0;
    std::string inventory;
    for (const auto& [path, data] : files) {
        if (!relative_path(path))
            throw std::runtime_error("invalid development path: " + path);
        bytes += data.size();
        if (bytes > max_bytes)
            throw std::runtime_error("development snapshot exceeds 64 MiB");
        inventory += path + '\0' + std::to_string(data.size()) + '\0' + digest(data) + '\n';
    }
    return {MemoryContent::create(std::move(files)), digest(inventory)};
}
DevelopmentRevision DevelopmentSession::capture() const {
    if (!std::filesystem::is_directory(_source))
        throw std::runtime_error("development requires a loose content directory");
    const auto publishing = _source / ".kin-import-publishing";
    if (std::filesystem::exists(publishing))
        throw std::runtime_error("asset import publication is pending");
    const auto report = _source / ".kin-import-report.json";
    const auto before = std::filesystem::exists(report) ? read(report) : std::string{};
    MemoryContent::Files files;
    std::size_t bytes = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(_source);
         it != std::filesystem::recursive_directory_iterator{}; ++it) {
        const auto name = it->path().lexically_relative(_source).generic_string();
        if (!relative_path(name)) {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_symlink())
            throw std::runtime_error("development content cannot contain symlinks: " + name);
        if (it->is_regular_file()) {
            if (files.size() >= max_files)
                throw std::runtime_error("too many development files");
            auto data = read(it->path());
            bytes += data.size();
            if (bytes > max_bytes)
                throw std::runtime_error("development snapshot exceeds 64 MiB");
            files.emplace(name, std::move(data));
        }
    }
    if (std::filesystem::exists(publishing) ||
        (std::filesystem::exists(report) ? read(report) : std::string{}) != before)
        throw std::runtime_error("asset import changed during snapshot; retry after publication");
    return revision(std::move(files));
}
DevelopmentSession::DevelopmentSession(std::filesystem::path source)
    : _source(std::filesystem::absolute(source).lexically_normal()), _active(capture()), _disk_seen(_active.id) {}
void DevelopmentSession::add_target(std::string name, Prepare prepare) {
    if (name.empty() || !prepare ||
        std::ranges::any_of(_targets, [&](const auto& target) { return target.first == name; }))
        throw std::invalid_argument("duplicate/empty development target");
    _targets.emplace_back(std::move(name), std::move(prepare));
}
bool DevelopmentSession::request(std::string& error) {
    if (pending()) {
        error = "a development reload is already pending";
        return false;
    }
    try {
        _pending = capture();
        _disk_seen = _pending.id;
        _status = "queued";
        _error.clear();
        return true;
    } catch (const std::exception& e) {
        error = _error = e.what();
        _status = "rejected";
        return false;
    }
}
bool DevelopmentSession::request(const JsonValue& changes, std::string& error) {
    if (pending()) {
        error = "a development reload is already pending";
        return false;
    }
    try {
        if (!changes.is_object())
            throw std::runtime_error("reload changes must be an object");
        auto files = _active.content->files();
        for (const auto& [name, value] : changes.members()) {
            if (!relative_path(name))
                throw std::runtime_error("reload path must be a normalized relative content path");
            if (value.is_null())
                files.erase(name);
            else if (value.is_string())
                files[name] = value.as_string();
            else if (value.is_object() && value.members().size() == 1 && value.find("hex") &&
                     value.find("hex")->is_string())
                files[name] = unhex(value.string_at("hex"));
            else
                throw std::runtime_error("reload edit must be text, {hex:string}, or null");
        }
        _pending = revision(std::move(files));
        _status = "queued";
        _error.clear();
        return true;
    } catch (const std::exception& e) {
        error = _error = e.what();
        _status = "rejected";
        return false;
    }
}
void DevelopmentSession::apply(u64 tick) {
    ++_attempt;
    _last_attempted = _pending.id;
    std::vector<std::unique_ptr<DevelopmentChange>> candidates;
    std::size_t activated = 0;
    try {
        for (const auto& [name, prepare] : _targets) {
            std::string error;
            auto candidate = prepare(_pending, error);
            if (!candidate || !candidate->activate || !candidate->rollback)
                throw std::runtime_error(name + ": " + (error.empty() ? "invalid reload candidate" : error));
            candidates.push_back(std::move(candidate));
        }
        for (auto& candidate : candidates) {
            ++activated;
            std::string error;
            if (!candidate->activate(error))
                throw std::runtime_error(error.empty() ? "reload activation failed" : error);
        }
    } catch (const std::exception& e) {
        while (activated) {
            try {
                candidates[--activated]->rollback();
            } catch (...) {
                std::terminate();
            }
        }
        _error = e.what();
        const auto root = _pending.content->root().string();
        std::size_t offset = 0;
        while ((offset = _error.find(root, offset)) != std::string::npos) {
            _error.replace(offset, root.size(), _source.string());
            offset += _source.string().size();
        }
        _pending = {};
        _status = "rejected";
        KIN_LOG_WARN("development", _error);
        return;
    }
    for (auto& candidate : candidates)
        if (candidate->finalize) {
            try {
                candidate->finalize();
            } catch (...) {
                std::terminate();
            }
        }
    _active = std::move(_pending);
    _pending = {};
    ++_generation;
    _activation_tick = tick;
    _status = "applied";
    _error.clear();
    KIN_LOG_INFO("development", "activated content revision " + _active.id);
}
void DevelopmentSession::advance(u64 tick, bool watch_disk) {
    if (pending()) {
        apply(tick);
        return;
    }
    if (!watch_disk)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - _last_poll < std::chrono::milliseconds(150))
        return;
    _last_poll = now;
    try {
        auto candidate = capture();
        if (candidate.id == _disk_seen)
            return;
        if (candidate.id == _active.id) {
            _observed.clear();
            _disk_seen = candidate.id;
            _status = "idle";
            _error.clear();
            return;
        }
        if (candidate.id == _last_attempted) {
            _disk_seen = candidate.id;
            return;
        }
        if (candidate.id != _observed) {
            _observed = candidate.id;
            return;
        }
        _observed.clear();
        _disk_seen = candidate.id;
        _pending = std::move(candidate);
        apply(tick);
    } catch (const std::exception& e) {
        _error = e.what();
        _status = "rejected";
    }
}
void DevelopmentSession::write_status(JsonWriter& json) const {
    json.begin_object()
        .field("generation", _generation)
        .field("attempt", _attempt)
        .field("pending", pending())
        .field("status", _status)
        .field("error", _error)
        .field("revision", _active.id)
        .field("activation_tick", _activation_tick);
    json.key("sources").begin_array();
    for (const auto& [path, bytes] : _active.content->files())
        json.begin_object()
            .field("path", path)
            .field("hash", digest(bytes))
            .field("bytes", static_cast<u64>(bytes.size()))
            .end_object();
    json.end_array();
    json.key("targets").begin_array();
    for (const auto& [name, prepare] : _targets)
        json.value(name);
    json.end_array().end_object();
}
JsonValue DevelopmentSession::pending_changes() const {
    JsonValue::Object changes;
    if (!pending())
        return JsonValue{std::move(changes)};
    std::size_t bytes = 0;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto& [path, data] : _pending.content->files()) {
        const auto old = _active.content->files().find(path);
        if (old != _active.content->files().end() && old->second == data)
            continue;
        bytes += data.size();
        if (bytes > 4 * 1024 * 1024)
            throw std::runtime_error("RPC reload edits exceed 4 MiB; use disk development watching for larger changes");
        std::string hex;
        hex.reserve(data.size() * 2);
        for (unsigned char c : data) {
            hex.push_back(digits[c >> 4]);
            hex.push_back(digits[c & 15]);
        }
        changes[path] = JsonValue{JsonValue::Object{{"hex", JsonValue{std::move(hex)}}}};
    }
    for (const auto& [path, data] : _active.content->files())
        if (!_pending.content->files().contains(path))
            changes[path] = JsonValue{};
    return JsonValue{std::move(changes)};
}
} // namespace kin
