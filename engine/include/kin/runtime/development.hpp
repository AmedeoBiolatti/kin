#pragma once

#include <kin/assets/development_revision.hpp>
#include <kin/core/json_value.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kin {

class JsonWriter;

// A game's development session (`--dev`, built with KIN_ENABLE_DEVELOPMENT):
// an immutable snapshot of its loose content folder and the targets (scene and
// assets, Lua, ...) that reload from a new snapshot together, or not at all.
// This is kin's live-reload path; ScriptSceneConfig::hot_reload and
// SceneAssetRuntime::reload_if_changed are the older per-file paths, kept for
// existing games and off while a session runs.
class DevelopmentSession {
public:
    using Prepare = std::function<std::unique_ptr<DevelopmentChange>(const DevelopmentRevision&, std::string&)>;

    explicit DevelopmentSession(std::filesystem::path source);
    void add_target(std::string name, Prepare prepare);
    // Queues the active revision with `changes` applied: relative paths mapped
    // to UTF-8 text, {"hex": "..."} bytes, or null to delete.
    bool request(const JsonValue& changes, std::string& error);
    // Queues the content folder as it is now.
    bool request(std::string& error);
    // Between simulation steps: applies a queued revision at `tick`. With
    // `watch_disk`, also looks at the folder (at most every 150 ms) and applies
    // an edit once its files' sizes and times stayed the same for two looks,
    // as FileWatcher does.
    void advance(u64 tick, bool watch_disk = false);
    // dev.status: the summary, every source file with its hash, and the targets.
    void write_status(JsonWriter& json) const;
    // The summary alone (generation, attempt, pending, status, error, revision,
    // activation tick), as automation snapshots carry it.
    void write_summary(JsonWriter& json) const;
    const DevelopmentRevision& active() const { return _active.revision; }
    u64 generation() const { return _generation; }
    bool pending() const { return static_cast<bool>(_pending.revision.content); }
    const std::string& error() const { return _error; }
    // The queued revision's differences from the active one, as request() takes them.
    JsonValue pending_changes() const;
    void reject_request(std::string error);

private:
    struct Snapshot {
        DevelopmentRevision revision;
        std::vector<std::string> hashes; // each file's, in content order
        std::string stamps;              // the folder's sizes and times when read; empty for edits
    };

    Snapshot capture() const;
    static Snapshot snapshot(MemoryContent::Files files);
    bool queue(const std::function<Snapshot()>& make, std::string& error);
    void apply(u64 tick);
    void watch(u64 tick);
    void write_summary_fields(JsonWriter& json) const;

    std::filesystem::path _source;
    Snapshot _active;
    Snapshot _pending;
    std::vector<std::pair<std::string, Prepare>> _targets;
    std::string _error;
    std::string _status = "idle";
    std::string _last_attempted;  // the revision id of the last apply()
    std::string _disk_seen;       // the revision id the folder was last read as
    std::string _settled_stamps;  // the folder as last read
    std::string _changing_stamps; // the folder as last looked at, while an edit settles
    u64 _generation = 0;
    u64 _attempt = 0;
    u64 _activation_tick = 0;
    std::chrono::steady_clock::time_point _last_poll{};
};

} // namespace kin
