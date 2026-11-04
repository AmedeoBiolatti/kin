#pragma once
#include <chrono>
#include <functional>
#include <kin/assets/content.hpp>
#include <kin/core/json_value.hpp>
#include <memory>
#include <string>
#include <vector>
namespace kin {
class JsonWriter;
// Preparation owns candidates without changing live state. Activation and
// rollback run on the owning thread, between simulation steps.
struct DevelopmentChange {
    std::function<bool(std::string&)> activate;
    std::function<void()> rollback;
    std::function<void()> finalize; // no-fail ownership cleanup, after every activation succeeds
};
struct DevelopmentRevision {
    std::shared_ptr<const MemoryContent> content;
    std::string id;
};
class DevelopmentSession {
public:
    using Prepare = std::function<std::unique_ptr<DevelopmentChange>(const DevelopmentRevision&, std::string&)>;
    explicit DevelopmentSession(std::filesystem::path source);
    void add_target(std::string name, Prepare prepare);
    // Relative paths mapped to UTF-8 strings, {hex:...} data, or null (delete).
    bool request(const JsonValue& changes, std::string& error);
    bool request(std::string& error);
    void advance(u64 tick, bool watch_disk = false);
    void write_status(JsonWriter& json) const;
    const DevelopmentRevision& active() const { return _active; }
    const std::filesystem::path& source() const { return _source; }
    u64 generation() const { return _generation; }
    bool pending() const { return static_cast<bool>(_pending.content); }
    const std::string& error() const { return _error; }
    JsonValue pending_changes() const;
    void reject_request(std::string error) {
        _pending = {};
        _status = "rejected";
        _error = std::move(error);
    }

private:
    DevelopmentRevision capture() const;
    static DevelopmentRevision revision(MemoryContent::Files files);
    void apply(u64 tick);
    std::filesystem::path _source;
    DevelopmentRevision _active, _pending;
    std::vector<std::pair<std::string, Prepare>> _targets;
    std::string _error, _status = "idle", _observed, _last_attempted, _disk_seen;
    u64 _generation = 0, _attempt = 0, _activation_tick = 0;
    std::chrono::steady_clock::time_point _last_poll{};
};
} // namespace kin
